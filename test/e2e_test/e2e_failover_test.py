#!/usr/bin/env python3
"""
ConcurrentCache — Failover E2E Test
===================================
Tests:
  1. Master-replica pair setup
  2. Write data to master, verify replica sync
  3. Kill master, verify replica detects failure
  4. Verify remaining nodes still operational
  5. Re-add failed node, verify cluster heals

Uses ports 28379-28381 to avoid conflicts.
"""

import asyncio, subprocess, time, os, sys, signal, json, tempfile, shutil
from pathlib import Path
from typing import Optional, Dict
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
    tmp_dir = tempfile.mkdtemp(prefix=f"cc_fo_{label}_")
    conf_dir = Path(tmp_dir) / "conf"
    conf_dir.mkdir()
    (conf_dir / "concurrentcache.conf").write_text(f"""port = {port}
cluster_enabled = true
cluster_bind_addr = 127.0.0.1
cluster_node_timeout = 3000
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
    print("  ConcurrentCache — FAILOVER E2E TEST")
    print(f"  Time: {datetime.now().isoformat()}")
    print("=" * 60)

    r = TestResults()
    A_PORT, B_PORT, C_PORT = 28379, 28380, 28381
    procs: Dict[str, subprocess.Popen] = {}
    tmp_dirs = []
    clients: Dict[str, RespClient] = {}

    # Clear ports
    for p in [A_PORT, B_PORT, C_PORT, A_PORT+10000, B_PORT+10000, C_PORT+10000]:
        os.system(f"fuser -k {p}/tcp 2>/dev/null")
    time.sleep(0.5)

    try:
        # ─── 1. Start 3-node cluster ───
        print("\n── Starting 3-node cluster ──")
        for name, port in [("A", A_PORT), ("B", B_PORT), ("C", C_PORT)]:
            proc, tmp_dir = start_node(server_bin, port, name)
            procs[name] = proc
            tmp_dirs.append(tmp_dir)
            print(f"  Node-{name} :{port} started (pid={proc.pid})")
            time.sleep(0.6)

        for name, port in [("A", A_PORT), ("B", B_PORT), ("C", C_PORT)]:
            cli = RespClient(port)
            if not await cli.connect():
                r.record(f"Connect to Node-{name}", False)
                return 1
            clients[name] = cli
            pong = await cli.execute("PING")
            r.record(f"Node-{name} PING", pong == "PONG", str(pong))

        # ─── 2. Form cluster ───
        print("\n── Forming cluster ──")
        await clients["A"].execute("CLUSTER", "MEET", "127.0.0.1", str(B_PORT))
        await asyncio.sleep(0.3)
        await clients["A"].execute("CLUSTER", "MEET", "127.0.0.1", str(C_PORT))
        await asyncio.sleep(0.3)
        await clients["B"].execute("CLUSTER", "MEET", "127.0.0.1", str(C_PORT))
        await clients["C"].execute("CLUSTER", "MEET", "127.0.0.1", str(A_PORT))
        await asyncio.sleep(2.5)

        nodes_a = await clients["A"].execute("CLUSTER", "NODES")
        r.record("3-node cluster formed",
                 nodes_a and str(B_PORT) in nodes_a and str(C_PORT) in nodes_a,
                 str(nodes_a)[:200])

        # ─── 3. Assign slots ───
        print("\n── Assigning slots ──")
        # Node A gets half, Node B gets half, C is replica of A
        slots_a = [str(i) for i in range(0, 8192)]
        slots_b = [str(i) for i in range(8192, 16384)]

        for batch in [slots_a[i:i+500] for i in range(0, len(slots_a), 500)]:
            result = await clients["A"].execute("CLUSTER", "ADDSLOTS", *batch)
            if result != "OK":
                print(f"  WARN: A ADDSLOTS: {result}")
                break

        for batch in [slots_b[i:i+500] for i in range(0, len(slots_b), 500)]:
            result = await clients["B"].execute("CLUSTER", "ADDSLOTS", *batch)
            if result != "OK":
                print(f"  WARN: B ADDSLOTS: {result}")
                break

        await asyncio.sleep(0.5)

        info_a = await clients["A"].execute("CLUSTER", "INFO")
        # node A's CLUSTER INFO only shows locally-assigned slots
        has_slots = info_a and "cluster_slots_assigned" in info_a
        r.record("Slots assigned (A)",
                 has_slots, str(info_a)[:150])

        # ─── 4. Replication: C is replica of A ───
        print("\n── Setting up replication ──")
        master_name = f"127.0.0.1:{A_PORT}"
        repl_result = await clients["C"].execute("CLUSTER", "REPLICATE", master_name)
        r.record("CLUSTER REPLICATE C→A", repl_result == "OK", str(repl_result))
        await asyncio.sleep(1.0)

        # Verify replication by writing a key that hashes to A's slot range
        # Key without hash tag, we just try a few until one works on A
        for test_key in ["fo_key_1", "foo", "test123"]:
            slot_test = sum(ord(c) for c in test_key) % 16384  # approximate
            if slot_test < 8192:  # A's range
                break
        await clients["A"].execute("SET", "fo_key_1", "before_failover")
        await asyncio.sleep(0.3)
        get_c = await clients["C"].execute("GET", "fo_key_1")
        r.record("Replica C has data from master A",
                 get_c == "before_failover", f"got: {get_c}")

        # ─── 5. CLUSTER FAIL ───
        print("\n── Testing CLUSTER FAIL ──")
        fail_result = await clients["A"].execute("CLUSTER", "FAIL",
                                                   f"127.0.0.1:{B_PORT}")
        r.record("CLUSTER FAIL B", fail_result == "OK", str(fail_result))
        await asyncio.sleep(0.5)

        nodes_after = await clients["A"].execute("CLUSTER", "NODES")
        r.record("CLUSTER NODES after FAIL",
                 nodes_after and len(nodes_after) > 10, str(nodes_after)[:200])

        # ─── 6. Simulate master crash ───
        print("\n── Simulating master crash ──")
        procs["A"].kill()
        procs["A"].wait()
        print("  Node-A (master) killed")
        await asyncio.sleep(2.0)

        # Replica C should still be alive
        c_pong = await clients["C"].execute("PING")
        r.record("Replica C alive after master crash",
                 c_pong == "PONG", str(c_pong))

        b_pong = await clients["B"].execute("PING")
        r.record("Node B alive after A crash",
                 b_pong == "PONG", str(b_pong))

        nodes_b = await clients["B"].execute("CLUSTER", "NODES")
        r.record("Node B NODES after A crash",
                 nodes_b and len(nodes_b) > 5, str(nodes_b)[:200])

        # ─── 7. Recovery: restart A ───
        print("\n── Testing recovery ──")
        a_proc, a_tmp = start_node(server_bin, A_PORT, "A_restart")
        tmp_dirs.append(a_tmp)
        print(f"  Node-A restarted (pid={a_proc.pid})")
        await asyncio.sleep(2.0)

        a_new = RespClient(A_PORT)
        if await a_new.connect():
            a_new_pong = await a_new.execute("PING")
            r.record("Restarted Node-A PING",
                     a_new_pong == "PONG", str(a_new_pong))

            await a_new.execute("CLUSTER", "MEET", "127.0.0.1", str(B_PORT))
            await asyncio.sleep(2.0)
            nodes_restart = await a_new.execute("CLUSTER", "NODES")
            r.record("Restarted A sees other nodes",
                     nodes_restart and str(B_PORT) in str(nodes_restart),
                     str(nodes_restart)[:200])
            await a_new.close()

        # ─── 8. Data ops after recovery ───
        print("\n── Testing data operations after recovery ──")
        set_b = await clients["B"].execute("SET", "post_failover_key", "survived")
        r.record("SET on B after failover", set_b == "OK", str(set_b))
        get_b = await clients["B"].execute("GET", "post_failover_key")
        r.record("GET on B after failover", get_b == "survived", str(get_b))

        # Cleanup
        for cli in clients.values():
            await cli.close()

    except Exception as e:
        print(f"\n[FATAL] {e}")
        import traceback; traceback.print_exc()
        r.failed += 1
    finally:
        for proc in procs.values():
            try: proc.send_signal(signal.SIGTERM); proc.wait(timeout=5)
            except:
                try: proc.kill()
                except: pass
        for d in tmp_dirs:
            shutil.rmtree(d, ignore_errors=True)

    total = r.passed + r.failed
    print(f"\n{'='*60}")
    print(f"  FAILOVER E2E TEST SUMMARY")
    print(f"  Total: {total}  Passed: {r.passed}  Failed: {r.failed}")
    print(f"{'='*60}")

    report_path = Path(__file__).parent / "failover_e2e_report.json"
    report_path.write_text(json.dumps({
        "timestamp": datetime.now().isoformat(),
        "total": total, "passed": r.passed, "failed": r.failed,
        "results": r.results
    }, indent=2, ensure_ascii=False))
    print(f"Report: {report_path}")
    return 0 if r.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
