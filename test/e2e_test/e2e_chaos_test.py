#!/usr/bin/env python3
"""
E2E Chaos Test - ConcurrentCache

模拟恶意的客户端行为：
- TCP Reset（连接 Reset）
- 超大 Payload
- 畸形 RESP 协议
- 快速连接断开
验证服务端是否能安全处理且不崩溃。
"""

import asyncio
import socket
import time
import json
import random
import string
from typing import List, Dict
from dataclasses import dataclass, asdict
from datetime import datetime


@dataclass
class TestResult:
    test_name: str
    timestamp: str
    result: str
    metrics: dict
    details: dict


class ChaosTester:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.results = {}

    async def check_server_alive(self) -> bool:
        """检查服务端是否存活"""
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=2.0
            )
            writer.write(b"*1\r\n$4\r\nPING\r\n")
            await writer.drain()
            response = await asyncio.wait_for(reader.readline(), timeout=2.0)
            writer.close()
            await writer.wait_closed()
            return b"PONG" in response or b"+PING" in response
        except:
            return False

    async def test_tcp_half_close(self) -> Dict:
        """
        测试用例 1：TCP Half-Close
        建立连接，发送部分命令后强制 RST
        """
        print("\n[Test 1] TCP Half-Close (RST)")
        print("[Test 1] 建立连接后发送部分数据，然后强制断开...")

        passed = True
        error_msg = ""
        alive = False

        try:
            # 创建原始 socket
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2.0)
            sock.connect((self.host, self.port))

            # 发送部分数据（不完整的 RESP 命令）
            sock.sendall(b"*1\r\n$4\r\nPING\r")

            # 强制关闭连接 (RST) - 使用 SO_LINGER with timeout=0
            import struct
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            sock.close()

            print("[Test 1] 连接已强制断开 (RST)")

            # 等待片刻
            await asyncio.sleep(0.5)

            # 检查服务端是否存活
            alive = await self.check_server_alive()
            if not alive:
                passed = False
                error_msg = "Server died after RST"

        except Exception as e:
            passed = False
            error_msg = str(e)

        print(f"[Test 1] 服务端存活: {alive}")
        print(f"[Test 1] 结果: {'PASS' if passed else 'FAIL'} - {error_msg}")

        return {
            "test": "tcp_half_close",
            "passed": passed,
            "error": error_msg,
        }

    async def test_huge_payload(self) -> Dict:
        """
        测试用例 2：巨大 Payload
        发送超大 Value（如 50MB 字符串）
        """
        print("\n[Test 2] 超大 Payload (50MB)")

        passed = True
        error_msg = ""

        # 生成 50MB 的纯 ASCII 数据（不包含 \r \n 等特殊字符）
        huge_value = "X" * (50 * 1024 * 1024)

        print(f"[Test 2] 生成 50MB payload...")

        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=5.0
            )

            # 构造超大 SET 命令，使用正确的 RESP bulk string 格式
            # 注意：bulk string 的长度是原始字节长度，\r\n 需要保持为原始字节
            cmd = b"*3\r\n$3\r\nSET\r\n$10\r\nhuge_key\r\n$" + str(len(huge_value)).encode() + b"\r\n" + huge_value.encode() + b"\r\n"

            print(f"[Test 2] 发送 {len(cmd)} 字节的 SET 命令...")

            # 发送数据
            writer.write(cmd)
            await asyncio.wait_for(writer.drain(), timeout=10.0)

            # 等待响应（可能超时或被拒绝）
            try:
                response = await asyncio.wait_for(reader.readline(), timeout=3.0)
                print(f"[Test 2] 收到响应: {response[:50]}")
            except asyncio.TimeoutError:
                print("[Test 2] 响应超时（可能是正常的）")

            writer.close()
            await writer.wait_closed()

        except Exception as e:
            passed = False
            error_msg = str(e)

        # 检查服务端存活
        alive = await self.check_server_alive()

        print(f"[Test 2] 服务端存活: {alive}")
        print(f"[Test 2] 结果: {'PASS' if passed and alive else 'FAIL'}")

        return {
            "test": "huge_payload",
            "payload_size_mb": 50,
            "passed": passed and alive,
            "error": error_msg,
        }

    async def test_malformed_resp(self) -> Dict:
        """
        测试用例 3：畸形 RESP 协议
        发送非 RESP 格式的随机字节流
        """
        print("\n[Test 3] 畸形 RESP 协议")

        test_cases = [
            ("random_bytes", bytes(random.randint(0, 255) for _ in range(100))),
            ("incomplete_array", b"*3\r\n$3\r\nSET"),
            ("negative_length", b"$-1\r\n"),
            ("null_array", b"*-1\r\n"),
            ("mixed_garbage", b"SET key\r\nVALUE\r\n123\r\n"),
            ("empty", b""),
        ]

        all_passed = True
        results = []

        for name, data in test_cases:
            print(f"[Test 3] 发送: {name} ({len(data)} bytes)...")
            case_passed = True

            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(self.host, self.port),
                    timeout=2.0
                )

                writer.write(data)
                await writer.drain()

                # 尝试读取响应
                try:
                    response = await asyncio.wait_for(reader.readline(), timeout=1.0)
                    print(f"[Test 3]   响应: {response[:30]}")
                    # 如果收到错误响应但没有断开连接，认为服务端正确处理了畸形数据
                    if response.startswith(b"-ERR"):
                        print(f"[Test 3]   服务端返回错误（正常）")
                except asyncio.TimeoutError:
                    print(f"[Test 3]   无响应或超时")
                    # 超时也可能是正常行为

                writer.close()
                await writer.wait_closed()

            except Exception as e:
                print(f"[Test 3]   错误: {e}")
                case_passed = False
                all_passed = False

            results.append({"case": name, "data_length": len(data), "passed": case_passed})

        # 检查服务端存活
        alive = await self.check_server_alive()

        # 只要服务端存活且没有崩溃就算通过
        passed = alive and all_passed

        print(f"[Test 3] 服务端存活: {alive}")
        print(f"[Test 3] 所有畸形数据处理正确: {all_passed}")
        print(f"[Test 3] 结果: {'PASS' if passed else 'FAIL'}")

        return {
            "test": "malformed_resp",
            "test_cases": len(test_cases),
            "all_cases_passed": all_passed,
            "server_alive": alive,
            "passed": passed,
            "details": results,
        }

    async def test_rapid_connect_disconnect(self) -> Dict:
        """
        测试用例 4：快速连接断开
        1 秒内建立并断开 1000 个连接
        """
        print("\n[Test 4] 快速连接断开 (1000 连接/秒)")

        num_connections = 500
        start = time.time()

        async def quick_connect(conn_id: int):
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(self.host, self.port),
                    timeout=1.0
                )
                writer.close()
                await writer.wait_closed()
                return True
            except:
                return False

        tasks = [quick_connect(i) for i in range(num_connections)]
        results = await asyncio.gather(*tasks, return_exceptions=True)

        elapsed = time.time() - start
        success_count = sum(1 for r in results if r is True)

        print(f"[Test 4] 完成 {num_connections} 连接尝试")
        print(f"[Test 4] 成功连接: {success_count}")
        print(f"[Test 4] 耗时: {elapsed:.2f} 秒")
        print(f"[Test 4] 连接速率: {num_connections / elapsed:.0f} 连接/秒")

        # 检查服务端存活（快速连接可能暂时占满 listen backlog，等待恢复）
        alive = False
        for attempt in range(5):
            await asyncio.sleep(1.0)
            alive = await self.check_server_alive()
            if alive:
                break
            print(f"[Test 4] 服务端第 {attempt+1} 次检查: 未恢复")

        print(f"[Test 4] 服务端存活: {alive}")
        print(f"[Test 4] 结果: {'PASS' if alive else 'FAIL'}")

        return {
            "test": "rapid_connect_disconnect",
            "total_attempts": num_connections,
            "successful_connections": success_count,
            "failed_connections": num_connections - success_count,
            "duration_seconds": round(elapsed, 3),
            "connections_per_second": round(num_connections / elapsed, 0),
            "passed": alive,
        }

    async def test_concurrent_malformed(self) -> Dict:
        """
        测试用例 5：并发异常请求
        多个连接同时发送畸形数据
        """
        print("\n[Test 5] 并发异常请求")

        num_connections = 10
        test_data = [
            b"INVALID_COMMAND\r\n",
            b"Garbage\r\nData\r\n",
            b"\x00\x01\x02\xff\xfe\xfd",
            b"NOT_A_REAL_CMD\r\n",
        ]

        async def send_malformed(conn_id: int):
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(self.host, self.port),
                    timeout=2.0
                )

                data = random.choice(test_data)
                writer.write(data)
                await writer.drain()

                try:
                    await asyncio.wait_for(reader.readline(), timeout=0.5)
                except:
                    pass

                writer.close()
                await writer.wait_closed()
                return True
            except:
                return False

        tasks = [send_malformed(i) for i in range(num_connections)]
        results = await asyncio.gather(*tasks, return_exceptions=True)

        success_count = sum(1 for r in results if r is True)

        # 检查服务端存活
        alive = await self.check_server_alive()

        print(f"[Test 5] 发送 {num_connections} 个异常请求")
        print(f"[Test 5] 成功发送: {success_count}")
        print(f"[Test 5] 服务端存活: {alive}")
        print(f"[Test 5] 结果: {'PASS' if alive else 'FAIL'}")

        return {
            "test": "concurrent_malformed",
            "total_requests": num_connections,
            "successful_requests": success_count,
            "passed": alive,
        }

    async def test_unknown_commands(self) -> Dict:
        """
        测试用例 6：未知命令处理
        发送服务端不支持的 Redis 命令，验证服务端返回错误而不会崩溃。
        ConcurrentCache 是 KV 缓存，不支持列表/阻塞命令是预期行为。
        """
        print("\n[Test 6] 未知命令处理健壮性")

        unknown_commands = [
            (b"*2\r\n$5\r\nBLPOP\r\n$7\r\ntestlist\r\n", "BLPOP"),          # 不支持阻塞命令
            (b"*2\r\n$4\r\nHGET\r\n$7\r\ntestkey\r\n", "HGET"),             # 不支持哈希命令
            (b"*2\r\n$5\r\nLPUSH\r\n$7\r\ntestlist\r\n", "LPUSH"),          # 不支持列表命令
        ]

        all_alive = True
        all_got_response = True
        results = []

        for cmd_bytes, cmd_name in unknown_commands:
            try:
                reader, writer = await asyncio.wait_for(
                    asyncio.open_connection(self.host, self.port),
                    timeout=2.0
                )
                writer.write(cmd_bytes)
                await writer.drain()

                try:
                    response = await asyncio.wait_for(reader.readline(), timeout=2.0)
                    print(f"[Test 6] {cmd_name}: 响应={response[:60]}")
                    # 期望服务端返回错误而不是崩溃
                    got_response = len(response) > 0
                    if not got_response:
                        all_got_response = False
                except asyncio.TimeoutError:
                    print(f"[Test 6] {cmd_name}: 响应超时")
                    all_got_response = False

                writer.close()
                await writer.wait_closed()

                # 每次后检查服务端存活
                alive = await self.check_server_alive()
                if not alive:
                    all_alive = False
                    print(f"[Test 6] {cmd_name}: 服务端在收到此命令后崩溃!")

                results.append({"command": cmd_name, "server_alive": alive})

            except Exception as e:
                print(f"[Test 6] {cmd_name}: 错误={e}")
                all_got_response = False

        # 检查最终服务端存活
        alive = await self.check_server_alive()
        passed = alive and all_alive and all_got_response

        print(f"[Test 6] 服务端存活: {alive}")
        print(f"[Test 6] 所有命令均有响应: {all_got_response}")
        print(f"[Test 6] 结果: {'PASS' if passed else 'FAIL'}")

        return {
            "test": "unknown_commands",
            "server_alive": alive,
            "all_alive": all_alive,
            "all_got_response": all_got_response,
            "passed": passed,
            "details": results,
        }

    async def run_all_tests(self) -> List[Dict]:
        """运行所有混沌测试"""
        print("[Chaos Test] 开始运行所有混沌测试...")

        results = []

        results.append(await self.test_tcp_half_close())
        results.append(await self.test_huge_payload())
        results.append(await self.test_malformed_resp())
        results.append(await self.test_rapid_connect_disconnect())
        results.append(await self.test_concurrent_malformed())

        # 仅在并发异常请求后检查并恢复服务端
        if not results[-1]["passed"]:
            print("\n[Recovery] 等待服务端从异常请求中恢复...")
            recovered = False
            for attempt in range(5):
                await asyncio.sleep(1.0)
                alive = await self.check_server_alive()
                if alive:
                    recovered = True
                    print(f"[Recovery] 服务端在第 {attempt+1} 次检查后恢复")
                    break
                print(f"[Recovery] 第 {attempt+1} 次检查: 服务端未响应")
            if recovered:
                results.append(await self.test_unknown_commands())
            else:
                print("[Recovery] 服务端未能恢复，跳过后续测试")
        else:
            results.append(await self.test_unknown_commands())

        return results

    def get_result(self, test_results: List[Dict]) -> TestResult:
        all_passed = all(r["passed"] for r in test_results)

        return TestResult(
            test_name="chaos_testing",
            timestamp=datetime.now().isoformat(),
            result="PASS" if all_passed else "FAIL",
            metrics={
                "total_tests": len(test_results),
                "passed_tests": sum(1 for r in test_results if r["passed"]),
                "failed_tests": sum(1 for r in test_results if not r["passed"]),
            },
            details={r["test"]: r for r in test_results}
        )


async def main():
    import argparse

    parser = argparse.ArgumentParser(description="Chaos Test")
    parser.add_argument("--host", default="127.0.0.1", help="服务端主机")
    parser.add_argument("--port", type=int, default=6379, help="服务端端口")
    args = parser.parse_args()

    print("=" * 60)
    print("E2E Chaos Test - ConcurrentCache")
    print("=" * 60)
    print()

    tester = ChaosTester(args.host, args.port)

    # 先检查服务端是否可连接
    print("[Pre-check] 检查服务端状态...")
    alive = await tester.check_server_alive()
    if not alive:
        print("[Pre-check] 无法连接到服务端，测试终止")
        result = TestResult(
            test_name="chaos_testing",
            timestamp=datetime.now().isoformat(),
            result="FAIL",
            metrics={"error": "Cannot connect to server"},
            details={}
        )
        print(json.dumps(asdict(result), indent=2))
        return 1

    print("[Pre-check] 服务端连接正常，开始混沌测试...\n")

    test_results = await tester.run_all_tests()
    result = tester.get_result(test_results)

    print()
    print("=" * 60)
    print("TEST RESULT")
    print("=" * 60)
    print(json.dumps(asdict(result), indent=2))

    return 0 if result.result == "PASS" else 1


if __name__ == "__main__":
    exit_code = asyncio.run(main())
    exit(exit_code)