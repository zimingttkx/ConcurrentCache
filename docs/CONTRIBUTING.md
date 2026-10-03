# 贡献与提交流程

本项目是单人维护，但提交流程按"每一次改动都必须先过 CI 才能进 main"来设计。

## 日常流程

```bash
git checkout -b <type>/<scope>          # 不要直接改 main
# 改完提交
git push -u origin <type>/<scope>
gh pr create --base main --head <type>/<scope> --fill
gh pr merge --auto --squash              # 绿了自己合，红了自己看
```

`main` 开了分支保护：不能直接 push、不能 force push、不能删分支，六个 required check 全绿才会合并。`--auto` 让 GitHub 在检查变绿时自动 squash 合并，所以单人仓库也不需要"等别人 review"，但每一笔改动都留下一个 PR 作为记录。

## 六个 required check

| check 名 | 内容 | 红了一般是什么原因 |
|---|---|---|
| `build-release` | Release + `-Werror` 构建全部 target | 编译错误、gcc-14 新警告 |
| `build-assert` | Debug 构建（`assert` 真的生效） | Debug 独有的编译问题 |
| `gate-tests` | `ctest -L gate -j1` | 真实回归——这一层的用例必须永远绿 |
| `consistency` | `scripts/ci/check_consistency.py` | 三张表脱钩、新增孤儿测试文件、配置项没人读、端口不一致 |
| `asan-smoke` | Debug + ASan 构建 | ASan 下的编译/链接问题 |
| `docker-build` | `docker build` | `.dockerignore` 与构建脚本又对不上了 |

`contract-tests`（可见不拦）与 `daily.yml` 里的 sanitizer / 慢压测 / e2e 不在 required 列表里。

## 加测试的规则

1. 新用例在 `test/CMakeLists.txt` 的 `CC_TESTS` 注册表里加一行：`名称|target|标签|超时秒`。
2. **必须带 `TIMEOUT`**，否则一个挂死的用例会烧满 GitHub 的 6 小时上限。
3. 不确定它能不能过的，先打 `contract`；连续绿之后改成 `gate`，那一行 diff 就是"门禁收紧一格"。
4. `test/` 下新增 `.cpp` 一定会被 `consistency` 检查：要么进某个 target，要么写进 `ci/known-failures.txt` 并说明为什么它现在编不过。没有第三种状态——"躺在目录里不编译"正是这个项目之前坏掉的地方。

## 修缺陷时顺手做的一件事

`ci/known-failures.txt` 里每一项都必须在当前真实失败。修好一个就删一行；不删的话 `consistency` 会直接报"已经不再触发，请删掉这一行"。名单只能变短。
