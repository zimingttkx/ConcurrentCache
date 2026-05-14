#!/usr/bin/env python3
"""
E2E Test Runner - ConcurrentCache

一次性运行所有 E2E 测试并生成汇总报告。
"""

import subprocess
import json
import sys
import os
from datetime import datetime
from dataclasses import asdict


def run_test(script_path: str, test_name: str) -> dict:
    """运行单个测试脚本"""
    print(f"\n{'='*60}")
    print(f"Running: {test_name}")
    print(f"{'='*60}")

    try:
        result = subprocess.run(
            [sys.executable, script_path, "--host", "127.0.0.1", "--port", "6379"],
            capture_output=True,
            text=True,
            timeout=300  # 5 分钟超时
        )

        output = result.stdout + result.stderr

        # 尝试解析 JSON 输出 - 查找最后一个以 "{" 开头到 "}" 结尾的 JSON 对象
        try:
            # 在输出中搜索 JSON 对象的起始和结束位置
            json_start = output.rfind('{')
            json_end = output.rfind('}')

            if json_start != -1 and json_end != -1 and json_start < json_end:
                json_str = output[json_start:json_end + 1]
                result_data = json.loads(json_str)

                return {
                    "name": test_name,
                    "passed": result_data.get("result") == "PASS",
                    "exit_code": result.returncode,
                    "metrics": result_data.get("metrics", {}),
                }
            else:
                print(f"Output:\n{output[-500:]}")  # 打印最后 500 字符
                return {
                    "name": test_name,
                    "passed": result.returncode == 0,
                    "exit_code": result.returncode,
                    "error": "No JSON found in output",
                }
        except json.JSONDecodeError as e:
            print(f"JSON parse error: {e}")
            print(f"Output:\n{output[-500:]}")  # 打印最后 500 字符
            return {
                "name": test_name,
                "passed": result.returncode == 0,
                "exit_code": result.returncode,
                "error": f"Failed to parse JSON output: {e}",
            }

    except subprocess.TimeoutExpired:
        return {
            "name": test_name,
            "passed": False,
            "exit_code": -1,
            "error": "Test timeout (5 min)",
        }
    except Exception as e:
        return {
            "name": test_name,
            "passed": False,
            "exit_code": -1,
            "error": str(e),
        }


def main():
    print("=" * 60)
    print("E2E Test Suite - ConcurrentCache")
    print("=" * 60)
    print(f"Start time: {datetime.now().isoformat()}")
    print()

    # 获取脚本目录
    script_dir = os.path.dirname(os.path.abspath(__file__))

    tests = [
        ("e2e_connection_storm.py", "Connection Storm Test"),
        ("e2e_high_concurrency_load.py", "High Concurrency Load Test"),
        ("e2e_consistency_check.py", "Data Consistency Check Test"),
        ("e2e_chaos_test.py", "Chaos Test"),
    ]

    results = []
    for script_name, test_name in tests:
        script_path = os.path.join(script_dir, script_name)
        result = run_test(script_path, test_name)
        results.append(result)

    # 汇总报告
    print()
    print("=" * 60)
    print("E2E TEST SUITE SUMMARY")
    print("=" * 60)

    total = len(results)
    passed = sum(1 for r in results if r["passed"])
    failed = total - passed

    print(f"Total tests: {total}")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    print()

    for r in results:
        status = "PASS" if r["passed"] else "FAIL"
        print(f"  [{status}] {r['name']}")

    print()
    print(f"End time: {datetime.now().isoformat()}")

    # 生成 JSON 报告
    report = {
        "timestamp": datetime.now().isoformat(),
        "total_tests": total,
        "passed_tests": passed,
        "failed_tests": failed,
        "results": results,
    }

    report_path = os.path.join(script_dir, "e2e_report.json")
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)

    print(f"\nJSON report saved to: {report_path}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    exit(main())