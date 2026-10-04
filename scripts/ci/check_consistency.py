#!/usr/bin/env python3
"""CI 一致性检查：把"两张表脱钩""测试文件烂在角落""配了不生效"变成会红的检查。

设计取向写在注释里。一句话版本：注册表由运行时自己回答（command-table-probe），
脚本不信任自己对 C++ 源码的正则；基线名单只允许变短。
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# 既不复制也不重定向的管理/内部命令：它们本来就不该出现在复制写白名单里。
OPS_COMMANDS = {"cluster", "debug", "psync", "sync", "replconf", "save", "bgsave"}

# 只读命令。新增命令必须在这里或写白名单里显式出现，否则脚本判红——
# 这正是目的：让"忘了分类"变成一次 CI 失败而不是一次线上主从发散。
READ_COMMANDS = {
    "get", "exists", "ttl", "pttl", "llen", "lrange", "hget", "hlen", "hgetall",
    "scard", "sismember", "smembers", "zscore", "zcard", "zrange", "dbsize",
    "info", "lastsave", "ping",
}


def strip_line_comments(text: str) -> str:
    """去掉 // 之后的内容，避免文档注释里的示例代码被当成真调用。"""
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


def strip_cmake_comments(text: str) -> str:
    """去掉 # 之后的内容。不剥掉的话，"解释某文件为何不接入构建"的注释本身
    会让那个文件被判定为已被引用——这个坑是实跑时暴露出来的。"""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def load_baseline() -> dict[str, list[tuple[str, str]]]:
    """kind -> [(name, note)]；同时记录哪些条目真的被触发过。"""
    kinds: dict[str, list[tuple[str, str]]] = {}
    path = ROOT / "ci" / "known-failures.txt"
    if not path.exists():
        return kinds
    for raw in read(path).splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("|", 2)
        if len(parts) < 2:
            raise SystemExit(f"ci/known-failures.txt 格式错误: {raw!r}")
        kinds.setdefault(parts[0].strip(), []).append(
            (parts[1].strip(), (parts[2].strip() if len(parts) > 2 else ""))
        )
    return kinds


class Tracker:
    """把基线名单的'用过/没用过'记下来，结尾据此判红。"""

    def __init__(self, baseline: dict[str, list[tuple[str, str]]]) -> None:
        self.baseline = baseline
        self.used: set[tuple[str, str]] = set()
        self.errors: list[str] = []
        self.warns: list[str] = []

    def allowed(self, kind: str, name: str, problem: str) -> None:
        if any(n == name for n, _ in self.baseline.get(kind, [])):
            self.used.add((kind, name))
            self.warns.append(f"[已知缺口 {kind}:{name}] {problem}")
            return
        self.errors.append(problem)

    def stale(self) -> None:
        for kind, entries in self.baseline.items():
            for name, note in entries:
                if (kind, name) not in self.used:
                    self.errors.append(
                        f"基线名单里的 {kind}:{name} 已经不再触发（{note}），"
                        "请删掉这一行——名单只允许变短。"
                    )


def probe_registered(names: list[str], binary: Path) -> set[str]:
    """权威答案：哪些命令名真的能被 CommandFactory::create() 造出来。"""
    out = subprocess.run([str(binary), *names], check=True, capture_output=True, text=True).stdout
    registered = set()
    for line in out.splitlines():
        name, _, state = line.partition(" ")
        if state.strip() == "registered":
            registered.add(name)
    return registered


def main() -> int:
    probe = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "test" / "command-table-probe"
    if not probe.exists():
        print(f"找不到命令表探针 {probe}（先构建 command-table-probe）", file=sys.stderr)
        return 2

    tracker = Tracker(load_baseline())

    factory_src = strip_line_comments(read(ROOT / "src" / "command" / "command_factory.cpp"))
    parsed_registered = set(re.findall(r'register_command\("([a-z0-9_]+)"', factory_src))

    reactor_src = read(ROOT / "src" / "network" / "sub_reactor.cpp")
    block = re.search(r"bool is_write_command\s*=\s*\(([^;]*?)\);", reactor_src, re.DOTALL)
    if not block:
        tracker.errors.append("无法在 src/network/sub_reactor.cpp 定位写命令白名单，检查是否被改写")
        whitelist: set[str] = set()
    else:
        whitelist = set(re.findall(r'cmd_name\s*==\s*"([a-z0-9_]+)"', block.group(1)))

    candidates = sorted(parsed_registered | whitelist | READ_COMMANDS | OPS_COMMANDS)
    registered = probe_registered(candidates, probe)

    # 1) 正则解析与运行时必须一致，否则后面所有判断都建立在错误的名单上。
    only_in_source = parsed_registered - registered
    only_at_runtime = registered - parsed_registered
    if only_in_source or only_at_runtime:
        tracker.errors.append(
            f"注册表解析与运行时不一致：源码多 {sorted(only_in_source)}，运行时多 {sorted(only_at_runtime)}"
        )

    # 2) 写白名单里的命令必须真的注册（否则是死条目，副本保护形同虚设）。
    for name in sorted(whitelist - registered):
        tracker.allowed(
            "gap",
            f"write-command-not-registered:{name}",
            f"复制写白名单含 {name}，但 CommandFactory 没有注册它",
        )

    # 3) 每个已注册命令必须被明确分类：写白名单 / 只读 / 内部管理。
    for name in sorted(registered - whitelist - READ_COMMANDS - OPS_COMMANDS):
        tracker.allowed(
            "gap",
            f"unclassified-command:{name}",
            f"命令 {name} 已注册，但既不在复制写白名单也不在读/管理清单里——"
            "新增命令必须显式分类",
        )

    # 4) 测试文件清单：test/ 下的 .cpp 要么进某个 target，要么进 legacy 名单。
    # CMake 里这些源文件都是相对 test/ 写的（tests.cpp / datatype_test/object_test.cpp），
    # 所以用同一个相对路径比对即可，不做 basename 回退（那会让
    # network_stress_test_main.cpp 顺带把 test_main.cpp 判成已引用）。
    cmake_text = strip_cmake_comments(read(ROOT / "test" / "CMakeLists.txt"))
    for path in sorted((ROOT / "test").rglob("*.cpp")):
        rel = path.relative_to(ROOT / "test").as_posix()
        if rel in cmake_text:
            continue
        tracker.allowed(
            "legacy-file",
            f"test/{rel}",
            f"test/{rel} 不属于任何 target：它不会被编译，也就永远不会腐烂或失败",
        )

    # 4b) 接线判据：源文件出现在 CMake 里只说明它会被编译，不说明有人跑它。
    #
    # 只判 4) 那条子串，有两种漏法：add_executable 写了但忘了登记进 CC_TESTS，
    # 或者 LABELS 拼成一个不在 gate|contract|slow 里的值——两种情况下六个 ctest
    # 步骤（ci.yml 的 -L gate / -L contract、daily.yml 的 -L "gate|contract" 与
    # -L slow）全都选不到它，流水线照样全绿，而这个二进制从来没被执行过。
    built_targets = set(
        re.findall(r"add_executable\(\s*([A-Za-z0-9_.\-]+)", cmake_text)
    )
    registry = re.findall(
        r'"([A-Za-z0-9_]+)\|([A-Za-z0-9_.\-]+)\|([A-Za-z0-9_|]+)\|(\d+)"',
        cmake_text,
    )
    registered_targets = {tgt for _name, tgt, _label, _timeout in registry}
    instrumented = set(
        re.findall(
            r"set\(CC_TEST_TARGETS(.*?)\)", cmake_text, re.DOTALL
        )[0].split() if "set(CC_TEST_TARGETS" in cmake_text else []
    )
    # command-table-probe 不是 ctest 用例，是给本脚本用的探针，不参与门禁选标签。
    PROBE_TARGETS = {"command-table-probe"}
    for target in sorted(built_targets - registered_targets - PROBE_TARGETS):
        tracker.errors.append(
            f"测试 target {target} 会被编译，但没有登记进 CC_TESTS："
            "没有任何 ctest 步骤会跑到它，等于一份不会失败的检查"
        )
    for name, target, labels, _timeout in registry:
        unknown = set(labels.split("|")) - {"gate", "contract", "slow"}
        if unknown:
            tracker.errors.append(
                f"{name} 的 LABELS={labels} 含未知标签 {sorted(unknown)}："
                "三个作业没有一个会选到它"
            )
        if target not in instrumented:
            tracker.errors.append(
                f"{name} ({target}) 没进 CC_TEST_TARGETS：sanitizer 档不给它加插桩，"
                "daily 的 sanitizer 矩阵看不见它"
            )
    # 5) 配置口径：conf 里的键必须被代码读到，否则是"配了不生效"。
    conf_text = read(ROOT / "conf" / "concurrentcache.conf")
    conf_keys = set(re.findall(r"^\s*([a-z0-9_]+)\s*=", conf_text, re.MULTILINE))
    code_text = strip_line_comments(
        read(ROOT / "main.cpp")
        + "\n".join(
            p.read_text(encoding="utf-8", errors="replace")
            for p in (ROOT / "src").rglob("*")
            if p.is_file() and p.suffix in {".cpp", ".h"}
        )
    )
    for key in sorted(conf_keys):
        if not re.search(rf'get(?:Int|String|Bool)\(\s*"{key}"', code_text):
            tracker.allowed("config-warn", key, f"配置项 {key} 被解析但从未被代码读取")

    # 6) 端口一致性：conf / Dockerfile EXPOSE / Dockerfile HEALTHCHECK 必须同一个端口。
    conf_port = re.search(r"^\s*port\s*=\s*(\d+)", conf_text, re.MULTILINE)
    docker_text = read(ROOT / "Dockerfile")
    expose = re.search(r"EXPOSE\s+(\d+)", docker_text)
    health = re.search(r"redis-cli\s+-p\s+(\d+)", docker_text)
    ports = {
        "conf": conf_port.group(1) if conf_port else None,
        "Dockerfile EXPOSE": expose.group(1) if expose else None,
        "Dockerfile HEALTHCHECK": health.group(1) if health else None,
    }
    if len(set(ports.values())) != 1 or None in ports.values():
        tracker.errors.append(f"监听端口不一致：{ports}")

    tracker.stale()

    for message in tracker.warns:
        print(f"::notice::{message}")
    for message in tracker.errors:
        print(f"::error::{message}")

    print(
        f"\n命令表：注册 {len(registered)} / 写白名单 {len(whitelist)} / "
        f"读 {len(READ_COMMANDS)} / 管理 {len(OPS_COMMANDS)}"
    )
    print(f"基线名单已用 {len(tracker.used)} 项，剩余未触发项会在下面报错")

    if tracker.errors:
        print(f"\n一致性检查失败：{len(tracker.errors)} 项", file=sys.stderr)
        return 1
    print("\n一致性检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
