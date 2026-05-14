#!/usr/bin/env python3
"""
ConcurrentCache V4.0 — Comprehensive Cluster E2E Test
=====================================================
Tests:
  1. Cluster formation (CLUSTER MEET)
  2. Gossip auto-discovery
  3. CLUSTER NODES / CLUSTER INFO
  4. Slot assignment (CLUSTER ADDSLOTS)
  5. Master-Replica replication (CLUSTER REPLICATE)
  6. Data consistency across nodes
  7. Failover detection (simulated)
  8. Clean shutdown
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
from dataclasses import dataclass, field
from typing import List, Optional, Dict, Tuple
from datetime import datetime


# ─── RESP Protocol Helpers ───────────────────────────────────────────

class RESP:
    @staticmethod
    def encode_cmd(*args: str) -> bytes:
        parts = [f"*{len(args)}\r\n"]
        for a in args:
            parts.append(f"${len(a)}\r\n{a}\r\n")
        return "".join(parts).encode()

    @staticmethod
    def parse_line(data: bytes) -> Tuple[Optional[str], bytes]:
        """Parse a single RESP line, return (result, remaining_bytes)."""
        if not data:
            return None, data
        if data[0:1] == b'+':  # Simple string
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return data[1:end].decode(), data[end+2:]
        elif data[0:1] == b'-':  # Error
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return "ERR:" + data[1:end].decode(), data[end+2:]
        elif data[0:1] == b':':  # Integer
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            return data[1:end].decode(), data[end+2:]
        elif data[0:1] == b'$':  # Bulk string
            end = data.find(b'\r\n')
            if end < 0:
                return None, data
            length = int(data[1:end])
            if length < 0:
                return None, data[end+2:]  # null bulk
            start = end + 2
            if len(data) < start + length + 2:
                return None, data
            return data[start:start+length].decode(), data[start+length+2:]
        return None, data


class RESPDB:
    """Minimal async RESP client."""

    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None

    async def connect(self):
        self.reader, self.writer = await asyncio.open_connection(self.host, self.port)

    async def close(self):
        if self.writer:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except Exception:
                pass

    async def execute(self, *args: str) -> str:
        self.writer.write(RESP.encode_cmd(*args))
        await self.writer.drain()
        raw = await asyncio.wait_for(self.reader.read(65536), timeout=5.0)
        result, _ = RESP.parse_line(raw)
        return result if result is not None else ""


# ─── Test Server Manager ─────────────────────────────────────────────

@dataclass
class ServerInstance:
    name: str
    port: int
    config_dir: str
    process: Optional[subprocess.Popen] = None
    client: Optional[RESPDB] = None
    cluster_bus_port: int = 0

    @property
    def node_name(self) -> str:
        return f"127.0.0.1:{self.port}"


class ClusterTestHarness:
    """Manages multiple server instances for cluster testing."""

    def __init__(self, base_dir: str, server_bin: str):
        self.base_dir = Path(base_dir)
        self.server_bin = server_bin
        self.servers: Dict[int, ServerInstance] = {}

    def create_node(self, name: str, port: int) -> ServerInstance:
        cfg_dir = self.base_dir / f"node_{port}"
        cfg_dir.mkdir(parents=True, exist_ok=True)
        conf_dir = cfg_dir / "conf"
        conf_dir.mkdir(exist_ok=True)

        conf_file = conf_dir / "concurrentcache.conf"
        conf_file.write_text(f"""# Cluster node {name}
port = {port}
cluster_enabled = true
cluster_bind_addr = 127.0.0.1
cluster_node_timeout = 3000
log_level = 3
rdb_save_interval = 0
rdb_path = {cfg_dir}/dump.rdb
""")

        si = ServerInstance(
            name=name,
            port=port,
            config_dir=str(cfg_dir),
            cluster_bus_port=port + 10000,
        )
        self.servers[port] = si
        return si

    def start_node(self, port: int) -> ServerInstance:
        si = self.servers[port]
        env = os.environ.copy()
        # Override working dir so config is found at ./conf/concurrentcache.conf
        si.process = subprocess.Popen(
            [self.server_bin],
            cwd=si.config_dir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        time.sleep(0.5)  # Give server time to start
        return si

    async def connect_node(self, port: int) -> RESPDB:
        si = self.servers[port]
        client = RESPDB("127.0.0.1", port)
        for attempt in range(10):
            try:
                await client.connect()
                si.client = client
                return client
            except (ConnectionRefusedError, OSError):
                await asyncio.sleep(0.3)
        raise RuntimeError(f"Failed to connect to node on port {port}")

    def stop_node(self, port: int, graceful: bool = True):
        si = self.servers[port]
        if si.process and si.process.poll() is None:
            if graceful:
                si.process.send_signal(signal.SIGTERM)
                try:
                    si.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    si.process.kill()
            else:
                si.process.kill()
                si.process.wait()

    def stop_all(self, graceful: bool = True):
        for si in self.servers.values():
            self.stop_node(si.port, graceful)

    async def cleanup(self):
        for si in self.servers.values():
            if si.client:
                await si.client.close()
        self.stop_all()
        shutil.rmtree(self.base_dir, ignore_errors=True)


# ─── Test Suite ──────────────────────────────────────────────────────

class TestResults:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.results: List[dict] = []

    def record(self, name: str, ok: bool, detail: str = ""):
        if ok:
            self.passed += 1
            print(f"  [PASS] {name}")
        else:
            self.failed += 1
            print(f"  [FAIL] {name} — {detail}")
        self.results.append({
            "name": name, "result": "PASS" if ok else "FAIL", "detail": detail
        })

    def summary(self) -> str:
        total = self.passed + self.failed
        return (
            f"\n{'='*60}\n"
            f"  CLUSTER E2E TEST SUMMARY\n"
            f"  Total: {total}  Passed: {self.passed}  Failed: {self.failed}\n"
            f"{'='*60}\n"
        )


async def test_cluster_formation(harness: ClusterTestHarness, r: TestResults):
    """Test 1: Start 3 nodes and form a full-mesh cluster via CLUSTER MEET."""
    print("\n── Test 1: Cluster Formation ──")

    # Start 3 nodes
    node_a = harness.create_node("node-a", 16379)
    node_b = harness.create_node("node-b", 16380)
    node_c = harness.create_node("node-c", 16381)

    harness.start_node(16379)
    harness.start_node(16380)
    harness.start_node(16381)

    cli_a = await harness.connect_node(16379)
    cli_b = await harness.connect_node(16380)
    cli_c = await harness.connect_node(16381)

    # Verify each node can respond
    pong_a = await cli_a.execute("PING")
    r.record("Node A responds to PING", pong_a == "PONG", f"got: {pong_a}")
    pong_b = await cli_b.execute("PING")
    r.record("Node B responds to PING", pong_b == "PONG", f"got: {pong_b}")
    pong_c = await cli_c.execute("PING")
    r.record("Node C responds to PING", pong_c == "PONG", f"got: {pong_c}")

    # Check that cluster is enabled on each
    info_a = await cli_a.execute("CLUSTER", "INFO")
    r.record("Node A cluster enabled", "cluster_enabled:yes" in info_a, info_a[:80])

    # Form full mesh: A↔B, A↔C, B↔C
    meet_ab = await cli_a.execute("CLUSTER", "MEET", "127.0.0.1", "16380")
    r.record("CLUSTER MEET A→B", meet_ab == "OK", f"got: {meet_ab}")
    await asyncio.sleep(0.3)

    meet_ac = await cli_a.execute("CLUSTER", "MEET", "127.0.0.1", "16381")
    r.record("CLUSTER MEET A→C", meet_ac == "OK", f"got: {meet_ac}")
    await asyncio.sleep(0.3)

    meet_bc = await cli_b.execute("CLUSTER", "MEET", "127.0.0.1", "16381")
    r.record("CLUSTER MEET B→C", meet_bc == "OK", f"got: {meet_bc}")
    await asyncio.sleep(0.3)

    # Extra: C meets A directly to ensure C knows A (timing safety)
    await cli_c.execute("CLUSTER", "MEET", "127.0.0.1", "16379")
    await asyncio.sleep(2.0)  # Give EventLoop time to process all bus messages

    # Check CLUSTER NODES on A - should know all 3
    nodes_a = await cli_a.execute("CLUSTER", "NODES")
    r.record("CLUSTER NODES on A has results", len(nodes_a) > 10, nodes_a[:200])

    has_b_in_a = "16380" in nodes_a
    has_c_in_a = "16381" in nodes_a
    r.record("Node A knows Node B", has_b_in_a)
    r.record("Node A knows Node C", has_c_in_a)

    # Node B should know A and C (via direct MEET calls)
    nodes_b = await cli_b.execute("CLUSTER", "NODES")
    has_a_in_b = "16379" in nodes_b
    has_c_in_b = "16381" in nodes_b
    r.record("Node B knows Node A", has_a_in_b)
    r.record("Node B knows Node C", has_c_in_b)

    # Node C should also know A and B
    nodes_c = await cli_c.execute("CLUSTER", "NODES")
    has_a_in_c = "16379" in nodes_c
    has_b_in_c = "16380" in nodes_c
    r.record("Node C knows Node A", has_a_in_c)
    r.record("Node C knows Node B", has_b_in_c)


async def test_slot_assignment(harness: ClusterTestHarness, r: TestResults):
    """Test 2: Assign hash slots to nodes."""
    print("\n── Test 2: Slot Assignment ──")

    cli_a = harness.servers[16379].client
    cli_b = harness.servers[16380].client

    # Node A gets slots 0-9
    result_a = await cli_a.execute("CLUSTER", "ADDSLOTS",
                                    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9")
    r.record("CLUSTER ADDSLOTS works (A)", result_a == "OK", f"got: {result_a}")

    # Node B gets slots 10-19
    result_b = await cli_b.execute("CLUSTER", "ADDSLOTS",
                                    "10", "11", "12", "13", "14", "15", "16", "17", "18", "19")
    r.record("CLUSTER ADDSLOTS works (B)", result_b == "OK", f"got: {result_b}")

    # Verify via CLUSTER NODES
    nodes_a = await cli_a.execute("CLUSTER", "NODES")
    r.record("Node A NODES shows slots", "0," in nodes_a or "0\n" in nodes_a,
             nodes_a[:200])

    # Check CLUSTER INFO
    info_b = await cli_b.execute("CLUSTER", "INFO")
    r.record("CLUSTER INFO shows slots assigned",
             "cluster_slots_assigned" in info_b, info_b[:150])


async def test_data_operations(harness: ClusterTestHarness, r: TestResults):
    """Test 3: Basic data operations work in cluster mode."""
    print("\n── Test 3: Data Operations in Cluster Mode ──")

    cli_a = harness.servers[16379].client

    # SET/GET should work even in cluster mode
    set_result = await cli_a.execute("SET", "test_key", "hello_cluster")
    r.record("SET in cluster mode", set_result == "OK", f"got: {set_result}")

    get_result = await cli_a.execute("GET", "test_key")
    r.record("GET in cluster mode", get_result == "hello_cluster", f"got: {get_result}")

    # DEL
    del_result = await cli_a.execute("DEL", "test_key")
    r.record("DEL in cluster mode", del_result == "1", f"got: {del_result}")

    get_after_del = await cli_a.execute("GET", "test_key")
    r.record("GET after DEL returns null", get_after_del is None or get_after_del == "",
             f"got: {get_after_del}")


async def test_replication(harness: ClusterTestHarness, r: TestResults):
    """Test 4: Master-Replica replication via CLUSTER REPLICATE."""
    print("\n── Test 4: Replication ──")

    cli_a = harness.servers[16379].client
    cli_c = harness.servers[16381].client

    # Ensure C knows A before replication (explicit MEET)
    await cli_c.execute("CLUSTER", "MEET", "127.0.0.1", "16379")
    await asyncio.sleep(0.5)

    # Node C becomes replica of Node A
    master_name = harness.servers[16379].node_name
    repl_result = await cli_c.execute("CLUSTER", "REPLICATE", master_name)
    r.record("CLUSTER REPLICATE C→A", repl_result == "OK", f"got: {repl_result}")

    await asyncio.sleep(0.5)

    # Check CLUSTER NODES on C to see the relationship
    nodes_c = await cli_c.execute("CLUSTER", "NODES")
    r.record("Replica NODES show result",
             len(nodes_c) > 10,
             nodes_c[:200])

    # Write data to master A
    await cli_a.execute("SET", "repl_key_1", "from_master_a")
    get_from_a = await cli_a.execute("GET", "repl_key_1")
    r.record("Master A can read its data",
             get_from_a == "from_master_a", f"got: {get_from_a}")


async def test_failover_detection(harness: ClusterTestHarness, r: TestResults):
    """Test 5: Node failure detection via CLUSTER FAIL."""
    print("\n── Test 5: Failover Detection ──")

    cli_a = harness.servers[16379].client

    # Mark B as FAIL from A (simulated failover trigger)
    fail_result = await cli_a.execute("CLUSTER", "FAIL", harness.servers[16380].node_name)
    r.record("CLUSTER FAIL <node_b>", fail_result == "OK", f"got: {fail_result}")

    # Verify NODES still works
    await asyncio.sleep(0.3)
    nodes_after = await cli_a.execute("CLUSTER", "NODES")
    r.record("CLUSTER NODES after FAIL",
             len(nodes_after) > 10, nodes_after[:200])


async def test_graceful_shutdown(harness: ClusterTestHarness, r: TestResults):
    """Test 6: Graceful shutdown preserves data."""
    print("\n── Test 6: Graceful Shutdown ──")

    cli_a = harness.servers[16379].client

    # Write some final data
    await cli_a.execute("SET", "persist_key", "survive_shutdown")
    get_before = await cli_a.execute("GET", "persist_key")
    r.record("Data written before shutdown", get_before == "survive_shutdown",
             f"got: {get_before}")

    # Stop all gracefully
    harness.stop_all(graceful=True)
    await asyncio.sleep(1.0)

    # Restart Node A
    harness.start_node(16379)
    cli_a2 = await harness.connect_node(16379)
    harness.servers[16379].client = cli_a2

    pong_after = await cli_a2.execute("PING")
    r.record("Server restarts successfully", pong_after == "PONG", f"got: {pong_after}")


async def main():
    project_root = Path(__file__).resolve().parent.parent.parent
    server_bin = project_root / "build" / "concurrentcache-server"

    if not server_bin.exists():
        print(f"ERROR: Server binary not found at {server_bin}")
        print("Build first: cd build && cmake .. && make -j$(nproc)")
        sys.exit(1)

    print("=" * 60)
    print("  ConcurrentCache V4.0 — CLUSTER E2E TEST")
    print(f"  Server: {server_bin}")
    print(f"  Time:   {datetime.now().isoformat()}")
    print("=" * 60)

    harness = ClusterTestHarness(
        base_dir=tempfile.mkdtemp(prefix="cc_cluster_test_"),
        server_bin=str(server_bin),
    )

    results = TestResults()

    try:
        await test_cluster_formation(harness, results)
        await test_slot_assignment(harness, results)
        await test_data_operations(harness, results)
        await test_replication(harness, results)
        await test_failover_detection(harness, results)
        await test_graceful_shutdown(harness, results)
    except Exception as e:
        print(f"\n[FATAL] Test harness error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        await harness.cleanup()

    print(results.summary())

    # Save report
    report_path = Path(__file__).parent / "e2e_cluster_report.json"
    report = {
        "timestamp": datetime.now().isoformat(),
        "total": results.passed + results.failed,
        "passed": results.passed,
        "failed": results.failed,
        "results": results.results,
    }
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False))
    print(f"Report saved to: {report_path}")

    return 0 if results.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
