#!/usr/bin/env python3
"""
ConcurrentCache — 集群模式压力测试
=================================
3节点集群 + 槽分片 + 混合读写压测
客户端自动计算 CRC16 哈希槽，路由到正确节点。
"""

import asyncio, subprocess, time, os, sys, signal, json, tempfile, shutil
from pathlib import Path
from typing import Optional, Tuple, Dict, List
from datetime import datetime


# ─── CRC16 hash slot ───

CRC16_TAB = [
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50a5,0x60c6,0x70e7,
    0x8108,0x9129,0xa14a,0xb16b,0xc18c,0xd1ad,0xe1ce,0xf1ef,
]

def _crc16_byte(crc: int, byte: int) -> int:
    crc = ((crc << 8) & 0xffff) ^ CRC16_TAB[((crc >> 8) ^ byte) & 0x0F]
    crc = ((crc << 4) & 0xffff) ^ CRC16_TAB[((crc >> 4) ^ (byte >> 4)) & 0x0F]
    return crc

def key_to_slot(key: bytes) -> int:
    start = key.find(b'{')
    if start >= 0:
        end = key.find(b'}', start + 1)
        if end > start + 1:
            key = key[start + 1:end]
    crc = 0
    for b in key:
        crc = _crc16_byte(crc, b)
    return crc % 16384


class RESP:
    @staticmethod
    def encode_cmd(*args: str) -> bytes:
        parts = [f"*{len(args)}\r\n"]
        for a in args:
            parts.append(f"${len(a)}\r\n{a}\r\n")
        return "".join(parts).encode()

    @staticmethod
    def parse_line(data: bytes) -> Tuple[Optional[str], bytes]:
        if not data: return None, data
        b = data[0:1]
        end = data.find(b'\r\n')
        if end < 0: return None, data
        if b in (b'+', b'-', b':'): return data[1:end].decode(), data[end+2:]
        if b == b'$':
            length = int(data[1:end])
            if length < 0: return None, data[end+2:]
            start = end + 2
            if len(data) < start + length + 2: return None, data
            return data[start:start+length].decode(), data[start+length+2:]
        return None, data


class RespClient:
    def __init__(self, port: int):
        self.port = port
        self.reader = None
        self.writer = None

    async def connect(self) -> bool:
        try:
            self.reader, self.writer = await asyncio.wait_for(
                asyncio.open_connection("127.0.0.1", self.port), timeout=2.0)
            return True
        except Exception:
            return False

    async def close(self):
        if self.writer:
            self.writer.close()
            try: await self.writer.wait_closed()
            except Exception: pass

    async def execute(self, *args: str) -> Optional[str]:
        try:
            self.writer.write(RESP.encode_cmd(*args))
            await self.writer.drain()
            raw = await asyncio.wait_for(self.reader.read(65536), timeout=5.0)
            result, _ = RESP.parse_line(raw)
            return result if result is not None else ""
        except Exception:
            return None


class ClusterClient:
    def __init__(self, ports: List[int], slot_map: Dict[int, int]):
        self.ports = ports
        self.slot_map = slot_map
        self.clients: Dict[int, RespClient] = {}

    async def connect(self) -> bool:
        for port in self.ports:
            c = RespClient(port)
            if not await c.connect():
                return False
            self.clients[port] = c
        return True

    async def close(self):
        for c in self.clients.values():
            await c.close()

    def get_target_port(self, key: bytes) -> int:
        slot = key_to_slot(key)
        return self.slot_map.get(slot, self.ports[0])

    async def execute(self, *args: str) -> Optional[str]:
        if len(args) < 2 or args[0] in ("CLUSTER", "PING", "INFO"):
            return await self.clients[self.ports[0]].execute(*args)
        key = args[1].encode() if len(args) > 1 else b""
        target_port = self.get_target_port(key)
        return await self.clients[target_port].execute(*args)


async def setup_cluster() -> Tuple[Dict[int, RespClient], Dict[int, int], List[subprocess.Popen], List[str]]:
    """初始化3节点集群。返回 (clients, slot_map, procs, tmp_dirs)"""
    PORTS = [16379, 16380, 16381]
    server_bin = Path(__file__).resolve().parent.parent.parent / "build" / "concurrentcache-server"

    for p in PORTS:
        os.system(f"fuser -k {p}/tcp 2>/dev/null")
    time.sleep(0.5)

    # 启动节点
    procs = []
    tmp_dirs = []
    for port in PORTS:
        tmp_dir = tempfile.mkdtemp(prefix=f"cc_cluster_{port}_")
        tmp_dirs.append(tmp_dir)
        conf_dir = Path(tmp_dir) / "conf"
        conf_dir.mkdir()
        conf_file = conf_dir / "concurrentcache.conf"
        conf_file.write_text(f"""port = {port}
log_level = 4
rdb_save_interval = 0
cluster_enabled = true
cluster_bind_addr = 127.0.0.1
cluster_node_timeout = 3000
max_entries = 2000000
""")
        (Path(tmp_dir) / "dump.rdb").touch()
        proc = subprocess.Popen([str(server_bin)], cwd=tmp_dir,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(proc)
        print(f"  节点 :{port} 启动 (pid={proc.pid})")
        time.sleep(0.8)
    time.sleep(1.0)

    # 连接所有节点
    clients = {}
    for port in PORTS:
        c = RespClient(port)
        for attempt in range(15):
            if await c.connect():
                clients[port] = c
                break
            await asyncio.sleep(0.3)
        if port not in clients:
            print(f"  FAIL: 无法连接节点 :{port}")
            for p in procs: p.kill()
            return {}, {}, procs, tmp_dirs

    # 验证 PING
    for port in PORTS:
        pong = await clients[port].execute("PING")
        if pong != "PONG":
            print(f"  FAIL: 节点 :{port} PING 失败: {pong}")
            return {}, {}, procs, tmp_dirs
    print("  所有节点 PING OK ✓")

    # 组建集群
    print("  组建集群...")
    await clients[16379].execute("CLUSTER", "MEET", "127.0.0.1", "16380")
    await asyncio.sleep(0.3)
    await clients[16379].execute("CLUSTER", "MEET", "127.0.0.1", "16381")
    await asyncio.sleep(0.3)
    await clients[16380].execute("CLUSTER", "MEET", "127.0.0.1", "16381")
    await clients[16381].execute("CLUSTER", "MEET", "127.0.0.1", "16379")
    await asyncio.sleep(2.0)

    nodes_a = await clients[16379].execute("CLUSTER", "NODES")
    if not nodes_a or "16380" not in nodes_a or "16381" not in nodes_a:
        print(f"  FAIL: 集群组建失败: {nodes_a}")
        return {}, {}, procs, tmp_dirs
    print("  集群组建成功 ✓")

    # 分配槽 — 直接分配 + 验证
    print("  分配槽位...")
    slots_a = [str(i) for i in range(0, 5461)]
    slots_b = [str(i) for i in range(5461, 10923)]
    slots_c = [str(i) for i in range(10923, 16384)]

    # A: slots 0-5460
    for batch in [slots_a[i:i+500] for i in range(0, len(slots_a), 500)]:
        result = await clients[16379].execute("CLUSTER", "ADDSLOTS", *batch)
        if result != "OK":
            print(f"  FAIL: A ADDSLOTS 部分失败: {result}")
            break

    # B: slots 5461-10922
    for batch in [slots_b[i:i+500] for i in range(0, len(slots_b), 500)]:
        result = await clients[16380].execute("CLUSTER", "ADDSLOTS", *batch)
        if result != "OK":
            print(f"  FAIL: B ADDSLOTS 部分失败: {result}")
            break

    # C: slots 10923-16383
    for batch in [slots_c[i:i+500] for i in range(0, len(slots_c), 500)]:
        result = await clients[16381].execute("CLUSTER", "ADDSLOTS", *batch)
        if result != "OK":
            print(f"  FAIL: C ADDSLOTS 部分失败: {result}")
            break

    await asyncio.sleep(0.5)

    # 验证每个节点的 CLUSTER INFO
    for port, label in [(16379, "A"), (16380, "B"), (16381, "C")]:
        info = await clients[port].execute("CLUSTER", "INFO")
        if info and "cluster_slots_assigned" in info:
            try:
                slots_part = info.split("cluster_slots_assigned:")[1].split("\n")[0]
                print(f"  节点{label} 槽数: {slots_part}")
            except:
                print(f"  节点{label} INFO: {info[:100]}")

    # 构建 slot map
    slot_map = {}
    for s in range(0, 5461):
        slot_map[s] = 16379
    for s in range(5461, 10923):
        slot_map[s] = 16380
    for s in range(10923, 16384):
        slot_map[s] = 16381
    print("  槽位分配完成 ✓")

    return clients, slot_map, procs, tmp_dirs


async def cluster_stress_worker(cid: int, ports: List[int], slot_map: Dict[int, int],
                                 stop_event: asyncio.Event, stats: dict, lock: asyncio.Lock):
    """每个 worker 拥有独立的连接集"""
    client = ClusterClient(ports, slot_map)
    if not await client.connect():
        async with lock: stats["conn_fail"] += 1
        return

    ok = 0; fail = 0; lats = []
    i = 0
    try:
        while not stop_event.is_set():
            key = f"k:{cid}:{i}"
            t0 = time.monotonic()

            r = i % 100
            if r < 70:
                result = await client.execute("GET", key)
            elif r < 85:
                result = await client.execute("SET", key, f"v_{cid}_{i}")
            elif r < 95:
                result = await client.execute("DEL", key)
            else:
                result = await client.execute("INCR", f"counter:{cid % 100}")

            lat = (time.monotonic() - t0) * 1000
            lats.append(lat)
            if result is not None and not str(result).startswith("ERR") and "MOVED" not in str(result):
                ok += 1
            else:
                fail += 1
            i += 1
    except Exception:
        fail += 1
    finally:
        await client.close()

    async with lock:
        stats["ok"] += ok
        stats["fail"] += fail
        stats["lats"].extend(lats)


async def run_cluster_tier(name: str, total_concurrent: int, ports: list, slot_map: dict,
                            duration: int):
    stats = {"ok": 0, "fail": 0, "conn_fail": 0, "lats": []}
    lock = asyncio.Lock()
    stop = asyncio.Event()

    print(f"  [{name}] {total_concurrent}并发...", end=" ", flush=True)

    tasks = []
    for i in range(total_concurrent):
        tasks.append(asyncio.create_task(
            cluster_stress_worker(i, ports, slot_map, stop, stats, lock)))
        if len(tasks) % 200 == 0:
            await asyncio.sleep(0.01)

    await asyncio.sleep(duration)
    stop.set()
    await asyncio.sleep(3)

    total = stats["ok"] + stats["fail"]
    lats = sorted(stats["lats"])
    qps = total / duration if duration > 0 else 0
    p50 = lats[len(lats)//2] if lats else 0
    p99 = lats[int(len(lats)*0.99)] if lats else 0
    p999 = lats[int(len(lats)*0.999)] if lats else 0
    err = (stats["fail"] + stats["conn_fail"]) / max(total + stats["conn_fail"], 1) * 100

    status = "OK" if err < 1 else ("WARN" if err < 10 else "FAIL")
    print(f"QPS={qps:>10.1f}  p50={p50:>6.1f}ms  p99={p99:>7.1f}ms  "
          f"p999={p999:>7.1f}ms  err={err:.2f}%  [{status}]")
    return {"concurrent": total_concurrent, "qps": qps, "p50": p50, "p99": p99,
            "p999": p999, "total_ops": total, "error_rate": err}


async def main():
    print("=" * 70)
    print("  ConcurrentCache — 集群模式压力测试 (带槽路由)")
    print(f"  时间: {datetime.now().isoformat()}")
    print("=" * 70)

    clients, slot_map, procs, tmp_dirs = await setup_cluster()
    if not clients:
        for p in procs: p.kill()
        for d in tmp_dirs: shutil.rmtree(d, ignore_errors=True)
        sys.exit(1)

    try:
        PORTS = [16379, 16380, 16381]
        # 预热
        print("\n  预热 (写入 1000 keys)...")
        wc = ClusterClient(PORTS, slot_map)
        if await wc.connect():
            for i in range(1000):
                await wc.execute("SET", f"warmup:{i}", f"val_{i}")
            await wc.close()
        print("  预热完成\n")

        # ─── 压测 ───
        print(f"{'='*70}")
        print(f"  {'阶梯':>8}  {'并发':>6}  {'QPS':>10}  {'p50':>7}ms  {'p99':>8}ms  {'p999':>8}ms  {'错误率':>7}")
        print(f"  {'-'*60}")

        results = []
        tiers = [50, 100, 300, 600, 1000, 2000, 3000, 5000, 7500, 10000]

        for tier in tiers:
            r = await run_cluster_tier(f"T{tier}", tier, PORTS, slot_map, duration=12)
            results.append(r)
            await asyncio.sleep(1.5)
            if r["error_rate"] > 5:
                print(f"\n  >>> 集群破防点: {tier} 并发 <<<")
                break

        # 长期稳定性
        safe = max([r["concurrent"] for r in results if r["error_rate"] < 1] + [50])
        if safe > 50:
            print(f"\n{'='*70}")
            print(f"  集群长期稳定性: {safe} 并发, 3x30s")
            print(f"  {'-'*60}")
            for phase in range(3):
                await run_cluster_tier(f"L{phase+1}", safe, PORTS, slot_map, duration=30)
                await asyncio.sleep(2.0)

        # 报告
        print(f"\n{'='*70}")
        print(f"  {'集群压测报告':^60}")
        print(f"{'='*70}")
        print(f"  {'并发':>6}  {'QPS':>10}  {'p50':>7}ms  {'p99':>8}ms  {'p999':>8}ms  {'ops':>8}  {'错误率':>7}")
        print(f"  {'-'*60}")
        max_safe = 0; max_qps = 0.0
        for r in results:
            print(f"  {r['concurrent']:>6}  {r['qps']:>10.1f}  {r['p50']:>6.1f}   {r['p99']:>7.1f}   {r['p999']:>7.1f}   {r['total_ops']:>6}   {r['error_rate']:>5.1f}%")
            if r["error_rate"] < 1: max_safe = r["concurrent"]
            if r["qps"] > max_qps: max_qps = r["qps"]

        print(f"\n  ╔══════════════════════════════════════╗")
        print(f"  ║  集群安全并发: {max_safe:>6} 客户端         ║")
        print(f"  ║  集群峰值QPS:  {max_qps:>10.1f}           ║")
        print(f"  ╚══════════════════════════════════════╝")

        report_path = Path(__file__).parent / "cluster_stress_report.json"
        report_path.write_text(json.dumps({
            "timestamp": datetime.now().isoformat(),
            "type": "cluster_3node",
            "safe_concurrent": max_safe, "peak_qps": max_qps,
            "tiers": results
        }, indent=2, ensure_ascii=False))
        print(f"\n  报告: {report_path}")

    finally:
        print(f"\n  停止节点...")
        for c in clients.values():
            await c.close()
        for p in procs:
            p.send_signal(signal.SIGTERM)
            try: p.wait(timeout=8)
            except: p.kill()
        for d in tmp_dirs:
            shutil.rmtree(d, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
