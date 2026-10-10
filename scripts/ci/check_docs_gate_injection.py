#!/usr/bin/env python3
"""证明 §7 文档判据真的会拦：往文档里注入一个错，跑 check_consistency.py，
要求"只有对应那一条报错"，然后还原。

为什么要有这个文件：§7 那组判据比对的是名字集合、计数、端口、版本号和链接能不能
解析——它们全都可能因为正则写坏而变成"永远绿"。只看一次"跑过了"证明不了任何事，
所以这里逐条注入、逐条比对 ::error:: 文本，红错地方也算失败。

只在本地/CI 里对着工作树跑：它会改 docs/*.md 再改回来，所以不要在有并发写入时跑。
"""

import io
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)

FILES = ['docs/api.md', 'README.md', 'docs/deployment.md', 'docs/testing.md']

CASES = [
    ('7a 索引少一个命令', ['docs/api.md'],
     'INCRBY / DECRBY |', 'DECRBY |'),
    ('7a 总数写错', ['docs/api.md'],
     '**命令总数**：46 个', '**命令总数**：45 个'),
    ('7b ctest 名单写错', ['README.md'],
     '| ContractTests |', '| ContractTestXyz |'),
    ('7b ctest 条数写错', ['README.md'],
     '共 16 条', '共 15 条'),
    ('7c 相对链接断掉', ['docs/api.md'],
     '[部署文档 § 端口与连接](deployment.md)', '[部署文档 § 端口与连接](../deployment.md)'),
    ('7d 把总线端口当客户端口', ['docs/deployment.md'],
     'redis-cli -p 6379 INFO server', 'redis-cli -p 16379 INFO server'),
    ('7d 默认端口写错', ['docs/api.md'],
     '> **默认端口**：`6379`', '> **默认端口**：`16380`'),
    ('7e 版本号脱钩', ['docs/api.md'],
     'concurrentcache_version:4.0.0', 'concurrentcache_version:3.0.0'),
    ('8 声明未定义的方法', ['src/base/log.h'],
     'class Logger : public ConfigObserver {', 'class Logger : public ConfigObserver {\npublic:\n    void bogus_declared_only_marker();'),
    ('7f 指向不存在的镜像仓库', ['docs/deployment.md'],
     'docker build -t concurrentcache:latest .',
     'docker pull ghcr.io/someone/concurrentcache:latest'),
]

EXPECT = {
    '7a 索引少一个命令': '命令索引与注册表不一致',
    '7a 总数写错': '声明 45 个命令',
    '7b ctest 名单写错': 'ctest 名单与注册表不一致',
    '7b ctest 条数写错': '声称 ctest 用例共 15 条',
    '7c 相对链接断掉': '相对链接断了',
    '7d 把总线端口当客户端口': '用总线端口 16379 当客户端口',
    '7d 默认端口写错': '写的默认端口 16380',
    '7e 版本号脱钩': 'docs/api.md 示例写的是 3.0.0',
    '7f 指向不存在的镜像仓库': '没有',
    '8 声明未定义的方法': 'bogus_declared_only_marker',
}


def load(paths):
    return {p: io.open(p, encoding='utf-8', newline='').read() for p in paths}


def restore(snapshot):
    for p, text in snapshot.items():
        io.open(p, 'w', encoding='utf-8', newline='').write(text)


def run():
    r = subprocess.run([sys.executable, 'scripts/ci/check_consistency.py', '--docs-only'],
                       capture_output=True, text=True)
    errors = re.findall(r'::error::(.*)', r.stdout + r.stderr)
    return r.returncode, errors


print('=== 基线：不注入时必须绿（退 0、无 ::error::）===')
code, errors = run()
print('exit=%d errors=%d' % (code, len(errors)))
if code != 0 or errors:
    print('基线就不绿，后面的注入验证没有意义：', errors)
    sys.exit(1)

failed = 0
for name, paths, old, new in CASES:
    snapshot = load(paths)
    text = io.open(paths[0], encoding='utf-8', newline='').read()
    crlf = text.count('\r\n') > 0
    body = text.replace('\r\n', '\n')
    if body.count(old) < 1:
        print('!! %s：锚点没找到 %r' % (name, old[:40]))
        failed += 1
        continue
    body = body.replace(old, new, 1)
    io.open(paths[0], 'w', encoding='utf-8', newline='').write(
        body.replace('\n', '\r\n') if crlf else body)
    code, errors = run()
    restore(snapshot)
    hit = [e for e in errors if EXPECT[name] in e]
    only_expected = bool(hit) and len(errors) == 1
    status = 'RED-OK' if (code != 0 and only_expected) else 'WRONG'
    print('%-8s %-28s exit=%d errors=%d %s' % (status, name, code, len(errors),
                                               (hit[0][:70] if hit else '|'.join(errors)[:70])))
    if status != 'RED-OK':
        failed += 1

code, errors = run()
print('=== 还原后：exit=%d errors=%d ===' % (code, len(errors)))
if code != 0 or errors or failed:
    print('注入验证不完整：', failed, '条没有只把该红的那条弄红')
    sys.exit(1)
print('%d 条注入全部只让对应判据变红，还原后重新变绿' % len(CASES))
