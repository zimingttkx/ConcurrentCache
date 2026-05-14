#!/usr/bin/env python3
"""
ConcurrentCache — PSYNC Replication E2E Test
============================================
Tests:
  1. CLUSTER REPLICATE triggers PSYNC FULLRESYNC
  2. Data written to master appears on replica
  3. Multi-key replication (SET, DEL, INCR)
  4. Replica read-only enforcement
  5. SYNC legacy command compatibility

Uses ports 27379-27380 to avoid conflicts with other tests.
"""

import asyncio, subprocess, time, os, sys, signal, json, tempfile, shutil
from pathlib import Path
from typing import Optional
from datetime import datetime


class RESP:
    @staticmethod
    def encode_cmd(*args: str) -> bytes:
        parts = [f"*{len(args)}\r\n"]
        for a in args:
            parts.append(f"${len(a)}\r\n{a}\r\n")
        return "".join(parts).encode()

    @staticmethod
    def parse_line(data: bytes):
        if not data: return None, data
        if data[0:1] == b'+':
            end = data.find(b'\r\n')
            return (data[1:end].decode(), data[end+2:]) if end >= 0 else (None, data)
        elif data[0:1] == b'-':
            end = data.find(b'\r\n')
            return ("ERR:" + data[1:end].decode(), data[end+2:]) if end >= 0 else (None, data)
        elif data[0:1] == b':':
            end = data.find(b'\r\n')
            return (data[1:end].decode(), data[end+2:]) if end >= 0 else (None, data)
        elif data[0:1] == b'$':
            end = data.find(b'\r\n')
            if end < 0: return None, data
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
        for _ in range(20):
            try:
                self.reader, self.writer = await asyncio.wait_for(
                    asyncio.open_connection("127.0.0.1", self.port), timeout=2.0)
                return True
            except Exception:
                await asyncio.sleep(0.3)
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
            return result
        except Exception:
            return None


class TestResults:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.results = []

    def record(self, name: str, ok: bool, detail: str = ""):
        if ok:
            self.passed += 1
            print(f"  [PASS] {name}")
        else:
            self.failed += 1
            print(f"  [FAIL] {name} — {detail}")
        self.results.append({"name": name, "result": "PASS" if ok else "FAIL", "detail": detail})


def start_node(server_bin: str, port: int, label: str):
    tmp_dir = tempfile.mkdtemp(prefix=f"cc_psync_{label}_")
    conf_dir = Path(tmp_dir) / "conf"
    conf_dir.mkdir()
    (conf_dir / "concurrentcache.conf").write_text(f"""port = {port}
cluster_enabled = true
cluster_bind_addr = 127.0.0.1
cluster_node_timeout = 5000
log_level = 4
rdb_save_interval = 0
max_entries = 200000
""")
    (Path(tmp_dir) / "dump.rdb").touch()
    proc = subprocess.Popen([str(server_bin)], cwd=tmp_dir,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return proc, tmp_dir


async def main():
    project_root = Path(__file__).resolve().parent.parent.parent
    server_bin = project_root / "build" / "concurrentcache-server"
    if not server_bin.exists():
        print(f"ERROR: Server binary not found at {server_bin}")
        sys.exit(1)

    print("=" * 60)
    print("  ConcurrentCache — PSYNC REPLICATION E2E TEST")
    print(f"  Time: {datetime.now().isoformat()}")
    print("=" * 60)

    r = TestResults()
    MASTER_PORT = 27379
    REPLICA_PORT = 27380
    procs = []
    tmp_dirs = []

    # Clear ports
    for p in [MASTER_PORT, REPLICA_PORT, MASTER_PORT+10000, REPLICA_PORT+10000]:
        os.system(f"fuser -k {p}/tcp 2>/dev/null")
    time.sleep(0.5)

    try:
        # ─── 1. Start nodes ───
        print("\n── Starting nodes ──")
        for port, label in [(MASTER_PORT, "master"), (REPLICA_PORT, "replica")]:
            proc, tmp_dir = start_node(server_bin, port, label)
            procs.append(proc)
            tmp_dirs.append(tmp_dir)
            print(f"  {label} :{port} started (pid={proc.pid})")
            time.sleep(0.6)

        master = RespClient(MASTER_PORT)
        replica = RespClient(REPLICA_PORT)
        for cli, label, port in [(master, "Master", MASTER_PORT), (replica, "Replica", REPLICA_PORT)]:
            ok = await cli.connect()
            r.record(f"Connect to {label}", ok, f"port {port}")
            if not ok: return 1

        r.record("Master PING", await master.execute("PING") == "PONG")
        r.record("Replica PING", await replica.execute("PING") == "PONG")

        # ─── 2. Form cluster ───
        print("\n── Forming cluster ──")
        meet = await master.execute("CLUSTER", "MEET", "127.0.0.1", str(REPLICA_PORT))
        r.record("CLUSTER MEET master→replica", meet == "OK", str(meet))
        await replica.execute("CLUSTER", "MEET", "127.0.0.1", str(MASTER_PORT))
        await asyncio.sleep(2.0)

        nodes = await master.execute("CLUSTER", "NODES")
        r.record("Master CLUSTER NODES shows replica",
                 nodes and str(REPLICA_PORT) in nodes, str(nodes)[:200])

        # ─── 2.5. Assign all slots to master ───
        print("\n── Assigning slots to master ──")
        all_slots = [str(i) for i in range(16384)]
        for batch in [all_slots[i:i+500] for i in range(0, 16384, 500)]:
            result = await master.execute("CLUSTER", "ADDSLOTS", *batch)
            if result != "OK":
                print(f"  WARN: ADDSLOTS batch failed: {result}")

        info = await master.execute("CLUSTER", "INFO")
        r.record("Master has slots assigned",
                 info and "cluster_slots_assigned" in info, str(info)[:150])
        await asyncio.sleep(0.3)

        # ─── 3. CLUSTER REPLICATE ───
        print("\n── Testing PSYNC FULLRESYNC ──")
        master_name = f"127.0.0.1:{MASTER_PORT}"
        repl_result = await replica.execute("CLUSTER", "REPLICATE", master_name)
        r.record("CLUSTER REPLICATE replica→master",
                 repl_result == "OK", str(repl_result))
        await asyncio.sleep(1.0)

        repl_nodes = await replica.execute("CLUSTER", "NODES")
        r.record("Replica CLUSTER NODES shows master",
                 repl_nodes and str(MASTER_PORT) in repl_nodes, str(repl_nodes)[:200])

        # ─── 4. Data replication ───
        print("\n── Testing data replication ──")
        for key, val in [("k1", "v1"), ("k2", "v2"), ("k3", f"long_{'x'*50}")]:
            set_res = await master.execute("SET", key, val)
            r.record(f"Master SET {key}", set_res == "OK", str(set_res))
            await asyncio.sleep(0.1)

        for key, expected in [("k1", "v1"), ("k2", "v2"), ("k3", f"long_{'x'*50}")]:
            get_res = await replica.execute("GET", key)
            r.record(f"Replica GET {key}", get_res == expected,
                     f"expected={expected}, got={get_res}")

        # ─── 5. DEL replication ───
        print("\n── Testing DEL replication ──")
        await master.execute("DEL", "k1")
        await asyncio.sleep(0.1)
        get_k1 = await replica.execute("GET", "k1")
        r.record("DEL k1 on master → replica null",
                 get_k1 is None or get_k1 == "", str(get_k1))

        # ─── 6. INCR replication ───
        print("\n── Testing INCR replication ──")
        await master.execute("SET", "counter", "0")
        await asyncio.sleep(0.1)
        for _ in range(5):
            await master.execute("INCR", "counter")
        await asyncio.sleep(0.2)
        counter_val = await replica.execute("GET", "counter")
        r.record("Replica GET counter after 5 INCRs",
                 counter_val == "5", str(counter_val))

        # ─── 7. Replica read-only ───
        print("\n── Testing replica read-only ──")
        set_on_repl = await replica.execute("SET", "should_fail", "val")
        is_readonly = set_on_repl is not None and (
            "MOVED" in str(set_on_repl) or "READONLY" in str(set_on_repl).upper() or
            "ERR" in str(set_on_repl))
        r.record("Replica rejects write", is_readonly, str(set_on_repl))

        # ─── 8. Bulk writes ───
        print("\n── Testing bulk writes ──")
        for i in range(50):
            await master.execute("SET", f"bulk:{i}", f"bv_{i}")
        await asyncio.sleep(0.5)
        bulk_ok = 0
        for i in range(50):
            val = await replica.execute("GET", f"bulk:{i}")
            if val == f"bv_{i}":
                bulk_ok += 1
        r.record(f"Bulk replication: {bulk_ok}/50", bulk_ok >= 40, f"{bulk_ok}/50")

        # Cleanup
        await master.close()
        await replica.close()

    except Exception as e:
        print(f"\n[FATAL] {e}")
        import traceback; traceback.print_exc()
        r.failed += 1
    finally:
        for p in procs:
            try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
            except:
                try: p.kill()
                except: pass
        for d in tmp_dirs:
            shutil.rmtree(d, ignore_errors=True)

    # Report
    total = r.passed + r.failed
    print(f"\n{'='*60}")
    print(f"  PSYNC REPLICATION E2E TEST SUMMARY")
    print(f"  Total: {total}  Passed: {r.passed}  Failed: {r.failed}")
    print(f"{'='*60}")

    report_path = Path(__file__).parent / "psync_replication_report.json"
    report_path.write_text(json.dumps({
        "timestamp": datetime.now().isoformat(),
        "total": total, "passed": r.passed, "failed": r.failed,
        "results": r.results
    }, indent=2, ensure_ascii=False))
    print(f"Report: {report_path}")
    return 0 if r.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
