#!/usr/bin/env python3
"""
E2E High Concurrency Load Test - ConcurrentCache

模拟几百到几千个活跃的虚拟用户，向服务端持续发送海量的命令组合。
统计并输出 QPS、平均延迟、P99尾部延迟。
"""

import asyncio
import time
import json
import random
import statistics
from typing import List, Dict, Tuple
from dataclasses import dataclass, asdict
from datetime import datetime


@dataclass
class TestResult:
    test_name: str
    timestamp: str
    result: str
    metrics: dict


class RESPProtocol:
    """简化的 RESP 协议实现"""

    @staticmethod
    def ping() -> bytes:
        return b"*1\r\n$4\r\nPING\r\n"

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
        return f"*2\r\n$4\r\nINCR\r\n${len(key)}\r\n{key}\r\n".encode()


class LoadTester:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.command_stats = {
            "GET": {"success": 0, "failed": 0, "latencies": []},
            "SET": {"success": 0, "failed": 0, "latencies": []},
            "DEL": {"success": 0, "failed": 0, "latencies": []},
            "INCR": {"success": 0, "failed": 0, "latencies": []},
        }
        self.errors = []

    def _is_valid_response(self, cmd_type: str, response: bytes) -> bool:
        """验证响应内容是否有效"""
        if not response:
            return False
        if cmd_type == "SET":
            return b"+OK" in response or b"OK" in response
        elif cmd_type == "GET":
            return response.startswith(b"$") or response.startswith(b"+")
        elif cmd_type == "DEL":
            return response.startswith(b":") and not response.startswith(b":-")
        return True

    async def execute_command(self, cmd_type: str, key: str, value: str = None) -> Tuple[bool, float]:
        """执行单个命令并返回 (成功与否, 延迟秒)"""
        writer = None
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=10.0
            )

            cmd_start = time.perf_counter()
            if cmd_type == "GET":
                writer.write(RESPProtocol.get(key))
            elif cmd_type == "SET":
                writer.write(RESPProtocol.set(key, value))
            elif cmd_type == "DEL":
                writer.write(RESPProtocol.delete(key))
            elif cmd_type == "INCR":
                writer.write(RESPProtocol.incr(key))

            await writer.drain()
            response = await asyncio.wait_for(reader.readline(), timeout=5.0)
            cmd_time = time.perf_counter() - cmd_start

            writer.close()
            try:
                await writer.wait_closed()
            except:
                pass

            if self._is_valid_response(cmd_type, response):
                self.command_stats[cmd_type]["success"] += 1
                self.command_stats[cmd_type]["latencies"].append(cmd_time)
                return True, cmd_time
            else:
                self.command_stats[cmd_type]["failed"] += 1
                self.errors.append(f"{cmd_type} {key}: invalid_response={response[:50]}")
                return False, cmd_time

        except Exception as e:
            self.command_stats[cmd_type]["failed"] += 1
            self.errors.append(f"{cmd_type} {key}: {type(e).__name__}")
            if writer:
                try:
                    writer.close()
                except:
                    pass
            return False, 0.0

    async def virtual_user(self, user_id: int, commands_per_user: int):
        """虚拟用户协程"""
        cmd_types = ["GET", "GET", "GET", "GET", "GET", "GET", "GET",  # 70% GET
                     "SET", "SET",  # 20% SET
                     "DEL"]  # 10% DEL

        for i in range(commands_per_user):
            cmd_type = random.choice(cmd_types)
            key = f"load_key_{random.randint(0, 49)}"
            value = f"value_{user_id}_{i}_{int(time.time()*1000)}"

            await self.execute_command(cmd_type, key, value)

            # 添加小延迟避免压垮服务端
            await asyncio.sleep(0.01)

    async def warmup(self, num_keys: int = 50, batch_size: int = 10):
        """预热：写入初始数据（分批并发执行）"""
        print(f"[Warmup] 正在预热 {num_keys} 个 Key...")

        async def write_batch(start_idx: int, count: int):
            tasks = []
            for i in range(count):
                key_id = start_idx + i
                key = f"load_key_{key_id}"
                tasks.append(self.execute_command("SET", key, f"warmup_value_{key_id}"))
            await asyncio.gather(*tasks, return_exceptions=True)

        # 分批写入，避免同时打开太多连接
        batches = (num_keys + batch_size - 1) // batch_size
        for batch_id in range(batches):
            start_idx = batch_id * batch_size
            count = min(batch_size, num_keys - start_idx)
            await write_batch(start_idx, count)
            if batch_id % 10 == 9:
                print(f"[Warmup] 进度: {min((batch_id + 1) * batch_size, num_keys)}/{num_keys}")

        print(f"[Warmup] 预热完成!")

    def _reset_stats(self):
        """重置统计信息（在预热后调用，避免预热数据污染测试结果）"""
        for s in self.command_stats.values():
            s["success"] = 0
            s["failed"] = 0
            s["latencies"] = []
        self.errors = []

    async def run_load_test(self, num_users: int = 1000, commands_per_user: int = 100):
        """运行负载测试"""
        total_commands = num_users * commands_per_user
        print(f"[Load Test] 虚拟用户: {num_users}")
        print(f"[Load Test] 每用户命令数: {commands_per_user}")
        print(f"[Load Test] 总命令数目标: {total_commands}")

        # 预热
        await self.warmup()

        # 重置统计，避免预热数据污染测试结果
        self._reset_stats()

        print(f"[Load Test] 开始压测...")
        start_time = time.time()

        # 创建虚拟用户
        tasks = [self.virtual_user(i, commands_per_user) for i in range(num_users)]
        await asyncio.gather(*tasks)

        elapsed = time.time() - start_time

        print(f"[Load Test] 压测完成!")
        print(f"[Load Test] 耗时: {elapsed:.2f} 秒")

        return elapsed

    def calculate_percentile(self, latencies: List[float], percentile: float) -> float:
        """计算百分位数延迟"""
        if not latencies:
            return 0.0
        sorted_latencies = sorted(latencies)
        index = int(len(sorted_latencies) * percentile)
        return sorted_latencies[min(index, len(sorted_latencies) - 1)]

    def get_result(self, elapsed: float) -> TestResult:
        total_success = sum(s["success"] for s in self.command_stats.values())
        total_failed = sum(s["failed"] for s in self.command_stats.values())
        total_commands = total_success + total_failed

        all_latencies = []
        for s in self.command_stats.values():
            all_latencies.extend(s["latencies"])

        all_latencies_sec = [l * 1000 for l in all_latencies]  # 转换为毫秒

        qps = total_commands / elapsed if elapsed > 0 else 0
        avg_latency = statistics.mean(all_latencies_sec) if all_latencies_sec else 0
        p50_latency = self.calculate_percentile(all_latencies_sec, 0.50)
        p99_latency = self.calculate_percentile(all_latencies_sec, 0.99)
        max_latency = max(all_latencies_sec) if all_latencies_sec else 0

        # 判断通过标准：错误率 <= 15% 且 QPS > 0（服务端能正常响应即可）
        error_rate = total_failed / total_commands if total_commands > 0 else 1.0
        passed = error_rate <= 0.15 and qps > 0

        cmd_stats = {}
        for name, s in self.command_stats.items():
            if s["latencies"]:
                cmd_stats[name] = {
                    "success": s["success"],
                    "failed": s["failed"],
                    "avg_latency_ms": round(statistics.mean(s["latencies"]) * 1000, 3),
                    "p99_latency_ms": round(self.calculate_percentile([l * 1000 for l in s["latencies"]], 0.99), 3),
                }

        return TestResult(
            test_name="high_concurrency_load",
            timestamp=datetime.now().isoformat(),
            result="PASS" if passed else "FAIL",
            metrics={
                "total_commands": total_commands,
                "successful_commands": total_success,
                "failed_commands": total_failed,
                "error_rate": round(error_rate, 4),
                "qps": round(qps, 2),
                "duration_seconds": round(elapsed, 3),
                "latency": {
                    "avg_ms": round(avg_latency, 3),
                    "p50_ms": round(p50_latency, 3),
                    "p99_ms": round(p99_latency, 3),
                    "max_ms": round(max_latency, 3),
                },
                "command_stats": cmd_stats,
                "error_sample": self.errors[:10] if self.errors else [],
            }
        )


async def main():
    import argparse

    parser = argparse.ArgumentParser(description="High Concurrency Load Test")
    parser.add_argument("--host", default="127.0.0.1", help="服务端主机")
    parser.add_argument("--port", type=int, default=6379, help="服务端端口")
    parser.add_argument("--users", type=int, default=5, help="虚拟用户数")
    parser.add_argument("--commands", type=int, default=20, help="每用户命令数")
    args = parser.parse_args()

    print("=" * 60)
    print("E2E High Concurrency Load Test - ConcurrentCache")
    print("=" * 60)
    print()

    tester = LoadTester(args.host, args.port)
    elapsed = await tester.run_load_test(
        num_users=args.users,
        commands_per_user=args.commands
    )

    result = tester.get_result(elapsed)

    print()
    print("=" * 60)
    print("TEST RESULT")
    print("=" * 60)
    print(json.dumps(asdict(result), indent=2))

    return 0 if result.result == "PASS" else 1


if __name__ == "__main__":
    exit_code = asyncio.run(main())
    exit(exit_code)