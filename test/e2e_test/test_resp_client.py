#!/usr/bin/env python3
"""RespClient 回复配对的单元测试：不连服务器，只用假 reader 喂字节。

为什么要有这个文件：`comparison_test.py` 的"大量 key 写入"两栏曾经恒红，根因不在
被测的服务器，也不在 Redis，而在量具自己 —— `execute()` 每条命令 `read(65536)` 一次
并把 `parse_line` 返回的剩余字节直接丢掉。TCP 是字节流，一次 read 完全可以带回 3 条
回复，于是后 2 条永久丢失，之后每条命令读到的都是上一条的回复：配对全错，成功率
自然上不去。真 Redis 也一样红 —— 而这正是"永远红的行等于没有行"。

e2y/*.py 不被任何 required check 覆盖（daily 才有），所以量具坏掉在 CI 上表现为
"某一行红"，很容易被当成被测对象的问题。这几条断言直接钉住配对逻辑本身。
"""
import asyncio
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, 'comparison_test.py')

spec = importlib.util.spec_from_file_location("comparison_test", TARGET)
module = importlib.util.module_from_spec(spec)
sys.modules["comparison_test"] = module
spec.loader.exec_module(module)


class FakeReader:
    """按预设分块返回字节；耗尽后返回 b''（等价于对端关闭）。"""

    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.pos = 0

    async def read(self, _n):
        if self.pos >= len(self.chunks):
            return b''
        chunk = self.chunks[self.pos]
        self.pos += 1
        return chunk


async def collect(chunks, want):
    client = module.RespClient(port=1)
    client.reader = FakeReader(chunks)
    got = [await client.read_reply(timeout=2.0) for _ in range(want)]
    return got


async def main():
    failures = []

    def check(name, got, expect):
        if got != expect:
            failures.append("%s: got=%r expect=%r" % (name, got, expect))
            print("FAIL %s got=%r expect=%r" % (name, got, expect))
        else:
            print("PASS %s" % name)

    # 一次 read 带回 3 条：旧实现会丢掉后 2 条
    check('coalesced-three-replies',
          await collect([b'+OK\r\n+OK\r\n$3\r\nabc\r\n'], 3),
          ['OK', 'OK', 'abc'])

    # 一条回复被拆成两半分两次到达：必须等齐再解析
    check('split-reply',
          await collect([b'+O', b'K\r\n:7\r\n'], 2),
          ['OK', '7'])

    # 顺序不能错位（第 2 条必须读到 :1 而不是 :2）
    check('ordered-fanout',
          await collect([b'+OK\r\n:1\r\n:2\r\n:3\r\n'], 4),
          ['OK', '1', '2', '3'])

    # 只剩 1 条而对端关闭：返回 None，不能卡死或抛异常
    check('eof-returns-none',
          await collect([b'+OK\r\n'], 2),
          ['OK', None])

    # nil 回复（$-1）之后必须还能继续读后面的回复：曾经因为 nil 也映射成 Python
    # None 就不推进缓冲区，一条 nil 把整条连接永久卡死 —— 这正是对照测试里
    # "HDEL 删除字段"两栏一起红、而真 Redis 也红的根因（量具坏了，不是被测对象坏了）。
    check('nil-then-more-replies',
          await collect([b':1\r\n$-1\r\n$2\r\nv2\r\n+PONG\r\n'], 4),
          ['1', None, 'v2', 'PONG'])

    # nil 单独到达后再也没有字节：仍然只能返回 None，且不能把 EOF 当成 nil 的替身
    check('nil-at-eof',
          await collect([b'$-1\r\n'], 2),
          [None, None])

    # 错误回复也要按条取回，不能被当成 OK
    check('error-reply',
          await collect([b'-ERR wrong number of arguments\r\n+OK\r\n'], 2),
          ['ERR:ERR wrong number of arguments', 'OK'])

    return failures


if __name__ == "__main__":
    problems = asyncio.run(main())
    if problems:
        print("\nRespClient 回复配对检查失败 %d 项" % len(problems), file=sys.stderr)
        sys.exit(1)
    print("\nRespClient 回复配对检查通过")
