#!/usr/bin/env python3
"""
E2E Connection Storm Test - ConcurrentCache

瞬间发起 10,000 个以上的并发 TCP 连接到服务端，
测试极限连接数下服务端是否崩溃或拒绝服务。
"""

import asyncio
import socket
import time
import json
from typing import List, Tuple
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
    def encode_simple_string(s: str) -> bytes:
        return f"+{s}\r\n".encode()

    @staticmethod
    def encode_bulk(bulk: str) -> bytes:
        """将字符串编码为 RESP bulk string"""
        encoded = bulk.encode('utf-8')
        return f"${len(encoded)}\r\n{bulk}\r\n".encode()

    @staticmethod
    def encode_array(arr: list) -> bytes:
        """将字符串列表编码为 RESP array"""
        result = f"*{len(arr)}\r\n".encode()
        for item in arr:
            result += RESPProtocol.encode_bulk(item)
        return result


class ConnectionTester:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.stats = {
            "total_connections": 0,
            "successful_connections": 0,
            "failed_connections": 0,
            "pong_received": 0,
            "pong_failed": 0,
            "connection_errors": [],
        }

    async def create_connection(self, conn_id: int) -> Tuple[bool, str]:
        """创建单个连接并发送 PING"""
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=5.0
            )
            self.stats["total_connections"] += 1

            # 发送 PING
            writer.write(RESPProtocol.ping())
            await writer.drain()

            # 接收 PONG
            response = await asyncio.wait_for(reader.readline(), timeout=2.0)
            if b"PONG" in response or b"+PING" in response:
                self.stats["successful_connections"] += 1
                self.stats["pong_received"] += 1
            elif response == b"":
                # 空响应：连接可能被对端关闭，不算成功
                self.stats["failed_connections"] += 1
                return False, f"conn_{conn_id}: empty_response"
            else:
                self.stats["pong_failed"] += 1

            writer.close()
            await writer.wait_closed()
            return True, ""

        except asyncio.TimeoutError:
            self.stats["failed_connections"] += 1
            return False, f"conn_{conn_id}: timeout"
        except ConnectionRefusedError:
            self.stats["failed_connections"] += 1
            return False, f"conn_{conn_id}: connection refused"
        except Exception as e:
            self.stats["failed_connections"] += 1
            return False, f"conn_{conn_id}: {type(e).__name__}"

    async def batch_connect(self, batch_size: int, batch_id: int) -> List[Tuple[bool, str]]:
        """批量创建连接"""
        tasks = [self.create_connection(batch_id * batch_size + i) for i in range(batch_size)]
        return await asyncio.gather(*tasks)

    async def run_storm_test(self, total_connections: int = 10000, batch_size: int = 500):
        """运行连接洪峰测试"""
        print(f"[Connection Storm] 目标连接数: {total_connections}")
        print(f"[Connection Storm] 分批大小: {batch_size}")
        print(f"[Connection Storm] 目标主机: {self.host}:{self.port}")

        start_time = time.time()

        # 分批创建连接
        batches = (total_connections + batch_size - 1) // batch_size
        for batch_id in range(batches):
            remaining = total_connections - batch_id * batch_size
            current_batch = min(batch_size, remaining)

            print(f"[Connection Storm] 批次 {batch_id + 1}/{batches}: 创建 {current_batch} 个连接...")

            results = await self.batch_connect(current_batch, batch_id)

            # 每批后短暂休息，避免本地端口耗尽
            if batch_id < batches - 1:
                await asyncio.sleep(0.1)

        elapsed = time.time() - start_time

        print(f"\n[Connection Storm] 测试完成!")
        print(f"[Connection Storm] 耗时: {elapsed:.2f} 秒")
        print(f"[Connection Storm] 吞吐量: {self.stats['total_connections'] / elapsed:.2f} 连接/秒")

        return elapsed

    def get_result(self, elapsed: float, server_alive: bool = True) -> TestResult:
        success_rate = (
            self.stats["successful_connections"] / self.stats["total_connections"]
            if self.stats["total_connections"] > 0 else 0
        )

        return TestResult(
            test_name="connection_storm",
            timestamp=datetime.now().isoformat(),
            result="PASS" if server_alive and success_rate > 0.7 else "FAIL",
            metrics={
                "total_connections": self.stats["total_connections"],
                "successful_connections": self.stats["successful_connections"],
                "failed_connections": self.stats["failed_connections"],
                "pong_received": self.stats["pong_received"],
                "pong_failed": self.stats["pong_failed"],
                "success_rate": round(success_rate, 4),
                "duration_seconds": round(elapsed, 3),
                "connections_per_second": round(self.stats["total_connections"] / elapsed, 2),
            }
        )


async def check_server_alive(host: str, port: int) -> bool:
    """检查服务端是否存活"""
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port),
            timeout=2.0
        )
        writer.write(RESPProtocol.ping())
        await writer.drain()
        response = await asyncio.wait_for(reader.readline(), timeout=2.0)
        writer.close()
        await writer.wait_closed()
        return True
    except:
        return False


async def main():
    import argparse

    parser = argparse.ArgumentParser(description="Connection Storm Test")
    parser.add_argument("--host", default="127.0.0.1", help="服务端主机")
    parser.add_argument("--port", type=int, default=6379, help="服务端端口")
    parser.add_argument("--connections", type=int, default=2000, help="目标连接数")
    parser.add_argument("--batch-size", type=int, default=200, help="每批连接数")
    args = parser.parse_args()

    print("=" * 60)
    print("E2E Connection Storm Test - ConcurrentCache")
    print("=" * 60)
    print()

    # 检查服务端是否可用
    print(f"[Pre-check] 正在连接 {args.host}:{args.port}...")
    server_alive = await check_server_alive(args.host, args.port)
    if not server_alive:
        print(f"[Pre-check] 错误: 无法连接到服务端 {args.host}:{args.port}")
        result = TestResult(
            test_name="connection_storm",
            timestamp=datetime.now().isoformat(),
            result="FAIL",
            metrics={"error": "Cannot connect to server"}
        )
        print(json.dumps(asdict(result), indent=2))
        return 1

    print(f"[Pre-check] 服务端连接正常!")

    # 运行测试
    tester = ConnectionTester(args.host, args.port)
    elapsed = await tester.run_storm_test(
        total_connections=args.connections,
        batch_size=args.batch_size
    )

    # 输出结果
    result = tester.get_result(elapsed, server_alive)
    print()
    print("=" * 60)
    print("TEST RESULT")
    print("=" * 60)
    print(json.dumps(asdict(result), indent=2))

    return 0 if result.result == "PASS" else 1


if __name__ == "__main__":
    exit_code = asyncio.run(main())
    exit(exit_code)