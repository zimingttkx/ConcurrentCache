#!/usr/bin/env python3
"""
ConcurrentCache — 并发极限压力测试
==============================
阶梯式增加并发，找到系统吞吐量天花板和稳定运行的极限阈值。
"""

import asyncio
import subprocess
import time
import os
import sys
import signal
import json
import tempfile
import shutil
from pathlib import Path
from typing import List, Optional, Tuple
from dataclasses import dataclass
from datetime import datetime


# ─── RESP Client ────────────────────────────────────────────

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
        if data[0:1] == b'+':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return data[1:end].decode(), data[end+2:]
        elif data[0:1] == b'-':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return "ERR:" + data[1:end].decode(), data[end+2:]
        elif data[0:1] == b':':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return data[1:end].decode(), data[end+2:]
        elif data[0:1] == b'$':
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            length = int(data[1:end])
            if length < 0:
                return None, data[end+2:]
            start = end + 2
            if len(data) < start + length + 2:
                return None, data
            return data[start:start+length].decode(), data[start+length+2:]
        return None, data


class RespClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 6379):
        self.host = host
        self.port = port
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None

    async def connect(self) -> bool:
        try:
            self.reader, self.writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), timeout=2.0
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

    async def execute(self, *args: str) -> Optional[str]:
        try:
            self.writer.write(RESP.encode_cmd(*args))
            await self.writer.drain()
            raw = await asyncio.wait_for(self.reader.read(65536), timeout=5.0)
            result, _ = RESP.parse_line(raw)
            return result if result is not None else ""
        except Exception:
            return None


# ─── Stress Test Engine ─────────────────────────────────────

@dataclass
class TierResult:
    concurrent: int
    duration_sec: float
    total_ops: int
    success_ops: int
    failed_ops: int
    connect_fails: int
    qps: float
    p50_ms: float
    p99_ms: float
    p999_ms: float
    max_ms: float


class StressTest:
    def __init__(self, port: int = 6379):
        self.port = port
        self.results: List[TierResult] = []

    async def _worker(self, client_id: int, read_ratio: float,
                      stop_event: asyncio.Event, stats: dict, lock: asyncio.Lock):
        """单个客户端工作循环 — 持续运行直到 stop_event 被设置"""
        client = RespClient(port=self.port)
        if not await client.connect():
            async with lock:
                stats["connect_fails"] += 1
            return

        local_ops = 0
        local_fails = 0
        local_latencies = []
        i = 0

        try:
            while not stop_event.is_set():
                key = f"stress:{client_id}:{i % 500}"
                op_start = time.monotonic()

                try:
                    r = i % 100
                    if r < int(read_ratio * 100):
                        result = await client.execute("GET", key)
                    elif r < int(read_ratio * 100) + 10:
                        val = f"v_{client_id}_{i}"
                        result = await client.execute("SET", key, val)
                    else:
                        result = await client.execute("DEL", key)
                except Exception:
                    result = None

                elapsed = (time.monotonic() - op_start) * 1000
                local_latencies.append(elapsed)

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
            stats["latencies"].extend(local_latencies)

    async def run_tier(self, concurrent: int, duration_sec: int = 10,
                       read_ratio: float = 0.7) -> TierResult:
        """运行一个并发阶梯"""
        stats = {
            "total_ops": 0,
            "failed_ops": 0,
            "connect_fails": 0,
            "latencies": [],
        }
        lock = asyncio.Lock()
        stop_event = asyncio.Event()

        print(f"  [{concurrent:>5} 并发] ", end="", flush=True)

        start = time.monotonic()

        tasks = [
            asyncio.create_task(
                self._worker(i, read_ratio, stop_event, stats, lock)
            )
            for i in range(concurrent)
        ]

        # 等待指定时长，然后通知所有 worker 停止
        await asyncio.sleep(duration_sec)
        stop_event.set()

        # 等待所有 worker 优雅退出（最多 5 秒）
        await asyncio.wait_for(asyncio.gather(*tasks, return_exceptions=True), timeout=8.0)

        elapsed = time.monotonic() - start
        total = stats["total_ops"] + stats["failed_ops"]
        qps = total / elapsed if elapsed > 0 else 0

        lats = sorted(stats["latencies"])
        if lats:
            p50 = lats[len(lats) // 2]
            p99 = lats[int(len(lats) * 0.99)]
            p999 = lats[int(len(lats) * 0.999)]
            lat_max = lats[-1]
        else:
            p50 = p99 = p999 = lat_max = 0

        all_attempts = total + stats["connect_fails"]
        error_rate = (stats["failed_ops"] + stats["connect_fails"]) / max(all_attempts, 1) * 100

        result = TierResult(
            concurrent=concurrent, duration_sec=elapsed,
            total_ops=total, success_ops=stats["total_ops"],
            failed_ops=stats["failed_ops"], connect_fails=stats["connect_fails"],
            qps=qps, p50_ms=p50, p99_ms=p99, p999_ms=p999, max_ms=lat_max,
        )
        self.results.append(result)

        status = "OK" if error_rate < 1 else ("WARN" if error_rate < 10 else "FAIL")
        print(f"QPS={qps:>10.1f}  p50={p50:>6.1f}ms  p99={p99:>7.1f}ms  "
              f"p999={p999:>7.1f}ms  err={error_rate:.2f}%  [{status}]")

        return result


async def main():
    project_root = Path(__file__).resolve().parent.parent.parent
    server_bin = project_root / "build" / "concurrentcache-server"

    if not server_bin.exists():
        print(f"ERROR: Server binary not found at {server_bin}")
        sys.exit(1)

    print("=" * 70)
    print("  ConcurrentCache — 并发极限压力测试")
    print(f"  时间: {datetime.now().isoformat()}")
    print("=" * 70)

    tmp_dir = tempfile.mkdtemp(prefix="cc_stress_")
    conf_dir = Path(tmp_dir) / "conf"
    conf_dir.mkdir()
    conf_file = conf_dir / "concurrentcache.conf"
    conf_file.write_text("""port = 16379
log_level = 4
rdb_save_interval = 0
cluster_enabled = false
max_entries = 2000000
""")
    (Path(tmp_dir) / "dump.rdb").touch()
    print(f"  临时目录: {tmp_dir}")
    print(f"  端口: 16379")
    print(f"  配置: reactor_count=auto(32), thread_pool=auto(32)")
    print(f"        rdb=off, cluster=off (单机最大性能)")

    print("\n  启动服务器...")
    env = os.environ.copy()
    proc = subprocess.Popen(
        [str(server_bin)],
        cwd=tmp_dir,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
    )
    time.sleep(2.0)

    # 预热
    print("  预热 (1000 SETs)...")
    warmup = RespClient(port=16379)
    if await warmup.connect():
        for i in range(1000):
            await warmup.execute("SET", f"warmup:{i}", f"val_{i}")
        await warmup.close()
    print("  预热完成\n")

    tester = StressTest(port=16379)

    # 阶梯式压测
    tiers = [
        50, 100, 200, 300, 500, 700, 1000,
        1500, 2000, 3000, 4000, 5000, 7000,
        10000, 15000, 20000, 25000, 30000, 40000, 50000,
    ]

    print(f"{'='*70}")
    print(f"  {'并发数':>6}  {'QPS':>10}  {'p50(ms)':>8}  {'p99(ms)':>9}  {'p999(ms)':>9}  {'错误率':>7}  状态")
    print(f"{'-'*70}")

    breaking_point = None

    for tier in tiers:
        result = await tester.run_tier(tier, duration_sec=12, read_ratio=0.7)
        await asyncio.sleep(1.5)

        all_attempts = result.total_ops + result.connect_fails
        error_rate = (result.failed_ops + result.connect_fails) / max(all_attempts, 1) * 100

        if (error_rate > 5 or result.p999_ms > 1000) and breaking_point is None:
            breaking_point = tier
            print(f"\n  >>> 破防点检测: {tier} 并发 (错误率={error_rate:.1f}%, p999={result.p999_ms:.1f}ms) <<<")
            break

    # 长期稳定性测试（在破防点 70% 处，确保稳定）
    if breaking_point:
        stable_concurrent = int(breaking_point * 0.7)
    else:
        stable_concurrent = tiers[-1]

    print(f"\n{'='*70}")
    print(f"  长期稳定性验证: {stable_concurrent} 并发, 4x30s")
    print(f"{'-'*70}")
    for phase in range(4):
        await tester.run_tier(stable_concurrent, duration_sec=30, read_ratio=0.7)
        await asyncio.sleep(2.0)

    # 停止服务器
    print(f"\n  停止服务器...")
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    shutil.rmtree(tmp_dir, ignore_errors=True)

    # ─── 报告 ───
    print(f"\n{'='*70}")
    print(f"  {'压测报告':^60}")
    print(f"{'='*70}")
    print(f"  {'并发数':>6}  {'QPS':>10}  {'p50':>7}ms  {'p99':>8}ms  {'p999':>8}ms  {'ops':>8}  {'错误率':>7}")
    print(f"  {'-'*65}")

    max_safe = 0
    max_qps = 0.0
    for r in tester.results:
        err = (r.failed_ops + r.connect_fails) / max(r.total_ops + r.connect_fails, 1) * 100
        marker = " <-- 破防" if r.concurrent == breaking_point else ""
        print(f"  {r.concurrent:>6}  {r.qps:>10.1f}  {r.p50_ms:>6.1f}   {r.p99_ms:>7.1f}   {r.p999_ms:>7.1f}   {r.total_ops:>6}   {err:>5.1f}%{marker}")
        if err < 1:
            max_safe = r.concurrent
        if r.qps > max_qps:
            max_qps = r.qps

    print(f"\n  ╔══════════════════════════════════════╗")
    print(f"  ║  安全并发上限: {max_safe:>6} 客户端         ║")
    print(f"  ║  峰值 QPS:     {max_qps:>10.1f}           ║")
    print(f"  ╚══════════════════════════════════════╝")

    # 保存 JSON
    report_path = Path(__file__).parent / "stress_limit_report.json"
    report = {
        "timestamp": datetime.now().isoformat(),
        "safe_concurrent": max_safe,
        "peak_qps": max_qps,
        "tiers": [
            {
                "concurrent": r.concurrent,
                "qps": r.qps,
                "p50_ms": r.p50_ms,
                "p99_ms": r.p99_ms,
                "p999_ms": r.p999_ms,
                "total_ops": r.total_ops,
                "error_rate": (r.failed_ops + r.connect_fails) / max(r.total_ops + r.connect_fails, 1) * 100,
            }
            for r in tester.results
        ],
    }
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print(f"\n  详细报告: {report_path}")

    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
