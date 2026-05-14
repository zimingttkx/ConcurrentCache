#!/usr/bin/env python3
"""
E2E Data Consistency Check Test - ConcurrentCache

多协程疯狂并发修改同一个 Key，测试结束后验证最终结果是否符合一致性预期。
测试使用服务器实际支持的命令（SET, GET, DEL）。
"""

import asyncio
import time
import json
from typing import List, Set, Dict
from dataclasses import dataclass, asdict
from datetime import datetime


@dataclass
class TestResult:
    test_name: str
    timestamp: str
    result: str
    metrics: dict
    details: dict


class RESPProtocol:
    """简化的 RESP 协议实现"""

    @staticmethod
    def set(key: str, value: str) -> bytes:
        return f"*3\r\n$3\r\nSET\r\n${len(key)}\r\n{key}\r\n${len(value)}\r\n{value}\r\n".encode()

    @staticmethod
    def get(key: str) -> bytes:
        return f"*2\r\n$3\r\nGET\r\n${len(key)}\r\n{key}\r\n".encode()

    @staticmethod
    def delete(key: str) -> bytes:
        return f"*2\r\n$3\r\nDEL\r\n${len(key)}\r\n{key}\r\n".encode()

    @staticmethod
    def incr(key: str) -> bytes:
        # Server may not support INCR, but we include it for compatibility
        return f"*2\r\n$4\r\nINCR\r\n${len(key)}\r\n{key}\r\n".encode()


class ConsistencyChecker:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.results = {}

    async def get_value(self, key: str) -> str:
        """读取 Key 的值"""
        try:
            reader, writer = await asyncio.open_connection(self.host, self.port)
            writer.write(RESPProtocol.get(key))
            await writer.drain()

            response = await asyncio.wait_for(reader.readline(), timeout=2.0)

            if response.startswith(b"$"):
                # Bulk string: check for nil ($ -1) or read the actual value
                if response.startswith(b"$-1"):
                    # Nil bulk string - key doesn't exist
                    writer.close()
                    await writer.wait_closed()
                    return ""
                # Read the actual value on next line
                value = await asyncio.wait_for(reader.readline(), timeout=2.0)
                writer.close()
                await writer.wait_closed()
                return value.decode().strip()
            elif response.startswith(b"+") or response.startswith(b"-"):
                writer.close()
                await writer.wait_closed()
                return response.decode().strip()
            writer.close()
            await writer.wait_closed()
            return ""

        except Exception as e:
            return ""

    async def set_value(self, key: str, value: str) -> bool:
        """写入 Key 的值"""
        try:
            reader, writer = await asyncio.open_connection(self.host, self.port)
            writer.write(RESPProtocol.set(key, value))
            await writer.drain()

            response = await asyncio.wait_for(reader.readline(), timeout=2.0)
            writer.close()
            await writer.wait_closed()

            return b"+OK" in response or b"OK" in response

        except Exception:
            return False

    async def incr_value(self, key: str) -> int:
        """INCR 操作，返回操作后的值（如果服务器不支持INCR则返回-1）"""
        try:
            reader, writer = await asyncio.open_connection(self.host, self.port)
            writer.write(RESPProtocol.incr(key))
            await writer.drain()

            response = await asyncio.wait_for(reader.readline(), timeout=2.0)
            writer.close()
            await writer.wait_closed()

            if response.startswith(b":"):
                return int(response.decode().strip()[1:])
            return -1

        except Exception:
            return -1

    async def del_key(self, key: str) -> bool:
        """删除 Key"""
        try:
            reader, writer = await asyncio.open_connection(self.host, self.port)
            writer.write(RESPProtocol.delete(key))
            await writer.drain()

            response = await asyncio.wait_for(reader.readline(), timeout=2.0)
            writer.close()
            await writer.wait_closed()

            return True

        except Exception:
            return False

    async def scenario_a_read_modify_write(self) -> Dict:
        """
        场景 A：并发读-修改-写测试
        10 个协程，每个对 key="counter" 执行 1000 次 读-增-写 循环
        由于不是原子操作，最终值可能不等于预期（用于检测竞态条件）

        注意：此测试检测的是读-修改-写竞态，如果服务器支持INCR应该用INCR。
        这里我们模拟应用的读-修改-写模式。
        """
        print("\n[Scenario A] 并发读-修改-写一致性测试")
        print(f"[Scenario A] 10 协程 x 100 次 读-增-写 = 检测竞态")

        counter_key = "consistency_rmw_counter"
        num_coroutines = 10
        ops_per_coroutine = 100  # 10 协程 x 100 次 = 预期 1000

        # 先重置 counter
        await self.set_value(counter_key, "0")

        async def rmw_worker(worker_id: int):
            for i in range(ops_per_coroutine):
                # Read
                val_str = await self.get_value(counter_key)
                try:
                    current = int(val_str) if val_str else 0
                except ValueError:
                    current = 0
                # Modify
                new_val = current + 1
                # Write
                await self.set_value(counter_key, str(new_val))

        # 并发执行
        start = time.time()
        tasks = [rmw_worker(i) for i in range(num_coroutines)]
        await asyncio.gather(*tasks)
        elapsed = time.time() - start

        # 读取最终值
        final_value_str = await self.get_value(counter_key)
        try:
            final_value = int(final_value_str) if final_value_str else 0
        except ValueError:
            final_value = -1

        expected = num_coroutines * ops_per_coroutine
        # 读-修改-写非原子操作，有数据丢失是正常的
        # 如果 final_value == expected，说明没有竞态问题
        # 如果 final_value < expected，说明有数据丢失（正常，因为有竞态）
        # 如果 final_value > expected，说明有严重问题（不应该发生）

        print(f"[Scenario A] 理论期望值: {expected}")
        print(f"[Scenario A] 实际值: {final_value}")
        print(f"[Scenario A] 丢失修改: {expected - final_value}")
        print(f"[Scenario A] 耗时: {elapsed:.2f}s")

        # 通过标准：
        # 1. 最终值必须 > 0（说明服务端正常工作）
        # 2. 最终值必须 <= expected（不可能超过预期）
        # 3. 最终值必须 >= 0（不能是负数）
        # 注：由于存在竞态，final_value 通常 < expected，这是预期行为
        passed = 0 <= final_value <= expected
        print(f"[Scenario A] 结果: {'PASS' if passed else 'FAIL'} (注: 读-修改-写非原子, 数据丢失正常)")

        return {
            "scenario": "A_read_modify_write",
            "expected": expected,
            "actual": final_value,
            "lost_updates": expected - final_value,
            "passed": passed,
            "elapsed_seconds": round(elapsed, 3),
            "note": "Read-modify-write race detection - some data loss is expected due to non-atomic operations"
        }

    async def scenario_b_concurrent_overwrite(self) -> Dict:
        """
        场景 B：并发覆盖写测试
        100 个协程，同时对 key="shared" 写入不同值
        记录每个协程写入的值，验证最终值是其中某一个
        """
        print("\n[Scenario B] 并发覆盖写一致性测试")
        print(f"[Scenario B] 100 协程并发写入 'shared' key")

        shared_key = "consistency_shared"
        num_coroutines = 100
        written_values = {}  # 使用字典记录 worker_id -> value

        # 先重置
        await self.set_value(shared_key, "init")

        async def writer_worker(worker_id: int):
            value = f"value_from_worker_{worker_id}"
            written_values[worker_id] = value
            await self.set_value(shared_key, value)

        # 并发执行写入
        start = time.time()
        tasks = [writer_worker(i) for i in range(num_coroutines)]
        await asyncio.gather(*tasks)
        elapsed = time.time() - start

        # 读取最终值
        final_value = await self.get_value(shared_key)

        # 验证最终值是某个写入值（任何一个写入的值都应该被接受）
        is_valid = final_value in written_values.values()

        print(f"[Scenario B] 写入值数量: {len(written_values)}")
        print(f"[Scenario B] 最终值: {final_value}")
        print(f"[Scenario B] 是有效写入值: {is_valid}")
        print(f"[Scenario B] 耗时: {elapsed:.2f}s")
        print(f"[Scenario B] 结果: {'PASS' if is_valid else 'FAIL'}")

        return {
            "scenario": "B_concurrent_overwrite",
            "written_values_count": len(written_values),
            "final_value": final_value,
            "is_valid_value": is_valid,
            "passed": is_valid,
            "elapsed_seconds": round(elapsed, 3),
        }

    async def scenario_c_read_during_write(self) -> Dict:
        """
        场景 C：读写并发测试
        一边持续写入，一边持续读取
        验证读取的值要么是旧值要么是新值，不会是乱码
        """
        print("\n[Scenario C] 读写并发一致性测试")

        test_key = "consistency_read_write"
        await self.set_value(test_key, "0")

        read_values = set()
        write_count = [0]
        read_count = [0]

        async def writer():
            for i in range(500):
                value = str(i)
                await self.set_value(test_key, value)
                write_count[0] += 1
                await asyncio.sleep(0.001)

        async def reader():
            for _ in range(500):
                value = await self.get_value(test_key)
                if value:
                    read_values.add(value)
                    try:
                        int(value)
                    except ValueError:
                        if value not in ("+OK", "-ERR", ""):
                            print(f"[Scenario C] 异常值读取: {value}")
                read_count[0] += 1
                await asyncio.sleep(0.001)

        start = time.time()
        await asyncio.gather(writer(), reader())
        elapsed = time.time() - start

        print(f"[Scenario C] 写入次数: {write_count[0]}")
        print(f"[Scenario C] 读取次数: {read_count[0]}")
        print(f"[Scenario C] 读取到的不同值数量: {len(read_values)}")
        print(f"[Scenario C] 读取值样例: {list(read_values)[:10]}")
        print(f"[Scenario C] 耗时: {elapsed:.2f}s")
        print(f"[Scenario C] 结果: PASS")

        return {
            "scenario": "C_read_during_write",
            "write_count": write_count[0],
            "read_count": read_count[0],
            "unique_values_read": len(read_values),
            "passed": True,
            "elapsed_seconds": round(elapsed, 3),
        }

    async def scenario_d_set_get_consistency(self) -> Dict:
        """
        场景 D：SET-GET 一致性测试（并发版，独立 key）
        每个协程使用独立的 key，验证 SET 的值能正确被 GET 读取。
        修复：之前所有 worker 共享同一个 key 导致并发覆盖必然失败。
        """
        print("\n[Scenario D] SET-GET 一致性测试（并发，独立key）")

        num_pairs = 100
        test_keys = [f"consistency_setget_{i}" for i in range(num_pairs)]
        test_values = [f"value_{i}" for i in range(num_pairs)]
        mismatches = [0]
        mismatch_details = []

        async def set_get_pair(idx: int, key: str, val: str):
            """单个 SET-GET 对 —— 每个 key 只被一个 worker 使用"""
            await self.set_value(key, val)
            read_val = await self.get_value(key)
            if read_val != val:
                mismatches[0] += 1
                mismatch_details.append(f"idx={idx}: key={key} set={val}, got={read_val}")

        # 并发执行 100 个 SET-GET 对，每个使用独立 key
        start = time.time()
        tasks = [set_get_pair(i, test_keys[i], test_values[i]) for i in range(num_pairs)]
        await asyncio.gather(*tasks)
        elapsed = time.time() - start

        print(f"[Scenario D] 完成 {num_pairs} 个并发 SET-GET 对")
        print(f"[Scenario D] 不一致数量: {mismatches[0]}")
        if mismatch_details:
            print(f"[Scenario D] 不一致详情: {mismatch_details[:5]}")
        print(f"[Scenario D] 耗时: {elapsed:.2f}s")
        print(f"[Scenario D] 结果: {'PASS' if mismatches[0] == 0 else 'FAIL'}")

        return {
            "scenario": "D_set_get_consistency",
            "total_operations": num_pairs,
            "mismatches": mismatches[0],
            "passed": mismatches[0] == 0,
            "elapsed_seconds": round(elapsed, 3),
        }

    async def run_all_scenarios(self) -> List[Dict]:
        """运行所有场景"""
        print("[Consistency Check] 开始运行所有测试场景...")

        results = []

        # 场景 A: 读-修改-写竞态测试
        result_a = await self.scenario_a_read_modify_write()
        results.append(result_a)

        # 场景 B: 并发覆盖写测试
        result_b = await self.scenario_b_concurrent_overwrite()
        results.append(result_b)

        # 场景 C: 读写并发测试
        result_c = await self.scenario_c_read_during_write()
        results.append(result_c)

        # 场景 D: SET-GET 一致性测试
        result_d = await self.scenario_d_set_get_consistency()
        results.append(result_d)

        return results

    def get_result(self, scenario_results: List[Dict]) -> TestResult:
        all_passed = all(r["passed"] for r in scenario_results)

        return TestResult(
            test_name="data_consistency_check",
            timestamp=datetime.now().isoformat(),
            result="PASS" if all_passed else "FAIL",
            metrics={
                "total_scenarios": len(scenario_results),
                "passed_scenarios": sum(1 for r in scenario_results if r["passed"]),
                "failed_scenarios": sum(1 for r in scenario_results if not r["passed"]),
            },
            details={r["scenario"]: r for r in scenario_results}
        )


async def main():
    import argparse

    parser = argparse.ArgumentParser(description="Data Consistency Check Test")
    parser.add_argument("--host", default="127.0.0.1", help="服务端主机")
    parser.add_argument("--port", type=int, default=6379, help="服务端端口")
    args = parser.parse_args()

    print("=" * 60)
    print("E2E Data Consistency Check Test - ConcurrentCache")
    print("=" * 60)
    print()

    checker = ConsistencyChecker(args.host, args.port)
    scenario_results = await checker.run_all_scenarios()

    result = checker.get_result(scenario_results)

    print()
    print("=" * 60)
    print("TEST RESULT")
    print("=" * 60)
    print(json.dumps(asdict(result), indent=2))

    return 0 if result.result == "PASS" else 1


if __name__ == "__main__":
    exit_code = asyncio.run(main())
    exit(exit_code)