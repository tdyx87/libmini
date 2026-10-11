# CHANGELOG 门禁（最容易让 CI 变红）

> 上级索引：[`AGENTS.md`](../../AGENTS.md)。相关：[构建与测试](build-and-test.md)。

CI 有独立的 `changelog` job，会跑 `python ci/gen_changelog.py --check`，
**未发布段与 `git log` 不一致就直接失败**。

提交代码后、push 前，跑：

```bash
python ci/gen_changelog.py            # 刷新「未发布」的提交清单
python ci/test_gen_changelog.py       # 生成器自测
python ci/gen_changelog.py --check    # 校验是否与历史一致（CI 用的那条）
```

分类由**提交信息首词**决定（`Add`→新增，`Fix`→修复，`Document`/`Bump`→内部，
其余按关键词表落到「变更」）。首词命中不了时，在提交正文加 trailer：

```text
Fix retry ceiling off-by-one

Category: 修复          # 可选值：新增 / 变更 / 修复 / 内部
Changelog-Skip: yes     # 完全不进清单
```

---

## 关键机制（否则会被「漂移」误报搞懵）

- **有一提交延迟**：一条提交无法把自己写进 CHANGELOG，所以生成与校验都用区间
  `<最近 tag>..<最新提交>~1`（「除最新一条外的所有提交都必须已收录」）。
- 想在提交信息改完后把**最新一条**也收进来，用 `--until HEAD`。
- **只改 `CHANGELOG.md` 与 `ci/gen_changelog*.py` 的提交不进清单**
  （否则会「收录→产生新提交→又需收录」，无限循环）。

> ⚠️ 真实踩坑：`--until HEAD` 生成 → 提交 → 校验 `--check`（比对 `HEAD~1`）
> 会因为你多收了一条而失败。正确顺序有两种：
>
> **A. 单提交收尾**：先提交代码 → 用 `--until HEAD~1`（或直接
> `python ci/gen_changelog.py`）生成应包含到 `HEAD~1` 的内容 → 把它
> `git commit --amend` 进当前提交。
>
> **B. 两步走**：先提交代码 → 再单独一个 commit 刷 CHANGELOG（更直观）。

---

## 发版流程

```bash
V=0.3.1
python ci/gen_changelog.py --release $V     # 冻结未发布段 + 打印发布说明
# 把 CMakeLists 的 LIBMINI_VERSION 也一起改了（工具会比对，不一致直接报错，
# 避免 CHANGELOG 写 0.3.1、包名还是 0.3.0）
python ci/gen_changelog.py                  # 需要看清单时刷新
git commit -am "Release $V" && git push origin main
git tag -a v$V -m "Release $V" && git push origin v$V
python ci/gen_changelog.py                  # tag 之后重新基线清单
```

`--release` 刻意**不动生成清单**：打完 tag 前 CI 的校验区间是
`<最近 tag>..HEAD~1`，清单必须仍与那段历史一致，挪走或清空都会让发布提交当场变红。
清单在 tag 之后那次刷新里自然归零。

---

## 参考

- [`ci/gen_changelog.py`](../../ci/gen_changelog.py) — 生成/校验/发版主体
  （`--print` 只打印、`--until` 指定终点、`--release` 冻结、`--notes` 取发布段落）
- [`ci/test_gen_changelog.py`](../../ci/test_gen_changelog.py) — 生成器自测
- [`CHANGELOG.md`](../../CHANGELOG.md) — 被维护的文件（人工要点 + 生成清单）
- [`CMakeLists.txt`](../../CMakeLists.txt) — `LIBMINI_VERSION`（`--release` 会比对）
- [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) — `changelog` job（门禁）
- [`.github/workflows/release.yml`](../../.github/workflows/release.yml) — `--notes` 的实际调用点
- [Keep a Changelog](https://keepachangelog.com/) — 格式约定
