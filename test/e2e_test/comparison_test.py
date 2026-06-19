#!/usr/bin/env python3
"""
ConcurrentCache vs Redis — 深度对比测试
========================================
功能正确性 + 性能基准 + 极限压力 + 内存占用 全方位对比
"""

import asyncio
import subprocess
import time
import os
import sys
import signal
import json
import random
import statistics
import tempfile
import shutil
import struct
from pathlib import Path
from typing import List, Dict, Tuple, Optional
from dataclasses import dataclass, field, asdict
from datetime import datetime


# ═══════════════════════════════════════════════════════════════
# RESP 协议客户端
# ═══════════════════════════════════════════════════════════════

class RESP:
    @staticmethod
    def encode_cmd(*args: str) -> bytes:
        parts = [f"*{len(args)}\r\n"]
        for a in args:
            parts.append(f"${len(a)}\r\n{a}\r\n")
        return "".join(parts).encode()

    @staticmethod
    def parse_line(data: bytes) -> Tuple[Optional[str], bytes]:
        if not data:
            return None, data
        prefix = data[0:1]
        if prefix == b'+':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return data[1:end].decode(), data[end+2:]
        elif prefix == b'-':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return "ERR:" + data[1:end].decode(), data[end+2:]
        elif prefix == b':':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return data[1:end].decode(), data[end+2:]
        elif prefix == b'$':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            try:
                length = int(data[1:end])
            except ValueError:
                return None, data
            if length < 0:
                return None, data[end+2:]
            start = end + 2
            if len(data) < start + length + 2:
                return None, data
            return data[start:start+length].decode(), data[start+length+2:]
        elif prefix == b'*':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            try:
                count = int(data[1:end])
            except ValueError:
                return None, data
            remaining = data[end+2:]
            results = []
            for _ in range(count):
                val, remaining = RESP.parse_line(remaining)
                if val is None:
                    return None, data
                results.append(val)
            return "|".join(results), remaining
        return None, data


class RespClient:
    """异步 RESP 客户端 — 支持短连接和长连接两种模式"""

    def __init__(self, host: str = "127.0.0.1", port: int = 6379):
        self.host = host
        self.port = port
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None

    async def connect(self) -> bool:
        try:
            self.reader, self.writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), timeout=3.0
            )
            return True
        except Exception:
            return False

    async def close(self):
        if self.writer:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except Exception:
                pass
        self.reader = None
        self.writer = None

    async def execute(self, *args: str, timeout: float = 5.0) -> Optional[str]:
        """执行命令，返回解析后的字符串结果"""
        try:
            self.writer.write(RESP.encode_cmd(*args))
            await self.writer.drain()
            raw = await asyncio.wait_for(self.reader.read(65536), timeout=timeout)
            result, _ = RESP.parse_line(raw)
            return result
        except Exception:
            return None

    async def execute_raw(self, *args: str, timeout: float = 5.0) -> Optional[bytes]:
        """执行命令，返回原始字节"""
        try:
            self.writer.write(RESP.encode_cmd(*args))
            await self.writer.drain()
            raw = await asyncio.wait_for(self.reader.read(65536), timeout=timeout)
            return raw
        except Exception:
            return None

    async def execute_short(self, *args: str, timeout: float = 5.0) -> Optional[str]:
        """短连接模式：打开连接 → 执行命令 → 关闭连接"""
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), timeout=3.0
            )
            writer.write(RESP.encode_cmd(*args))
            await writer.drain()
            raw = await asyncio.wait_for(reader.read(65536), timeout=timeout)
            result, _ = RESP.parse_line(raw)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
            return result
        except Exception:
            return None


# ═══════════════════════════════════════════════════════════════
# 测试结果数据结构
# ═══════════════════════════════════════════════════════════════

@dataclass
class FuncTestResult:
    name: str
    cc_result: str = ""
    redis_result: str = ""
    cc_pass: bool = False
    redis_pass: bool = False
    detail: str = ""


@dataclass
class PerfTierResult:
    label: str
    cc_qps: float = 0
    cc_p50_ms: float = 0
    cc_p99_ms: float = 0
    cc_p999_ms: float = 0
    cc_max_ms: float = 0
    cc_err_rate: float = 0
    cc_total_ops: int = 0
    redis_qps: float = 0
    redis_p50_ms: float = 0
    redis_p99_ms: float = 0
    redis_p999_ms: float = 0
    redis_max_ms: float = 0
    redis_err_rate: float = 0
    redis_total_ops: int = 0


@dataclass
class ComparisonReport:
    timestamp: str
    cc_version: str = ""
    redis_version: str = ""
    functional_tests: List[FuncTestResult] = field(default_factory=list)
    perf_tiers: List[PerfTierResult] = field(default_factory=list)
    memory: Dict = field(default_factory=dict)
    summary: Dict = field(default_factory=dict)


# ═══════════════════════════════════════════════════════════════
# 服务器管理
# ═══════════════════════════════════════════════════════════════

class ServerManager:
    def __init__(self):
        self.cc_proc: Optional[subprocess.Popen] = None
        self.redis_proc: Optional[subprocess.Popen] = None
        self.cc_tmpdir: Optional[str] = None
        self.redis_tmpdir: Optional[str] = None
        self.cc_port = 16379
        self.redis_port = 16380

    def start_concurrentcache(self) -> bool:
        project_root = Path(__file__).resolve().parent.parent.parent
        server_bin = project_root / "build" / "concurrentcache-server"
        if not server_bin.exists():
            print(f"  [CC] ERROR: binary not found at {server_bin}")
            return False

        self.cc_tmpdir = tempfile.mkdtemp(prefix="cc_cmp_")
        conf_dir = Path(self.cc_tmpdir) / "conf"
        conf_dir.mkdir()
        conf_file = conf_dir / "concurrentcache.conf"
        conf_file.write_text(f"""port = {self.cc_port}
log_level = 4
rdb_save_interval = 0
cluster_enabled = false
max_entries = 2000000
""")
        (Path(self.cc_tmpdir) / "dump.rdb").touch()

        print(f"  [CC] 启动 ConcurrentCache (端口 {self.cc_port})...")
        env = os.environ.copy()
        self.cc_proc = subprocess.Popen(
            [str(server_bin)],
            cwd=self.cc_tmpdir,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=env,
        )
        time.sleep(2.0)

        # 验证启动
        try:
            result = subprocess.run(
                ["redis-cli", "-p", str(self.cc_port), "PING"],
                capture_output=True, text=True, timeout=5
            )
            if "PONG" in result.stdout:
                print(f"  [CC] ConcurrentCache 启动成功 ✓")
                return True
        except Exception:
            pass
        print(f"  [CC] ConcurrentCache 启动验证失败")
        return False

    def start_redis(self) -> bool:
        self.redis_tmpdir = tempfile.mkdtemp(prefix="redis_cmp_")
        conf_file = Path(self.redis_tmpdir) / "redis.conf"
        conf_file.write_text(f"""port {self.redis_port}
bind 127.0.0.1
daemonize no
loglevel warning
save ""
appendonly no
""")

        print(f"  [Redis] 启动 Redis (端口 {self.redis_port})...")
        self.redis_proc = subprocess.Popen(
            ["redis-server", str(conf_file)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(1.5)

        try:
            result = subprocess.run(
                ["redis-cli", "-p", str(self.redis_port), "PING"],
                capture_output=True, text=True, timeout=5
            )
            if "PONG" in result.stdout:
                print(f"  [Redis] Redis 启动成功 ✓")
                return True
        except Exception:
            pass
        print(f"  [Redis] Redis 启动验证失败")
        return False

    def stop_all(self):
        for name, proc in [("ConcurrentCache", self.cc_proc), ("Redis", self.redis_proc)]:
            if proc:
                print(f"  停止 {name}...")
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        for d in [self.cc_tmpdir, self.redis_tmpdir]:
            if d:
                shutil.rmtree(d, ignore_errors=True)

    def get_redis_version(self) -> str:
        try:
            r = subprocess.run(["redis-server", "--version"], capture_output=True, text=True, timeout=3)
            return r.stdout.strip().split("v=")[1].split()[0] if "v=" in r.stdout else "unknown"
        except Exception:
            return "unknown"

    def get_memory_rss(self, port: int) -> int:
        """通过 redis-cli INFO memory 获取 used_memory_rss"""
        try:
            r = subprocess.run(
                ["redis-cli", "-p", str(port), "INFO", "memory"],
                capture_output=True, text=True, timeout=5
            )
            for line in r.stdout.split("\n"):
                if line.startswith("used_memory_rss:"):
                    return int(line.split(":")[1].strip())
        except Exception:
            pass
        return 0


# ═══════════════════════════════════════════════════════════════
# 第一部分：功能正确性测试
# ═══════════════════════════════════════════════════════════════

class FunctionalTester:
    def __init__(self, cc_port: int, redis_port: int):
        self.cc_port = cc_port
        self.redis_port = redis_port
        self.results: List[FuncTestResult] = []

    async def _test_both(self, name: str, test_fn) -> FuncTestResult:
        """对两个服务器执行相同测试逻辑"""
        r = FuncTestResult(name=name)
        try:
            r.cc_result, r.cc_pass, r.detail = await test_fn(self.cc_port, "CC")
        except Exception as e:
            r.cc_result = f"EXCEPTION: {e}"
            r.cc_pass = False
        try:
            r.redis_result, r.redis_pass, _ = await test_fn(self.redis_port, "Redis")
        except Exception as e:
            r.redis_result = f"EXCEPTION: {e}"
            r.redis_pass = False
        self.results.append(r)
        return r

    async def _flush(self, port: int):
        c = RespClient(port=port)
        if await c.connect():
            await c.execute("FLUSHDB")
            await c.close()

    async def run_all(self):
        print("\n" + "=" * 70)
        print("  第一部分：功能正确性测试")
        print("=" * 70)

        # 清理两个数据库
        await self._flush(self.cc_port)
        await self._flush(self.redis_port)

        # ── String 命令 ──
        print("\n  ── String 命令 ──")

        await self._test_both("SET/GET 基本读写", lambda port, tag: self._test_set_get(port, tag))
        await self._test_both("SET 覆盖已有 key", lambda port, tag: self._test_set_overwrite(port, tag))
        await self._test_both("DEL 删除 key", lambda port, tag: self._test_del(port, tag))
        await self._test_both("DEL 多 key 批量删除", lambda port, tag: self._test_del_multi(port, tag))
        await self._test_both("EXISTS 存在性检查", lambda port, tag: self._test_exists(port, tag))
        await self._test_both("INCR 递增", lambda port, tag: self._test_incr(port, tag))
        await self._test_both("DECR 递减", lambda port, tag: self._test_decr(port, tag))
        await self._test_both("INCR/DECR 非整数 key", lambda port, tag: self._test_incr_nonint(port, tag))
        await self._test_both("SETEX 带过期设置", lambda port, tag: self._test_setex(port, tag))
        await self._test_both("GET 不存在的 key", lambda port, tag: self._test_get_nonexist(port, tag))

        # ── List 命令 ──
        print("\n  ── List 命令 ──")

        await self._test_both("LPUSH/RPUSH 推入", lambda port, tag: self._test_push(port, tag))
        await self._test_both("LPOP/RPOP 弹出", lambda port, tag: self._test_pop(port, tag))
        await self._test_both("LLEN 长度", lambda port, tag: self._test_llen(port, tag))
        await self._test_both("LRANGE 范围查询", lambda port, tag: self._test_lrange(port, tag))
        await self._test_both("LRANGE 负索引", lambda port, tag: self._test_lrange_neg(port, tag))
        await self._test_both("LPOP 空列表", lambda port, tag: self._test_pop_empty(port, tag))

        # ── Hash 命令 ──
        print("\n  ── Hash 命令 ──")

        await self._test_both("HSET/HGET 基本操作", lambda port, tag: self._test_hset_hget(port, tag))
        await self._test_both("HDEL 删除字段", lambda port, tag: self._test_hdel(port, tag))
        await self._test_both("HLEN 字段数", lambda port, tag: self._test_hlen(port, tag))
        await self._test_both("HGETALL 全部字段", lambda port, tag: self._test_hgetall(port, tag))
        await self._test_both("HGET 不存在字段", lambda port, tag: self._test_hget_nonexist(port, tag))

        # ── Set 命令 ──
        print("\n  ── Set 命令 ──")

        await self._test_both("SADD/SMEMBERS 基本操作", lambda port, tag: self._test_sadd_smembers(port, tag))
        await self._test_both("SISMEMBER 成员检查", lambda port, tag: self._test_sismember(port, tag))
        await self._test_both("SCARD 基数", lambda port, tag: self._test_scard(port, tag))
        await self._test_both("SPOP 随机弹出", lambda port, tag: self._test_spop(port, tag))

        # ── ZSet 命令 ──
        print("\n  ── ZSet (有序集合) 命令 ──")

        await self._test_both("ZADD/ZSCORE 基本操作", lambda port, tag: self._test_zadd_zscore(port, tag))
        await self._test_both("ZCARD 基数", lambda port, tag: self._test_zcard(port, tag))
        await self._test_both("ZRANGE 正序范围", lambda port, tag: self._test_zrange(port, tag))
        await self._test_both("ZRANGE WITHSCORES", lambda port, tag: self._test_zrange_withscores(port, tag))

        # ── 过期机制 ──
        print("\n  ── 过期机制 ──")

        await self._test_both("EXPIRE 设置过期", lambda port, tag: self._test_expire(port, tag))
        await self._test_both("TTL 查询剩余时间", lambda port, tag: self._test_ttl(port, tag))
        await self._test_both("PERSIST 移除过期", lambda port, tag: self._test_persist(port, tag))
        await self._test_both("过期后 key 自动删除", lambda port, tag: self._test_expire_auto_del(port, tag))

        # ── 服务器命令 ──
        print("\n  ── 服务器命令 ──")

        await self._test_both("PING", lambda port, tag: self._test_ping(port, tag))
        await self._test_both("DBSIZE", lambda port, tag: self._test_dbsize(port, tag))
        await self._test_both("FLUSHDB", lambda port, tag: self._test_flushdb(port, tag))
        await self._test_both("INFO 命令", lambda port, tag: self._test_info(port, tag))

        # ── 类型转换 ──
        print("\n  ── 类型转换边界 ──")

        await self._test_both("STRING→LIST 自动转换", lambda port, tag: self._test_string_to_list(port, tag))
        await self._test_both("WRONGTYPE 错误", lambda port, tag: self._test_wrongtype(port, tag))

        # 打印功能测试汇总
        self._print_func_summary()

    # ── String 测试实现 ──

    async def _test_set_get(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        r1 = await c.execute("SET", "cmp_str_key", "hello")
        r2 = await c.execute("GET", "cmp_str_key")
        await c.close()
        ok = r1 is not None and "OK" in str(r1) and r2 == "hello"
        return (f"SET={r1}, GET={r2}", ok, "")

    async def _test_set_overwrite(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_ow_key", "v1")
        await c.execute("SET", "cmp_ow_key", "v2")
        r = await c.execute("GET", "cmp_ow_key")
        await c.close()
        ok = r == "v2"
        return (f"GET={r}", ok, "")

    async def _test_del(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_del_key", "x")
        r1 = await c.execute("DEL", "cmp_del_key")
        r2 = await c.execute("GET", "cmp_del_key")
        await c.close()
        ok = r1 == "1" and r2 is None
        return (f"DEL={r1}, GET={r2}", ok, "")

    async def _test_del_multi(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_md1", "a")
        await c.execute("SET", "cmp_md2", "b")
        await c.execute("SET", "cmp_md3", "c")
        r = await c.execute("DEL", "cmp_md1", "cmp_md2", "cmp_md3")
        await c.close()
        ok = r == "3"
        return (f"DEL={r}", ok, "")

    async def _test_exists(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_ex_key", "x")
        r1 = await c.execute("EXISTS", "cmp_ex_key")
        r2 = await c.execute("EXISTS", "cmp_nonexist_xyz")
        await c.close()
        ok = r1 == "1" and r2 == "0"
        return (f"EXISTS(exist)={r1}, EXISTS(none)={r2}", ok, "")

    async def _test_incr(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_counter", "10")
        r1 = await c.execute("INCR", "cmp_counter")
        r2 = await c.execute("INCR", "cmp_counter")
        r3 = await c.execute("GET", "cmp_counter")
        await c.close()
        ok = r1 == "11" and r2 == "12" and r3 == "12"
        return (f"INCRx2={r1},{r2}, GET={r3}", ok, "")

    async def _test_decr(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_dec_counter", "10")
        r1 = await c.execute("DECR", "cmp_dec_counter")
        r2 = await c.execute("DECR", "cmp_dec_counter")
        await c.close()
        ok = r1 == "9" and r2 == "8"
        return (f"DECRx2={r1},{r2}", ok, "")

    async def _test_incr_nonint(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_str_val", "hello")
        r = await c.execute("INCR", "cmp_str_val")
        await c.close()
        ok = r is not None and "ERR" in str(r)
        return (f"INCR on string={r}", ok, "应返回错误")

    async def _test_setex(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        r1 = await c.execute("SETEX", "cmp_setex_key", "10", "val")
        r2 = await c.execute("GET", "cmp_setex_key")
        r3 = await c.execute("TTL", "cmp_setex_key")
        await c.close()
        ok = r1 is not None and "OK" in str(r1) and r2 == "val" and r3 is not None
        return (f"SETEX={r1}, GET={r2}, TTL={r3}", ok, "")

    async def _test_get_nonexist(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        r = await c.execute("GET", "cmp_nonexist_xyzabc")
        await c.close()
        ok = r is None
        return (f"GET={r}", ok, "应返回 null")

    # ── List 测试实现 ──

    async def _test_push(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        r1 = await c.execute("LPUSH", "cmp_list", "a", "b", "c")
        r2 = await c.execute("RPUSH", "cmp_list", "x", "y")
        r3 = await c.execute("LRANGE", "cmp_list", "0", "-1")
        await c.close()
        # LPUSH a,b,c → c,b,a; RPUSH x,y → c,b,a,x,y
        ok = r1 == "3" and r2 == "5" and r3 is not None and "c" in str(r3) and "y" in str(r3)
        return (f"LPUSH={r1}, RPUSH={r2}, LRANGE={r3}", ok, "")

    async def _test_pop(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_poplist")
        await c.execute("RPUSH", "cmp_poplist", "1", "2", "3")
        r1 = await c.execute("LPOP", "cmp_poplist")
        r2 = await c.execute("RPOP", "cmp_poplist")
        r3 = await c.execute("LRANGE", "cmp_poplist", "0", "-1")
        await c.close()
        ok = r1 == "1" and r2 == "3" and r3 == "2"
        return (f"LPOP={r1}, RPOP={r2}, LRANGE={r3}", ok, "")

    async def _test_llen(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_llenlist")
        await c.execute("RPUSH", "cmp_llenlist", "a", "b", "c", "d", "e")
        r = await c.execute("LLEN", "cmp_llenlist")
        await c.close()
        ok = r == "5"
        return (f"LLEN={r}", ok, "")

    async def _test_lrange(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_lrlist")
        await c.execute("RPUSH", "cmp_lrlist", "0", "1", "2", "3", "4", "5")
        r = await c.execute("LRANGE", "cmp_lrlist", "1", "3")
        await c.close()
        ok = r is not None and "1" in str(r) and "2" in str(r) and "3" in str(r)
        return (f"LRANGE 1-3={r}", ok, "")

    async def _test_lrange_neg(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_lrneg")
        await c.execute("RPUSH", "cmp_lrneg", "a", "b", "c", "d", "e")
        r = await c.execute("LRANGE", "cmp_lrneg", "-3", "-1")
        await c.close()
        ok = r is not None and "c" in str(r) and "d" in str(r) and "e" in str(r)
        return (f"LRANGE -3~-1={r}", ok, "")

    async def _test_pop_empty(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_emptylist")
        r = await c.execute("LPOP", "cmp_emptylist")
        await c.close()
        ok = r is None
        return (f"LPOP empty={r}", ok, "应返回 null")

    # ── Hash 测试实现 ──

    async def _test_hset_hget(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_hash")
        r1 = await c.execute("HSET", "cmp_hash", "f1", "v1")
        r2 = await c.execute("HSET", "cmp_hash", "f2", "v2")
        r3 = await c.execute("HGET", "cmp_hash", "f1")
        r4 = await c.execute("HGET", "cmp_hash", "f2")
        await c.close()
        ok = r3 == "v1" and r4 == "v2"
        return (f"HSET={r1},{r2}, HGET={r3},{r4}", ok, "")

    async def _test_hdel(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_hdelhash")
        await c.execute("HSET", "cmp_hdelhash", "f1", "v1")
        await c.execute("HSET", "cmp_hdelhash", "f2", "v2")
        r1 = await c.execute("HDEL", "cmp_hdelhash", "f1")
        r2 = await c.execute("HGET", "cmp_hdelhash", "f1")
        r3 = await c.execute("HGET", "cmp_hdelhash", "f2")
        await c.close()
        ok = r1 == "1" and r2 is None and r3 == "v2"
        return (f"HDEL={r1}, HGET f1={r2}, HGET f2={r3}", ok, "")

    async def _test_hlen(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_hlenhash")
        await c.execute("HSET", "cmp_hlenhash", "a", "1")
        await c.execute("HSET", "cmp_hlenhash", "b", "2")
        await c.execute("HSET", "cmp_hlenhash", "c", "3")
        r = await c.execute("HLEN", "cmp_hlenhash")
        await c.close()
        ok = r == "3"
        return (f"HLEN={r}", ok, "")

    async def _test_hgetall(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_hgahash")
        await c.execute("HSET", "cmp_hgahash", "x", "100")
        await c.execute("HSET", "cmp_hgahash", "y", "200")
        r = await c.execute("HGETALL", "cmp_hgahash")
        await c.close()
        ok = r is not None and "x" in str(r) and "100" in str(r) and "y" in str(r) and "200" in str(r)
        return (f"HGETALL={r}", ok, "")

    async def _test_hget_nonexist(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_hgne")
        await c.execute("HSET", "cmp_hgne", "only", "val")
        r = await c.execute("HGET", "cmp_hgne", "nope")
        await c.close()
        ok = r is None
        return (f"HGET nonexist={r}", ok, "应返回 null")

    # ── Set 测试实现 ──

    async def _test_sadd_smembers(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_set")
        r1 = await c.execute("SADD", "cmp_set", "a", "b", "c")
        r2 = await c.execute("SADD", "cmp_set", "a")  # 重复
        r3 = await c.execute("SMEMBERS", "cmp_set")
        await c.close()
        ok = r1 == "3" and r2 == "0" and r3 is not None and "a" in str(r3) and "b" in str(r3) and "c" in str(r3)
        return (f"SADD={r1},{r2}, SMEMBERS={r3}", ok, "")

    async def _test_sismember(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_simset")
        await c.execute("SADD", "cmp_simset", "hello")
        r1 = await c.execute("SISMEMBER", "cmp_simset", "hello")
        r2 = await c.execute("SISMEMBER", "cmp_simset", "world")
        await c.close()
        ok = r1 == "1" and r2 == "0"
        return (f"SISMEMBER(yes)={r1}, SISMEMBER(no)={r2}", ok, "")

    async def _test_scard(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_scardset")
        await c.execute("SADD", "cmp_scardset", "x", "y", "z")
        r = await c.execute("SCARD", "cmp_scardset")
        await c.close()
        ok = r == "3"
        return (f"SCARD={r}", ok, "")

    async def _test_spop(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_spopset")
        await c.execute("SADD", "cmp_spopset", "1", "2", "3")
        r1 = await c.execute("SPOP", "cmp_spopset")
        r2 = await c.execute("SCARD", "cmp_spopset")
        await c.close()
        ok = r1 is not None and r2 == "2"
        return (f"SPOP={r1}, SCARD={r2}", ok, "")

    # ── ZSet 测试实现 ──

    async def _test_zadd_zscore(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_zset")
        r1 = await c.execute("ZADD", "cmp_zset", "1.0", "a", "2.0", "b", "3.0", "c")
        r2 = await c.execute("ZSCORE", "cmp_zset", "b")
        await c.close()
        ok = r1 == "3" and r2 == "2.0"
        return (f"ZADD={r1}, ZSCORE(b)={r2}", ok, "")

    async def _test_zcard(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_zcardz")
        await c.execute("ZADD", "cmp_zcardz", "1", "x", "2", "y")
        r = await c.execute("ZCARD", "cmp_zcardz")
        await c.close()
        ok = r == "2"
        return (f"ZCARD={r}", ok, "")

    async def _test_zrange(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_zrz")
        await c.execute("ZADD", "cmp_zrz", "1", "a", "2", "b", "3", "c")
        r = await c.execute("ZRANGE", "cmp_zrz", "0", "1")
        await c.close()
        ok = r is not None and "a" in str(r) and "b" in str(r) and "c" not in str(r)
        return (f"ZRANGE 0-1={r}", ok, "")

    async def _test_zrange_withscores(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_zrws")
        await c.execute("ZADD", "cmp_zrws", "10", "x", "20", "y")
        r = await c.execute("ZRANGE", "cmp_zrws", "0", "-1", "WITHSCORES")
        await c.close()
        ok = r is not None and "x" in str(r) and "10" in str(r) and "y" in str(r) and "20" in str(r)
        return (f"ZRANGE WITHSCORES={r}", ok, "")

    # ── 过期机制测试 ──

    async def _test_expire(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_exp_key", "val")
        r1 = await c.execute("EXPIRE", "cmp_exp_key", "100")
        r2 = await c.execute("TTL", "cmp_exp_key")
        await c.close()
        ok = r1 == "1" and r2 is not None and int(r2) > 0
        return (f"EXPIRE={r1}, TTL={r2}", ok, "")

    async def _test_ttl(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_ttl_key", "val")
        r1 = await c.execute("TTL", "cmp_ttl_key")  # 无过期 → -1
        await c.execute("EXPIRE", "cmp_ttl_key", "50")
        r2 = await c.execute("TTL", "cmp_ttl_key")  # 有过期 → >0
        await c.close()
        ok = r1 == "-1" and r2 is not None and int(r2) > 0
        return (f"TTL(noexp)={r1}, TTL(exp)={r2}", ok, "")

    async def _test_persist(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_pers_key", "val")
        await c.execute("EXPIRE", "cmp_pers_key", "100")
        r1 = await c.execute("PERSIST", "cmp_pers_key")
        r2 = await c.execute("TTL", "cmp_pers_key")
        await c.close()
        ok = r1 == "1" and r2 == "-1"
        return (f"PERSIST={r1}, TTL={r2}", ok, "")

    async def _test_expire_auto_del(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_autodel", "val")
        await c.execute("EXPIRE", "cmp_autodel", "1")  # 1 秒过期
        await c.close()
        await asyncio.sleep(2.5)  # 等待过期
        c2 = RespClient(port=port)
        await c2.connect()
        r = await c2.execute("GET", "cmp_autodel")
        await c2.close()
        ok = r is None
        return (f"GET after expire={r}", ok, "应返回 null (已过期)")

    # ── 服务器命令 ──

    async def _test_ping(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        r = await c.execute("PING")
        await c.close()
        ok = r == "PONG"
        return (f"PING={r}", ok, "")

    async def _test_dbsize(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("FLUSHDB")
        await c.execute("SET", "cmp_db1", "a")
        await c.execute("SET", "cmp_db2", "b")
        r = await c.execute("DBSIZE")
        await c.close()
        ok = r == "2"
        return (f"DBSIZE={r}", ok, "")

    async def _test_flushdb(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("SET", "cmp_flush1", "a")
        await c.execute("SET", "cmp_flush2", "b")
        r1 = await c.execute("FLUSHDB")
        r2 = await c.execute("DBSIZE")
        await c.close()
        ok = r1 is not None and "OK" in str(r1) and r2 == "0"
        return (f"FLUSHDB={r1}, DBSIZE={r2}", ok, "")

    async def _test_info(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        r = await c.execute("INFO")
        await c.close()
        ok = r is not None and len(str(r)) > 20
        return (f"INFO length={len(str(r)) if r else 0}", ok, "应返回非空字符串")

    # ── 类型转换 ──

    async def _test_string_to_list(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_convert")
        await c.execute("SET", "cmp_convert", "hello")
        r = await c.execute("LPUSH", "cmp_convert", "world")
        await c.close()
        ok = r is not None  # 应该成功转换
        return (f"LPUSH on string={r}", ok, "STRING 应自动转为 LIST")

    async def _test_wrongtype(self, port, tag):
        c = RespClient(port=port)
        await c.connect()
        await c.execute("DEL", "cmp_wt")
        await c.execute("SADD", "cmp_wt", "member")
        r = await c.execute("HGET", "cmp_wt", "field")  # Set 上执行 Hash 命令
        await c.close()
        ok = r is not None and "ERR" in str(r)
        return (f"HGET on Set={r}", ok, "应返回 WRONGTYPE 错误")

    def _print_func_summary(self):
        print("\n" + "-" * 70)
        print(f"  {'功能测试':<35} {'ConcurrentCache':<18} {'Redis':<18}")
        print("-" * 70)
        cc_pass = 0
        redis_pass = 0
        total = len(self.results)
        for r in self.results:
            cc_sym = "✓ PASS" if r.cc_pass else "✗ FAIL"
            redis_sym = "✓ PASS" if r.redis_pass else "✗ FAIL"
            print(f"  {r.name:<35} {cc_sym:<18} {redis_sym:<18}")
            if r.cc_pass:
                cc_pass += 1
            if r.redis_pass:
                redis_pass += 1
        print("-" * 70)
        print(f"  {'总计':<35} {cc_pass}/{total} {'':<11} {redis_pass}/{total}")
        print(f"  {'通过率':<35} {cc_pass/total*100:.1f}%{'':<13} {redis_pass/total*100:.1f}%")


# ═══════════════════════════════════════════════════════════════
# 第二部分：性能基准测试
# ═══════════════════════════════════════════════════════════════

class PerformanceTester:
    def __init__(self, cc_port: int, redis_port: int):
        self.cc_port = cc_port
        self.redis_port = redis_port
        self.results: List[PerfTierResult] = []

    async def _warmup(self, port: int, n: int = 2000):
        """预热：写入初始数据"""
        c = RespClient(port=port)
        if await c.connect():
            tasks = [c.execute("SET", f"perf_warm:{i}", f"val_{i}") for i in range(n)]
            # 分批执行避免连接过载
            for i in range(0, n, 50):
                batch = tasks[i:i+50]
                await asyncio.gather(*batch, return_exceptions=True)
            await c.close()

    async def _run_tier(self, port: int, tag: str, concurrent: int,
                        duration_sec: int, read_ratio: float,
                        value_size: int = 16) -> Tuple[float, float, float, float, float, float, int]:
        """运行一个并发阶梯，返回 (qps, p50, p99, p999, max, err_rate, total_ops)"""
        stats = {"total_ops": 0, "failed_ops": 0, "connect_fails": 0, "latencies": []}
        lock = asyncio.Lock()
        stop_event = asyncio.Event()

        async def worker(wid: int):
            client = RespClient(port=port)
            if not await client.connect():
                async with lock:
                    stats["connect_fails"] += 1
                return

            local_ops = 0
            local_fails = 0
            local_lats = []
            i = 0
            val = "x" * value_size

            try:
                while not stop_event.is_set():
                    key = f"perf_k:{wid}:{i % 500}"
                    op_start = time.monotonic()

                    try:
                        r = i % 100
                        if r < int(read_ratio * 100):
                            result = await client.execute("GET", key, timeout=3.0)
                        elif r < int(read_ratio * 100) + 10:
                            result = await client.execute("SET", key, val, timeout=3.0)
                        else:
                            result = await client.execute("DEL", key, timeout=3.0)
                    except Exception:
                        result = None

                    elapsed = (time.monotonic() - op_start) * 1000
                    local_lats.append(elapsed)

                    if result is None:
                        local_fails += 1
                    else:
                        local_ops += 1
                    i += 1

            except (ConnectionError, OSError, asyncio.TimeoutError):
                local_fails += 1
            finally:
                await client.close()

            async with lock:
                stats["total_ops"] += local_ops
                stats["failed_ops"] += local_fails
                stats["latencies"].extend(local_lats)

        start = time.monotonic()
        tasks = [asyncio.create_task(worker(i)) for i in range(concurrent)]
        await asyncio.sleep(duration_sec)
        stop_event.set()
        await asyncio.wait_for(asyncio.gather(*tasks, return_exceptions=True), timeout=8.0)
        elapsed = time.monotonic() - start

        total = stats["total_ops"] + stats["failed_ops"]
        qps = total / elapsed if elapsed > 0 else 0

        lats = sorted(stats["latencies"])
        if lats:
            p50 = lats[len(lats)//2]
            p99 = lats[int(len(lats)*0.99)]
            p999 = lats[int(len(lats)*0.999)]
            lat_max = lats[-1]
        else:
            p50 = p99 = p999 = lat_max = 0

        all_attempts = total + stats["connect_fails"]
        err_rate = (stats["failed_ops"] + stats["connect_fails"]) / max(all_attempts, 1) * 100

        return (qps, p50, p99, p999, lat_max, err_rate, total)

    async def run_all(self):
        print("\n" + "=" * 70)
        print("  第二部分：性能基准测试")
        print("=" * 70)

        # 预热两个服务器
        print("\n  预热 ConcurrentCache...")
        await self._warmup(self.cc_port, 2000)
        print("  预热 Redis...")
        await self._warmup(self.redis_port, 2000)
        print("  预热完成\n")

        # ── 测试1: 单连接纯 GET QPS ──
        print("  ── 单连接吞吐量 ──")
        await self._bench_single_conn("纯 GET (单连接)", 0.95, 16)
        await self._bench_single_conn("纯 SET (单连接)", 0.0, 16)
        await self._bench_single_conn("混合负载 (单连接)", 0.7, 16)

        # ── 测试2: 并发阶梯 ──
        print("\n  ── 并发扩展性 (混合负载 70%R/10%W/20%D) ──")
        tiers = [10, 50, 100, 200, 500, 1000, 2000, 3000, 5000]
        for tier in tiers:
            await self._bench_tier(f"并发={tier}", tier, 10, 0.7, 16)

        # ── 测试3: 大 Value 性能 ──
        print("\n  ── 大 Value 性能 (并发=100) ──")
        for size, label in [(1024, "1KB"), (10240, "10KB"), (102400, "100KB")]:
            await self._bench_tier(f"SET {label} value", 100, 8, 0.0, size)
            await self._bench_tier(f"GET {label} value", 100, 8, 0.95, size)

        # 打印性能汇总
        self._print_perf_summary()

    async def _bench_single_conn(self, label: str, read_ratio: float, val_size: int):
        """单连接基准测试"""
        r = PerfTierResult(label=label)
        r.cc_qps, r.cc_p50_ms, r.cc_p99_ms, r.cc_p999_ms, r.cc_max_ms, r.cc_err_rate, r.cc_total_ops = \
            await self._run_tier(self.cc_port, "CC", 1, 8, read_ratio, val_size)
        await asyncio.sleep(0.5)
        r.redis_qps, r.redis_p50_ms, r.redis_p99_ms, r.redis_p999_ms, r.redis_max_ms, r.redis_err_rate, r.redis_total_ops = \
            await self._run_tier(self.redis_port, "Redis", 1, 8, read_ratio, val_size)
        self.results.append(r)

        ratio = r.cc_qps / r.redis_qps * 100 if r.redis_qps > 0 else 0
        print(f"  {label:<30} CC: {r.cc_qps:>10.1f} QPS  p99={r.cc_p99_ms:>6.1f}ms  |  "
              f"Redis: {r.redis_qps:>10.1f} QPS  p99={r.redis_p99_ms:>6.1f}ms  |  "
              f"CC/Redis={ratio:.0f}%")

    async def _bench_tier(self, label: str, concurrent: int, duration: int,
                          read_ratio: float, val_size: int):
        """并发阶梯基准测试"""
        r = PerfTierResult(label=label)
        r.cc_qps, r.cc_p50_ms, r.cc_p99_ms, r.cc_p999_ms, r.cc_max_ms, r.cc_err_rate, r.cc_total_ops = \
            await self._run_tier(self.cc_port, "CC", concurrent, duration, read_ratio, val_size)
        await asyncio.sleep(1.0)
        r.redis_qps, r.redis_p50_ms, r.redis_p99_ms, r.redis_p999_ms, r.redis_max_ms, r.redis_err_rate, r.redis_total_ops = \
            await self._run_tier(self.redis_port, "Redis", concurrent, duration, read_ratio, val_size)
        self.results.append(r)

        ratio = r.cc_qps / r.redis_qps * 100 if r.redis_qps > 0 else 0
        print(f"  {label:<30} CC: {r.cc_qps:>10.1f} QPS  p99={r.cc_p99_ms:>6.1f}ms  err={r.cc_err_rate:.1f}%  |  "
              f"Redis: {r.redis_qps:>10.1f} QPS  p99={r.redis_p99_ms:>6.1f}ms  err={r.redis_err_rate:.1f}%  |  "
              f"CC/Redis={ratio:.0f}%")

    def _print_perf_summary(self):
        print("\n" + "=" * 70)
        print("  性能测试汇总")
        print("=" * 70)
        print(f"  {'测试场景':<30} {'CC QPS':>10} {'Redis QPS':>10} {'CC/Redis':>9} {'CC p99':>8} {'Redis p99':>8}")
        print("-" * 70)
        for r in self.results:
            ratio = r.cc_qps / r.redis_qps * 100 if r.redis_qps > 0 else 0
            print(f"  {r.label:<30} {r.cc_qps:>10.1f} {r.redis_qps:>10.1f} {ratio:>8.0f}% {r.cc_p99_ms:>7.1f}ms {r.redis_p99_ms:>7.1f}ms")

        # 计算平均 QPS 比率
        ratios = [r.cc_qps/r.redis_qps*100 for r in self.results if r.redis_qps > 0]
        if ratios:
            avg_ratio = statistics.mean(ratios)
            print(f"\n  平均 QPS 比率 (CC/Redis): {avg_ratio:.1f}%")


# ═══════════════════════════════════════════════════════════════
# 第三部分：极限/鲁棒性测试
# ═══════════════════════════════════════════════════════════════

class StressTester:
    def __init__(self, cc_port: int, redis_port: int):
        self.cc_port = cc_port
        self.redis_port = redis_port
        self.results: List[FuncTestResult] = []

    async def run_all(self):
        print("\n" + "=" * 70)
        print("  第三部分：极限/鲁棒性测试")
        print("=" * 70)

        # ── 连接风暴 ──
        print("\n  ── 连接风暴 (2000 并发连接) ──")
        await self._test_connection_storm()

        # ── 快速连接/断开 ──
        print("\n  ── 快速连接/断开 (500 次) ──")
        await self._test_rapid_connect()

        # ── 大 key 数量 ──
        print("\n  ── 大量 key 写入 (50000 keys) ──")
        await self._test_many_keys()

        # ── 并发写入一致性 ──
        print("\n  ── 并发写入一致性 ──")
        await self._test_concurrent_writes()

        self._print_stress_summary()

    async def _test_connection_storm(self):
        """瞬间大量连接"""
        async def storm(port: int, tag: str) -> Tuple[int, int, float]:
            success = 0
            failed = 0
            start = time.monotonic()

            async def one_conn():
                nonlocal success, failed
                try:
                    reader, writer = await asyncio.wait_for(
                        asyncio.open_connection("127.0.0.1", port), timeout=3.0
                    )
                    writer.write(b"*1\r\n$4\r\nPING\r\n")
                    await writer.drain()
                    resp = await asyncio.wait_for(reader.readline(), timeout=2.0)
                    if b"PONG" in resp:
                        success += 1
                    else:
                        failed += 1
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except Exception:
                        pass
                except Exception:
                    failed += 1

            tasks = [asyncio.create_task(one_conn()) for _ in range(2000)]
            await asyncio.gather(*tasks, return_exceptions=True)
            elapsed = time.monotonic() - start
            return (success, failed, elapsed)

        cc_ok, cc_fail, cc_time = await storm(self.cc_port, "CC")
        await asyncio.sleep(1.0)
        redis_ok, redis_fail, redis_time = await storm(self.redis_port, "Redis")

        cc_rate = cc_ok / (cc_ok + cc_fail) * 100 if (cc_ok + cc_fail) > 0 else 0
        redis_rate = redis_ok / (redis_ok + redis_fail) * 100 if (redis_ok + redis_fail) > 0 else 0

        print(f"  ConcurrentCache: {cc_ok}/{cc_ok+cc_fail} 成功 ({cc_rate:.1f}%), {cc_time:.1f}s")
        print(f"  Redis:           {redis_ok}/{redis_ok+redis_fail} 成功 ({redis_rate:.1f}%), {redis_time:.1f}s")

        self.results.append(FuncTestResult(
            name="连接风暴 2000",
            cc_result=f"{cc_ok}/{cc_ok+cc_fail} ({cc_rate:.1f}%)",
            redis_result=f"{redis_ok}/{redis_ok+redis_fail} ({redis_rate:.1f}%)",
            cc_pass=cc_rate > 70,
            redis_pass=redis_rate > 70,
        ))

    async def _test_rapid_connect(self):
        """快速连接/断开"""
        async def rapid(port: int, tag: str) -> Tuple[int, float]:
            success = 0
            start = time.monotonic()
            for _ in range(500):
                try:
                    reader, writer = await asyncio.wait_for(
                        asyncio.open_connection("127.0.0.1", port), timeout=2.0
                    )
                    writer.write(b"*1\r\n$4\r\nPING\r\n")
                    await writer.drain()
                    resp = await asyncio.wait_for(reader.readline(), timeout=2.0)
                    if b"PONG" in resp:
                        success += 1
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except Exception:
                        pass
                except Exception:
                    pass
            elapsed = time.monotonic() - start
            return (success, elapsed)

        cc_ok, cc_time = await rapid(self.cc_port, "CC")
        await asyncio.sleep(0.5)
        redis_ok, redis_time = await rapid(self.redis_port, "Redis")

        print(f"  ConcurrentCache: {cc_ok}/500 成功, {cc_time:.1f}s ({500/cc_time:.0f} conn/s)")
        print(f"  Redis:           {redis_ok}/500 成功, {redis_time:.1f}s ({500/redis_time:.0f} conn/s)")

        self.results.append(FuncTestResult(
            name="快速连接/断开 500",
            cc_result=f"{cc_ok}/500 ({500/cc_time:.0f} conn/s)",
            redis_result=f"{redis_ok}/500 ({500/redis_time:.0f} conn/s)",
            cc_pass=cc_ok >= 450,
            redis_pass=redis_ok >= 450,
        ))

    async def _test_many_keys(self):
        """大量 key 写入"""
        async def write_many(port: int, tag: str, count: int) -> Tuple[int, float]:
            success = 0
            start = time.monotonic()
            c = RespClient(port=port)
            if await c.connect():
                # 分批写入
                batch_size = 200
                for batch_start in range(0, count, batch_size):
                    batch_end = min(batch_start + batch_size, count)
                    tasks = []
                    for i in range(batch_start, batch_end):
                        tasks.append(c.execute("SET", f"mk:{i}", f"v{i}", timeout=10.0))
                    results = await asyncio.gather(*tasks, return_exceptions=True)
                    for r in results:
                        if r is not None and "OK" in str(r):
                            success += 1
                await c.close()
            elapsed = time.monotonic() - start
            return (success, elapsed)

        N = 50000
        print(f"  写入 {N} 个 key...")
        cc_ok, cc_time = await write_many(self.cc_port, "CC", N)
        await asyncio.sleep(0.5)
        redis_ok, redis_time = await write_many(self.redis_port, "Redis", N)

        print(f"  ConcurrentCache: {cc_ok}/{N} 成功, {cc_time:.1f}s ({cc_ok/cc_time:.0f} keys/s)")
        print(f"  Redis:           {redis_ok}/{N} 成功, {redis_time:.1f}s ({redis_ok/redis_time:.0f} keys/s)")

        self.results.append(FuncTestResult(
            name=f"大量 key 写入 {N}",
            cc_result=f"{cc_ok}/{N} ({cc_ok/cc_time:.0f} keys/s)",
            redis_result=f"{redis_ok}/{N} ({redis_ok/redis_time:.0f} keys/s)",
            cc_pass=cc_ok >= N * 0.95,
            redis_pass=redis_ok >= N * 0.95,
        ))

    async def _test_concurrent_writes(self):
        """并发写入一致性：100 并发各写不同 key，验证无串扰"""
        async def concurrent_write_test(port: int, tag: str) -> Tuple[int, int]:
            mismatches = 0
            total = 0

            async def writer_reader(wid: int):
                nonlocal mismatches, total
                key = f"cw:{wid}"
                val = f"value_from_{wid}"
                c = RespClient(port=port)
                if await c.connect():
                    r1 = await c.execute("SET", key, val)
                    r2 = await c.execute("GET", key)
                    await c.close()
                    total += 1
                    if r2 != val:
                        mismatches += 1

            tasks = [asyncio.create_task(writer_reader(i)) for i in range(100)]
            await asyncio.gather(*tasks, return_exceptions=True)
            return (total, mismatches)

        cc_total, cc_mismatch = await concurrent_write_test(self.cc_port, "CC")
        await asyncio.sleep(0.3)
        redis_total, redis_mismatch = await concurrent_write_test(self.redis_port, "Redis")

        print(f"  ConcurrentCache: {cc_mismatch}/{cc_total} 不一致")
        print(f"  Redis:           {redis_mismatch}/{redis_total} 不一致")

        self.results.append(FuncTestResult(
            name="并发写入一致性 100",
            cc_result=f"{cc_mismatch}/{cc_total} 不一致",
            redis_result=f"{redis_mismatch}/{redis_total} 不一致",
            cc_pass=cc_mismatch == 0,
            redis_pass=redis_mismatch == 0,
        ))

    def _print_stress_summary(self):
        print("\n" + "-" * 70)
        print(f"  {'极限/鲁棒性测试':<30} {'ConcurrentCache':<22} {'Redis':<22}")
        print("-" * 70)
        cc_pass = 0
        redis_pass = 0
        for r in self.results:
            cc_sym = "✓ PASS" if r.cc_pass else "✗ FAIL"
            redis_sym = "✓ PASS" if r.redis_pass else "✗ FAIL"
            print(f"  {r.name:<30} {cc_sym:<22} {redis_sym:<22}")
            if r.cc_pass:
                cc_pass += 1
            if r.redis_pass:
                redis_pass += 1
        total = len(self.results)
        print("-" * 70)
        print(f"  {'总计':<30} {cc_pass}/{total}{'':<17} {redis_pass}/{total}")


# ═══════════════════════════════════════════════════════════════
# 第四部分：内存占用对比
# ═══════════════════════════════════════════════════════════════

class MemoryTester:
    def __init__(self, cc_port: int, redis_port: int, server_mgr: ServerManager):
        self.cc_port = cc_port
        self.redis_port = redis_port
        self.server_mgr = server_mgr

    async def run(self) -> Dict:
        print("\n" + "=" * 70)
        print("  第四部分：内存占用对比")
        print("=" * 70)

        # 先 FLUSHDB 清空
        for port in [self.cc_port, self.redis_port]:
            c = RespClient(port=port)
            if await c.connect():
                await c.execute("FLUSHDB")
                await c.close()

        await asyncio.sleep(0.5)

        # 测量空载内存
        mem_empty_cc = self.server_mgr.get_memory_rss(self.cc_port)
        mem_empty_redis = self.server_mgr.get_memory_rss(self.redis_port)
        print(f"\n  空载内存: CC={mem_empty_cc/1024/1024:.1f}MB, Redis={mem_empty_redis/1024/1024:.1f}MB")

        # 写入 50000 个 key (每个 value 256 字节)
        N = 50000
        val = "x" * 256
        print(f"  写入 {N} 个 key (value=256B)...")

        for port, tag in [(self.cc_port, "CC"), (self.redis_port, "Redis")]:
            c = RespClient(port=port)
            if await c.connect():
                for i in range(0, N, 500):
                    batch_end = min(i + 500, N)
                    tasks = [c.execute("SET", f"mem:{j}", val) for j in range(i, batch_end)]
                    await asyncio.gather(*tasks, return_exceptions=True)
                await c.close()

        await asyncio.sleep(1.0)

        mem_full_cc = self.server_mgr.get_memory_rss(self.cc_port)
        mem_full_redis = self.server_mgr.get_memory_rss(self.redis_port)

        delta_cc = mem_full_cc - mem_empty_cc
        delta_redis = mem_full_redis - mem_empty_redis
        per_key_cc = delta_cc / N if N > 0 else 0
        per_key_redis = delta_redis / N if N > 0 else 0

        print(f"\n  {'':<20} {'ConcurrentCache':>18} {'Redis':>18}")
        print(f"  {'空载 RSS':<20} {mem_empty_cc/1024/1024:>15.1f} MB {mem_empty_redis/1024/1024:>15.1f} MB")
        print(f"  {'满载 RSS':<20} {mem_full_cc/1024/1024:>15.1f} MB {mem_full_redis/1024/1024:>15.1f} MB")
        print(f"  {'增量':<20} {delta_cc/1024/1024:>15.1f} MB {delta_redis/1024/1024:>15.1f} MB")
        print(f"  {'每 key 开销':<20} {per_key_cc:>15.0f} B {per_key_redis:>15.0f} B")

        return {
            "cc_empty_rss_mb": round(mem_empty_cc / 1024 / 1024, 2),
            "redis_empty_rss_mb": round(mem_empty_redis / 1024 / 1024, 2),
            "cc_full_rss_mb": round(mem_full_cc / 1024 / 1024, 2),
            "redis_full_rss_mb": round(mem_full_redis / 1024 / 1024, 2),
            "cc_delta_mb": round(delta_cc / 1024 / 1024, 2),
            "redis_delta_mb": round(delta_redis / 1024 / 1024, 2),
            "cc_per_key_b": round(per_key_cc, 0),
            "redis_per_key_b": round(per_key_redis, 0),
            "num_keys": N,
            "value_size": 256,
        }


# ═══════════════════════════════════════════════════════════════
# 主流程
# ═══════════════════════════════════════════════════════════════

async def main():
    print("=" * 70)
    print("  ConcurrentCache vs Redis — 深度对比测试")
    print(f"  时间: {datetime.now().isoformat()}")
    print("=" * 70)

    server_mgr = ServerManager()

    # 获取 Redis 版本
    redis_ver = server_mgr.get_redis_version()
    print(f"\n  Redis 版本: {redis_ver}")
    print(f"  ConcurrentCache 版本: 3.0")

    # 启动服务器
    print("\n" + "-" * 70)
    print("  启动服务器")
    print("-" * 70)

    if not server_mgr.start_concurrentcache():
        print("FATAL: ConcurrentCache 启动失败")
        server_mgr.stop_all()
        return 1

    if not server_mgr.start_redis():
        print("FATAL: Redis 启动失败")
        server_mgr.stop_all()
        return 1

    report = ComparisonReport(
        timestamp=datetime.now().isoformat(),
        cc_version="3.0",
        redis_version=redis_ver,
    )

    try:
        # 第一部分：功能正确性
        func_tester = FunctionalTester(server_mgr.cc_port, server_mgr.redis_port)
        await func_tester.run_all()
        report.functional_tests = func_tester.results

        # 第二部分：性能基准
        perf_tester = PerformanceTester(server_mgr.cc_port, server_mgr.redis_port)
        await perf_tester.run_all()
        report.perf_tiers = perf_tester.results

        # 第三部分：极限/鲁棒性
        stress_tester = StressTester(server_mgr.cc_port, server_mgr.redis_port)
        await stress_tester.run_all()
        # 合并到 functional_tests
        report.functional_tests.extend(stress_tester.results)

        # 第四部分：内存占用
        mem_tester = MemoryTester(server_mgr.cc_port, server_mgr.redis_port, server_mgr)
        report.memory = await mem_tester.run()

        # ── 生成最终报告 ──
        _generate_final_report(report)

    finally:
        server_mgr.stop_all()

    return 0


def _generate_final_report(report: ComparisonReport):
    print("\n")
    print("=" * 70)
    print("  ╔══════════════════════════════════════════════════════╗")
    print("  ║              最 终 对 比 报 告                        ║")
    print("  ╚══════════════════════════════════════════════════════╝")
    print("=" * 70)

    # 功能测试统计
    func_tests = [t for t in report.functional_tests if "连接风暴" not in t.name
                  and "快速连接" not in t.name and "大量 key" not in t.name
                  and "并发写入一致性" not in t.name]
    stress_tests = [t for t in report.functional_tests if "连接风暴" in t.name
                    or "快速连接" in t.name or "大量 key" in t.name
                    or "并发写入一致性" in t.name]

    cc_func_pass = sum(1 for t in func_tests if t.cc_pass)
    redis_func_pass = sum(1 for t in func_tests if t.redis_pass)
    func_total = len(func_tests)

    cc_stress_pass = sum(1 for t in stress_tests if t.cc_pass)
    redis_stress_pass = sum(1 for t in stress_tests if t.redis_pass)
    stress_total = len(stress_tests)

    print(f"\n  ┌─────────────────────────────────────────────────────┐")
    print(f"  │  功能正确性                                        │")
    print(f"  │    ConcurrentCache: {cc_func_pass}/{func_total} 通过 ({cc_func_pass/func_total*100:.1f}%){'':>16}│")
    print(f"  │    Redis:           {redis_func_pass}/{func_total} 通过 ({redis_func_pass/func_total*100:.1f}%){'':>16}│")
    print(f"  ├─────────────────────────────────────────────────────┤")
    print(f"  │  极限/鲁棒性                                       │")
    print(f"  │    ConcurrentCache: {cc_stress_pass}/{stress_total} 通过 ({cc_stress_pass/stress_total*100:.1f}%){'':>16}│")
    print(f"  │    Redis:           {redis_stress_pass}/{stress_total} 通过 ({redis_stress_pass/stress_total*100:.1f}%){'':>16}│")
    print(f"  ├─────────────────────────────────────────────────────┤")

    # 性能统计
    if report.perf_tiers:
        ratios = [r.cc_qps/r.redis_qps*100 for r in report.perf_tiers if r.redis_qps > 0]
        if ratios:
            avg_ratio = statistics.mean(ratios)
            min_ratio = min(ratios)
            max_ratio = max(ratios)
            print(f"  │  性能 (QPS CC/Redis)                               │")
            print(f"  │    平均比率: {avg_ratio:.1f}%{'':>33}│")
            print(f"  │    最低比率: {min_ratio:.1f}%{'':>33}│")
            print(f"  │    最高比率: {max_ratio:.1f}%{'':>33}│")

        # 找到峰值 QPS
        cc_peak = max((r.cc_qps for r in report.perf_tiers), default=0)
        redis_peak = max((r.redis_qps for r in report.perf_tiers), default=0)
        print(f"  │    峰值 QPS: CC={cc_peak:.0f}, Redis={redis_peak:.0f}{'':>16}│")

    print(f"  ├─────────────────────────────────────────────────────┤")

    # 内存统计
    if report.memory:
        m = report.memory
        print(f"  │  内存占用 ({m['num_keys']} keys, {m['value_size']}B value)                 │")
        print(f"  │    空载: CC={m['cc_empty_rss_mb']:.1f}MB, Redis={m['redis_empty_rss_mb']:.1f}MB{'':>18}│")
        print(f"  │    增量: CC={m['cc_delta_mb']:.1f}MB, Redis={m['redis_delta_mb']:.1f}MB{'':>18}│")
        print(f"  │    每key: CC={m['cc_per_key_b']:.0f}B, Redis={m['redis_per_key_b']:.0f}B{'':>20}│")

    print(f"  └─────────────────────────────────────────────────────┘")

    # 保存 JSON 报告
    report_path = Path(__file__).parent / "comparison_report.json"
    report_data = {
        "timestamp": report.timestamp,
        "cc_version": report.cc_version,
        "redis_version": report.redis_version,
        "functional": {
            "cc_pass": cc_func_pass,
            "cc_total": func_total,
            "cc_rate": round(cc_func_pass/func_total*100, 1) if func_total > 0 else 0,
            "redis_pass": redis_func_pass,
            "redis_total": func_total,
            "redis_rate": round(redis_func_pass/func_total*100, 1) if func_total > 0 else 0,
            "details": [{"name": t.name, "cc_pass": t.cc_pass, "redis_pass": t.redis_pass,
                         "cc_result": t.cc_result, "redis_result": t.redis_result}
                        for t in func_tests],
        },
        "stress": {
            "cc_pass": cc_stress_pass,
            "cc_total": stress_total,
            "redis_pass": redis_stress_pass,
            "redis_total": stress_total,
            "details": [{"name": t.name, "cc_pass": t.cc_pass, "redis_pass": t.redis_pass,
                         "cc_result": t.cc_result, "redis_result": t.redis_result}
                        for t in stress_tests],
        },
        "performance": {
            "avg_qps_ratio": round(statistics.mean(ratios), 1) if ratios else 0,
            "cc_peak_qps": round(cc_peak, 0) if 'cc_peak' in dir() else 0,
            "redis_peak_qps": round(redis_peak, 0) if 'redis_peak' in dir() else 0,
            "tiers": [asdict(r) for r in report.perf_tiers],
        },
        "memory": report.memory,
    }
    report_path.write_text(json.dumps(report_data, indent=2, ensure_ascii=False))
    print(f"\n  详细 JSON 报告: {report_path}")


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
