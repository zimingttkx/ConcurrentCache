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


def strip_hash_comments(text: str) -> str:
    """去掉 # 之后的内容。不剥掉的话，"解释某文件为何不接入构建"的注释本身
    会让那个文件被判定为已被引用——这个坑是实跑时暴露出来的。
    CMake 和 GitHub 的 YAML 都用 # 起注释，两边共用这一个。"""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def load_workflow_text() -> str:
    """把所有 workflow 拼成一份**已经去掉注释**的正文。

    4c) 拿它判断某个 e2e 脚本有没有真的被 job 跑起来。在注释里写一句
    "以后可以接 e2e_chaos_test.py" 并不构成接线，但如果用的是没剥注释的原文，
    这句注释就会让那个脚本被判成"已被引用"——判据想防的正好是这种事，所以
    必须先剥注释再匹配。
    """
    text = ""
    for wf in sorted((ROOT / ".github" / "workflows").glob("*.yml")):
        text += strip_hash_comments(read(wf))
    return text


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def md_texts() -> dict[Path, str]:
    """仓库里全部 markdown，统一折成 \n 再比对（有些 .md 是 CRLF）。"""
    out: dict[Path, str] = {}
    for pattern in ("*.md", "docs/**/*.md", "src/**/*.md", "test/**/*.md"):
        for path in ROOT.glob(pattern):
            if path.is_file() and "build" not in path.parts:
                out[path] = read(path).replace("\r\n", "\n")
    return out


def api_index_commands(api_text: str) -> set[str]:
    """docs/api.md §2 索引表里承诺的命令名。

    只认第三个单元格里以大写 ASCII 开头的 token：`CLUSTER（含 10 个子命令）` 取到
    CLUSTER，`[§ 4 字符串](#4-字符串-string)` 这种链接单元格里没有候选，表头与
    `|---|` 分隔行也不会误收。
    """
    if "## 2. 命令索引" not in api_text:
        raise SystemExit("docs/api.md 里找不到 §2 命令索引，检查章节标题是否被改")
    seg = api_text.split("## 2. 命令索引", 1)[1].split("## 3.", 1)[0]
    names: set[str] = set()
    for line in seg.splitlines():
        s = line.strip()
        if not s.startswith("|"):
            continue
        cells = [c.strip() for c in s.strip("|").split("|")]
        if len(cells) < 3:
            continue
        for token in re.split(r"\s*/\s*", cells[2]):
            m = re.match(r"([A-Z][A-Z0-9_]*)", token)
            if m:
                names.add(m.group(1).lower())
    return names


def ctest_table_names(readme: str) -> tuple[set[str], int | None]:
    """README § 单独测试 那张表第一列的 ctest 用例名，以及正文声明的条数。

    第一格必须正好是一个标识符才算数据行：表头写的是 `` `ctest` 用例名 ``，分隔行是
    `|------|`，两样都不会被收进来。
    """
    if "### 单独测试" not in readme:
        raise SystemExit("README.md 里找不到 § 单独测试，检查标题是否被改")
    seg = readme.split("### 单独测试", 1)[1].split("\n### ", 1)[0]
    names = set(re.findall(r"^\|\s*([A-Za-z][A-Za-z0-9_]*)\s*\|", seg, re.MULTILINE))
    stated = re.search(r"共\s*(\d+)\s*条", seg)
    return names, (int(stated.group(1)) if stated else None)


def readme_command_tokens(readme: str) -> set[str]:
    """README § 支持的命令 各表第一格里的命令名（只取每格开头那个标识符）。

    只扫这一节，不扫全文：全文扫会把版本表的 V1.0（截成 V1）、模块表的 ThreadCache
    之类当成命令名。取「每格第一个 token」而不是「全部 token」，是因为
    CLUSTER MEET/NODES/INFO 那一格里 NODES/ADDSLOTS 是子命令、不在注册表里，
    全取会假红。
    """
    if "## 支持的命令" not in readme:
        raise SystemExit("README.md 里找不到 § 支持的命令，检查标题是否被改")
    sec = readme.split("## 支持的命令", 1)[1].split("\n## ", 1)[0]
    toks: set[str] = set()
    for line in sec.splitlines():
        t = line.strip()
        if not (t.startswith("|") and t.endswith("|")):
            continue
        first = [c.strip() for c in t.strip("|").split("|")][0]
        m = re.match(r"([A-Z][A-Z0-9_]*)\b", first)
        if m:
            toks.add(m.group(1).lower())
    return toks


def deployment_config_rows(text: str) -> dict[str, str]:
    """docs/deployment.md § 4 配置项 的表格：key -> 整行原文。"""
    if "## 4. 配置项" not in text:
        raise SystemExit("docs/deployment.md 里找不到 § 4 配置项，检查标题是否被改")
    tbl = text.split("## 4. 配置项", 1)[1].split("\n## ", 1)[0]
    rows: dict[str, str] = {}
    for line in tbl.splitlines():
        m = re.match(r"^\|\s*`([a-z0-9_]+)`", line.strip())
        if m:
            rows[m.group(1)] = line
    return rows


def check_docs(docs: dict[Path, str], registered: set[str], registry_names: set[str],
               conf_port: int, tracker: "Tracker") -> None:
    """把"文档写的"和"代码/构建/配置里真的有的"当成两张表来对齐。

    这一组判据存在的理由：文档一旦靠人工复查维持一致，下一次加命令、改端口、重命名
    ctest target 就会再脱钩，而且脱钩后不会有任何东西变红。每条都只比"可数的量"
    （名字集合、计数、端口、版本号、链接能不能解析），不比措辞。
    """
    api_path = ROOT / "docs" / "api.md"
    readme_path = ROOT / "README.md"
    if api_path not in docs or readme_path not in docs:
        tracker.errors.append("docs/api.md 或 README.md 读不到，文档判据整体失效")
        return
    api_text, readme = docs[api_path], docs[readme_path]

    # 7a) 命令索引 ↔ 注册表（名字集合 + 声明的总数）。
    documented = api_index_commands(api_text)
    missing = registered - documented
    extra = documented - registered
    if missing or extra:
        tracker.errors.append(
            f"docs/api.md §2 命令索引与注册表不一致：文档缺 {sorted(missing)}，"
            f"文档多写了但没注册的 {sorted(extra)}"
        )
    declared = re.search(r"\*\*命令总数\*\*：(\d+) 个", api_text)
    if not declared:
        tracker.errors.append("docs/api.md 头部的『命令总数：NN 个』被改没了，判据无法核对")
    elif int(declared.group(1)) != len(registered):
        tracker.errors.append(
            f"docs/api.md 声明 {declared.group(1)} 个命令，注册表实际 {len(registered)} 个"
        )

    # 7b) README 的 ctest 表 ↔ test/CMakeLists.txt 的 CC_TESTS。
    table_names, stated_count = ctest_table_names(readme)
    if table_names != registry_names:
        tracker.errors.append(
            f"README.md § 单独测试 的 ctest 名单与注册表不一致：文档缺 "
            f"{sorted(registry_names - table_names)}，文档多写了不存在的 "
            f"{sorted(table_names - registry_names)}"
        )
    if stated_count is None:
        tracker.errors.append("README.md 的 ctest 表上方少了『共 N 条』这句可核对的计数")
    elif stated_count != len(registry_names):
        tracker.errors.append(
            f"README.md 声称 ctest 用例共 {stated_count} 条，CC_TESTS 实际 {len(registry_names)} 条"
        )

    # 7c) 相对链接必须能解析（只查指向 .md 的链接，避开正文里形似链接的代码片段）。
    for path, text in sorted(docs.items()):
        for m in re.finditer(r"\[[^\]]*\]\(([^)\s]+)\)", text):
            target = m.group(1).split("#", 1)[0]
            if not target.endswith(".md"):
                continue
            if not (path.parent / target).resolve().exists():
                tracker.errors.append(
                    f"{path.relative_to(ROOT)} 里的相对链接断了：{m.group(1)}"
                )

    # 7d) 端口口径。文档里"默认端口"必须是 conf 的那个值；而总线端口
    # （客户端端口 + 10000）不是客户端口，谁在 redis-cli 里用它，谁就是搞混了
    # ——这正是本轮修掉的那类错误，所以拿它当判据而不是拿"端口必须等于 6379"
    # （多节点示例里的 6380/6381 是合法的）。
    bus_port = conf_port + 10000
    for path, text in sorted(docs.items()):
        for m in re.finditer(r'redis-cli[ "\',]+-p[ "\',]+"?(\d+)', text):
            if int(m.group(1)) == bus_port:
                tracker.errors.append(
                    f"{path.relative_to(ROOT)} 用总线端口 {bus_port} 当客户端口发了 "
                    f"redis-cli -p（客户端默认端口是 {conf_port}）"
                )
    for path, text in sorted(docs.items()):
        for m in re.finditer(r"\*\*默认端口\*\*：`(\d+)`", text):
            if int(m.group(1)) != conf_port:
                tracker.errors.append(
                    f"{path.relative_to(ROOT)} 写的默认端口 {m.group(1)} 与 conf 的 "
                    f"{conf_port} 不一致"
                )
    for m in re.finditer(r"默认监听\s*\S*?:(\d+)", readme):
        if int(m.group(1)) != conf_port:
            tracker.errors.append(f"README.md 写的默认监听端口 {m.group(1)} 与 conf 不一致")

    # 7e) INFO 报的版本号必须等于 api.md 示例里的版本号。
    src = read(ROOT / "src" / "command" / "string_cmd.h").replace("\r\n", "\n")
    code_ver = re.search(r'concurrentcache_version:([0-9][0-9.]*)', src)
    doc_ver = re.search(r'concurrentcache_version:([0-9][0-9.]*)', api_text)
    if not code_ver or not doc_ver:
        tracker.errors.append("找不到 concurrentcache_version 的其中一处（代码或 api.md），判据失效")
    elif code_ver.group(1) != doc_ver.group(1):
        tracker.errors.append(
            f"INFO 实际输出 concurrentcache_version:{code_ver.group(1)}，"
            f"docs/api.md 示例写的是 {doc_ver.group(1)}"
        )

    # 7f) 文档里出现的镜像仓库地址，必须有工作流真的往那儿推。
    workflow_text = load_workflow_text()
    for path, text in sorted(docs.items()):
        for m in re.finditer(r"(ghcr\.io|docker\.io)/([A-Za-z0-9_./-]+)", text):
            if m.group(1) not in workflow_text:
                tracker.errors.append(
                    f"{path.relative_to(ROOT)} 让人拉 {m.group(0)}，但没有任何 workflow "
                    "往这个仓库推镜像（不存在这个可拉取的镜像）"
                )


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


# 8) "只有声明、没有定义也没有调用点"的成员函数。
#
# join_all 就是这么一个：SubReactorPool::join_all() 从来没有定义，谁调一次就是链接
# 错误，而现在没人调所以永远发现不了。这类声明不会让编译变红，只会让代码库看起来
# 承诺了它其实没有的能力 —— 和文档写了一个不存在的命令是同一类错误。
#
# 判据刻意保守，宁可漏报不误报：一个方法名在全树（src/ 的所有 .h/.hpp/.cpp 加
# main.cpp，剥掉注释后）里**只出现一次**（就是那条声明本身）才算。定义过它会出现
# `Class::name(`、调用它会出现 `.name(` / `->name(`，两者都会让计数 >= 2。
# 两个不同类用同名方法也会互相把对方算进去 —— 那是有意的漏报方向。
_NON_FUNCTION_WORDS = {
    "if", "for", "while", "switch", "return", "alignas", "alignof", "decltype",
    "noexcept", "static_cast", "reinterpret_cast", "dynamic_cast", "const_cast",
    "explicit", "operator", "sizeof", "throw", "new", "delete", "requires",
}


def class_bodies(text: str):
    """yield (class_name, body_text)，花括号配平，不用正则猜类边界。"""
    for m in re.finditer(r"\bclass\s+([A-Za-z0-9_]+)", text):
        name = m.group(1)
        start = text.find("{", m.end())
        if start < 0:
            continue
        depth = 0
        for i in range(start, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    yield name, text[start + 1:i]
                    break


def check_declared_only_methods(tracker: "Tracker") -> None:
    header_exts = {".h", ".hpp"}
    files: list[Path] = []
    for path in (ROOT / "src").rglob("*"):
        if path.is_file() and path.suffix in (header_exts | {".cpp"}):
            files.append(path)
    if (ROOT / "main.cpp").exists():
        files.append(ROOT / "main.cpp")

    stripped = {p: strip_hash_free(read(p)) for p in files}
    all_text = "\n".join(stripped.values())

    for path, text in stripped.items():
        if path.suffix not in header_exts:
            continue
        for cname, body in class_bodies(text):
            for seg in body.split(";"):
                # 声明行：有 ( 但没有 { / =（= 会把 inline 定义、纯虚、=delete、默认实参都排除）
                if "(" not in seg or "{" in seg or "}" in seg or "=" in seg:
                    continue
                names = re.findall(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", seg)
                if not names:
                    continue
                mname = names[-1]
                if mname == cname or mname.startswith("~") or mname in _NON_FUNCTION_WORDS:
                    continue
                if len(re.findall(r"\b" + re.escape(mname) + r"\b", all_text)) <= 1:
                    tracker.errors.append(
                        f"{path.relative_to(ROOT)}: {cname}::{mname}() 只有声明，"
                        "全树没有任何定义或调用点（要么补实现，要么删掉这条声明）"
                    )


def strip_hash_free(text: str) -> str:
    """剥掉 // 与 /* */ 注释。注释里写的名字会让"出现次数 >= 2"成立，从而漏报，
    所以这一步必须在计数之前做。"""
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def main() -> int:
    docs_only = "--docs-only" in sys.argv[1:]
    positional = [a for a in sys.argv[1:] if not a.startswith("--")]
    probe = Path(positional[0]) if positional else ROOT / "build" / "test" / "command-table-probe"
    if not docs_only and not probe.exists():
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
    if docs_only:
        # 本地（例如没有 POSIX 构建的 Windows 机器）跑文档判据时的退路：注册表取
        # 源码正则的解析结果。CI 不走这条路 —— 那边 1) 会先把正则与探针的运行时
        # 答案对齐，两者不一致就直接判红。
        registered = parsed_registered
        print("::notice::--docs-only：不调用探针，注册表用源码正则解析结果")
    else:
        registered = probe_registered(candidates, probe)

    # 1) 正则解析与运行时必须一致，否则后面所有判断都建立在错误的名单上。
    only_in_source = parsed_registered - registered
    only_at_runtime = registered - parsed_registered
    if not docs_only and (only_in_source or only_at_runtime):
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
    cmake_text = strip_hash_comments(read(ROOT / "test" / "CMakeLists.txt"))
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
    # 4c) e2e 的 .py 也要"被某个 job 跑起来"。
    #
    # 4) 只 rglob *.cpp，所以 test/e2e_test 下那批 Python 用例可以整年不执行而
    # 没人发现——和 4b) 修的是同一类腐烂，只是换了语言。判据：test/e2e_test/*.py
    # 里出现的脚本要么被某个 workflow 步骤引用，要么登记进 legacy-file 那条棘轮
    # （登记 = 公开承认"这个文件没接线"，而不是让它悄悄躺着）。
    workflow_text = load_workflow_text()
    for path in sorted((ROOT / "test" / "e2e_test").glob("*.py")):
        if path.name in workflow_text:
            continue
        tracker.allowed(
            "legacy-file",
            f"test/e2e_test/{path.name}",
            f"test/e2e_test/{path.name} 没有被任何 workflow 步骤引用：它不会被执行，"
            "也就永远不会腐烂或失败",
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

    # 7) 文档 ↔ 现实：把"文档没有任何错误"从一次性人工清扫变成每次 PR 都要过的检查。
    registry_names = {name for name, _tgt, _labels, _timeout in registry}
    conf_port_value = int(conf_port.group(1)) if conf_port else 0
    if not conf_port:
        tracker.errors.append("conf/concurrentcache.conf 里读不到 port，文档端口判据无法执行")
    else:
        check_docs(md_texts(), registered, registry_names, conf_port_value, tracker)

    # 8) 只声明未实现的方法（join_all 那一类）。
    check_declared_only_methods(tracker)

    # 7g) README 的命令表必须是注册表的子集。
    #
    # 只做单向的理由写在 readme_command_tokens 的注释里（一行里并排两个命令、
    # CLUSTER 那格是子命令），覆盖率由 7a) 的 api.md 精确等值负责。这条管的是
    # 「文档不许写根本不存在的命令」—— #94 修掉的多参数形式就属于这一类。
    docs_now = md_texts()
    readme_tokens = readme_command_tokens(docs_now[ROOT / "README.md"])
    ghost = readme_tokens - registered
    if ghost:
        tracker.errors.append(
            f"README.md § 支持的命令 写了注册表里不存在的命令：{sorted(ghost)}"
        )

    # 7h) 配置表两向核对：文档不许写代码不认的键，conf 里的键也不许漏文档。
    #
    # 已有的 5) 只管 conf -> 代码（配了不生效），这一条补代码/conf -> 文档，并把
    # 「代码确实不读」与「文档诚实写明未被读取」区分开：后者是诚实，前者才是错。
    dep_text = docs_now[ROOT / "docs" / "deployment.md"]
    rows = deployment_config_rows(dep_text)
    for key, row in sorted(rows.items()):
        read_by_code = re.search(rf'get(?:Int|String|Bool)\(\s*"{key}"', code_text)
        marked_unwired = ("未被读取" in row) or ("未读取" in row)
        if not read_by_code and not marked_unwired:
            tracker.errors.append(
                f"docs/deployment.md § 4 列了配置项 {key}，但代码里没人读它，"
                "行里也没写明「当前未被读取」"
            )
    for key in sorted(conf_keys - set(rows)):
        tracker.errors.append(
            f"conf/concurrentcache.conf 里有 {key}，但 docs/deployment.md § 4 配置项没写它"
        )

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
