#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 git log 生成 CHANGELOG.md 的「未发布」提交清单，并校验其是否与历史一致。

为什么需要它
------------
CHANGELOG 里手写的「未发布」段必然会和提交历史漂移：合并前忘了补、两个人
各写一遍、或者干脆忘了。本工具把「未发布」段的**提交清单**部分交给 git log 生成，
并提供 --check 闸门供 CI 使用，于是漂移会在 PR 上直接失败，而不是等到发版。

设计上的三个取舍
----------------
1. **只生成清单，不生成要点。** CHANGELOG.md 的「未发布」段分两部分：上面手写的
   要点（写清楚「为什么」，质量高、体量可控）和下面自动生成的提交清单（保证不
   漏、不重、顺序稳定）。发版时删掉清单小节即可。
2. **只覆盖标记之间的内容。** 标记外的任何字符都不会被动，因此历史版本段落
   （已冻结的散文）永不被改写。HEAD 恰落在某个 tag 上时（打 tag 的那一
   提交）整个校验与刷新直接跳过：那一刻区间起点就是它自己，「漂移」是假的。
3. **有一步固有延迟，且是刻意的。** 一条提交无法把自己写进 CHANGELOG，因此
   生成与校验都用同一个区间终点：`<tag>..<最新提交>~1`——「除最新一条外的
   所有提交都必须已收录」。两者共用终点很关键：否则每次「刷新完立刻校验」
   都会误报漂移。最新一条由下一次运行补上；想连它一起看用 `--until HEAD`。

分类规则
--------
提交信息首词决定分类，规则表见 CATEGORY_KEYWORDS。规则表覆盖不全或判错时，
在提交信息正文里加一行 trailer 覆盖：

    Category: 修复            # 新增 / 变更 / 修复 / 内部
    Changelog-Skip: yes       # 完全不出现在清单里

用法
----
    python ci/gen_changelog.py              # 刷新 CHANGELOG.md 里的清单
    python ci/gen_changelog.py --print      # 只打印，不写文件
    python ci/gen_changelog.py --check      # 校验是否与历史一致（CI 用）
    python ci/gen_changelog.py --force      # HEAD 在 tag 上也照常处理
    python ci/gen_changelog.py --until HEAD # 连最新一条一起收录
    python ci/gen_changelog.py --since v0.2.0 --print

发版时：

    python ci/gen_changelog.py --release 0.3.1            # 冻结段落 + 打印发布说明
    python ci/gen_changelog.py --release 0.3.1 --dry-run  # 只校验与打印
    python ci/gen_changelog.py --notes 0.3.1 > notes.md   # 抽取已冻结段落
    git tag -a v0.3.1 -m "..." && git push origin v0.3.1
    python ci/gen_changelog.py                           # tag 之后重新基线清单

--release 刻意**不动生成清单**：打完 tag 前，CI 的校验区间是
<最近 tag>..HEAD~1，清单必须仍与那段历史一致；挪走或清空都会让发布提交当场
变红。清单在 tag 之后的那次刷新里自然归零。

退出码：0 一致 / 1 已漂移（仅 --check）/ 2 用法或环境错误。

仅依赖 Python 3 标准库，Windows / Linux / macOS 通用。
"""

import argparse
import os
import re
import subprocess
import sys

BEGIN_MARKER = "<!-- BEGIN generated:unreleased -->"
END_MARKER = "<!-- END generated:unreleased -->"

DEFAULT_CHANGELOG = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "CHANGELOG.md")

# 这些提交只是本工具在维护 CHANGELOG 自身，不构成用户可见变更：
# 收录它们会让 --check 永远失败（收录 → 产生新提交 → 又需要收录 → …）
MAINTENANCE_FILES = frozenset(["changelog.md", "ci/gen_changelog.py",
                               "ci/test_gen_changelog.py"])

# 提交信息首词 → 分类。**顺序敏感**：先匹配到的赢，所以更具体的类别放前面。
# 新增一条规则时请同时确认它不会误伤已有提交：
#     python ci/gen_changelog.py --since v0.1.0 --print
CATEGORY_KEYWORDS = (
    ("内部", (
        "bump version", "ci ", "ci:", "changelog", "document", "docs",
        "readme", "chore", "refactor", "workflow", "test budget",
        "rebaseline", "reword",
        "assertion", "flaky", "timing", "timeout budget", "test store",
    )),
    ("修复", (
        "fix", "prevent", "correct", "repair", "guard against", "avoid",
    )),
    ("新增", (
        "add", "introduce", "expose", "support ",
    )),
    ("变更", (
        "align", "make", "widen", "narrow", "rename", "drop", "change",
        "tighten", "wire", "filter", "read ", "export", "raise", "move ",
        "deprecate", "harden", "gate ",
    )),
)
DEFAULT_CATEGORY = "变更"

# 早于本约定落地的提交：按实际性质手工归类，新提交不要往这里加
# （新提交应该用提交信息里的 `Category:` trailer 归类）
SHORT_SHA_OVERRIDES = {
    "1d0fe85": "修复",  # 换掉 macOS 上会抛编译错的 LL_ADDR 宏
    "9d04b15": "修复",  # macOS 卷列表混进伪文件系统
    "c8408c1": "内部",  # 只加 CHANGELOG
}

# 提交信息里的版本提升提交：不进清单，只用来报告「下一版本」
BUMP_RE = re.compile(r"^bump version to (\d+\.\d+\.\d+)", re.IGNORECASE)

CATEGORY_ORDER = ("新增", "变更", "修复", "内部")

# 版本号格式与「## [x.y.z] - date」版本标题
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")
VERSION_HEADING_RE = re.compile(r"^## \[(?P<version>[^\]]+)\]", re.M)
UNRELEASED_HEADING = "## [未发布]"
COMPARE_BASE = "https://github.com/tdyx87/libmini/compare"

CATEGORY_ALIASES = {
    "新增": "新增", "add": "新增", "added": "新增", "new": "新增",
    "feature": "新增", "feat": "新增",
    "变更": "变更", "change": "变更", "changed": "变更",
    "refactor": "变更", "update": "变更",
    "修复": "修复", "fix": "修复", "fixed": "修复", "bugfix": "修复",
    "内部": "内部", "internal": "内部", "chore": "内部", "docs": "内部",
    "ci": "内部", "test": "内部", "tests": "内部",
}

BULLET_ESCAPE = re.compile(r"\s+")


class ChangelogError(Exception):
    """用法或环境错误（退出码 2）。"""


def git(args, cwd):
    """跑一条 git 命令并返回 stdout。失败抛 ChangelogError。"""
    try:
        proc = subprocess.run(["git"] + args, cwd=cwd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
    except OSError as exc:
        raise ChangelogError("无法执行 git：%s" % exc)
    if proc.returncode != 0:
        detail = proc.stderr.decode("utf-8", "replace").strip()
        raise ChangelogError("git %s 失败：%s" % (" ".join(args), detail))
    return proc.stdout.decode("utf-8", "replace")


def latest_tag(cwd):
    try:
        out = git(["describe", "--tags", "--abbrev=0"], cwd).strip()
    except ChangelogError:
        out = ""
    if not out:
        raise ChangelogError("仓库里还没有任何 tag，无法确定未发布区间")
    return out


def head_is_tagged(cwd):
    """HEAD 是否恰好落在某个 tag 上。

    打 tag 的那一提交里，「未发布」段已经被冻结成正式版本段落，提交清单
    对它不再有意义（区间起点就是它自己）。此时校验必然「漂移」，但那不是
    真漂移；刷新也会改写已冻结的内容。所以 HEAD 在 tag 上时跳过校验与刷新，
    等下一个提交自然重建。--print 是只读查看，不跳过。
    """
    try:
        git(["describe", "--tags", "--exact-match", "HEAD"], cwd)
    except ChangelogError:
        return False
    return True


def content_tip(cwd):
    """当前工作树对应的最新非合并提交。

    PR 检出拿到的是 refs/pull/N/merge（一个合并提交），直接拿它当区间终点会
    让 --check 永远漂移：区间里少了 PR 分支的最新一条，块里却已经收录了它。
    检出为合并提交时退回第二父提交，那才是 PR 分支 tip。
    """
    try:
        out = git(["rev-parse", "--verify", "--quiet", "HEAD^2"], cwd).strip()
    except ChangelogError:
        return "HEAD"
    return out or "HEAD"


def resolve_until(cwd, until):
    """把 'HEAD~1' 之类解析成实际 sha；只有一条提交时退回 HEAD。"""
    if until == "HEAD":
        return "HEAD"
    try:
        return git(["rev-parse", "--verify", "--quiet", until], cwd).strip() or "HEAD"
    except ChangelogError:
        return "HEAD"


def commit_range_url(cwd):
    """仓库 web 地址，用于把短 hash 变成可点链接。认 https 与 scp 式 git 地址。"""
    try:
        remote = git(["remote", "get-url", "origin"], cwd).strip()
    except ChangelogError:
        return ""
    if not remote:
        return ""
    remote = re.sub(r"\.git$", "", remote)
    # https://host/owner/repo、ssh://git@host/owner/repo
    m = re.match(r"^(?:https?://|ssh://)(?:[^/@]+@)?([^/]+)/(.+)$", remote)
    if m:
        return "https://%s/%s" % (m.group(1), m.group(2))
    # git@host:owner/repo
    m = re.match(r"^(?:[^@/]+@)?([^:/]+):(.+)$", remote)
    if m:
        return "https://%s/%s" % (m.group(1), m.group(2))
    return ""


def parse_trailers(body):
    """从提交信息正文里取 Category: / Changelog-Skip: trailer。"""
    category = None
    skip = False
    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        m = re.match(r"^(?:changelog-)?category\s*[:=]\s*(.+)$", line, re.I)
        if m and category is None:
            key = m.group(1).strip().strip("`").lower()
            category = CATEGORY_ALIASES.get(key)
        m = re.match(r"^changelog-skip\s*[:=]\s*(.+)$", line, re.I)
        if m:
            value = m.group(1).strip().lower()
            if value in ("yes", "true", "1", "y"):
                skip = True
    return category, skip


def classify(subject, body, short_sha=""):
    override, skip = parse_trailers(body)
    if skip:
        return None
    if override:
        return override
    if short_sha in SHORT_SHA_OVERRIDES:
        return SHORT_SHA_OVERRIDES[short_sha]
    lowered = subject.lower().lstrip()
    for category, keywords in CATEGORY_KEYWORDS:
        for keyword in keywords:
            if lowered.startswith(keyword):
                return category
    return DEFAULT_CATEGORY


class Commit(object):
    __slots__ = ("sha", "subject", "body", "files")

    def __init__(self, sha, subject, body, files):
        self.sha = sha
        self.subject = subject
        self.body = body
        self.files = files

    @property
    def short_sha(self):
        return self.sha[:7]

    def is_maintenance(self):
        if not self.files:
            return False
        return all(f.lower() in MAINTENANCE_FILES for f in self.files)


def collect_commits(cwd, since, until):
    """取 since..until 的非合并提交，带主题、正文与改动文件。"""
    rng = "%s..%s" % (since, until)
    # %x00 作条目分隔符：提交信息正文可能含换行，但不可能含 NUL
    raw = git(["log", "--no-merges", "--format=%H%x1f%s%x1f%B%x00", rng], cwd)
    commits = []
    for chunk in raw.split("\x00"):
        chunk = chunk.strip("\n")
        if not chunk:
            continue
        parts = chunk.split("\x1f", 2)
        if len(parts) < 3:
            continue
        commits.append(Commit(parts[0].strip(), parts[1].strip(),
                              parts[2].strip(), []))
    if not commits:
        return commits

    by_sha = dict((c.sha, c) for c in commits)
    raw_names = git(["log", "--no-merges", "--format=%x1e%H", "--name-only", rng],
                    cwd)
    for chunk in raw_names.split("\x1e"):
        chunk = chunk.strip("\n")
        if not chunk:
            continue
        lines = chunk.split("\n")
        sha = lines[0].strip()
        if sha not in by_sha:
            continue
        by_sha[sha].files = [ln.strip() for ln in lines[1:] if ln.strip()]

    # 旧的在前：发布说明读起来是时间线，不是倒序流水账
    commits.reverse()
    return commits


def collect(cwd, since, until):
    """返回 (归类后的条目, 下一版本 or None)。"""
    entries = []
    next_version = None
    for commit in collect_commits(cwd, since, until):
        if commit.is_maintenance():
            continue
        bump = BUMP_RE.match(commit.subject)
        if bump:
            next_version = (bump.group(1), commit.short_sha)
            continue
        category = classify(commit.subject, commit.body, commit.short_sha)
        if category is None:
            continue
        entries.append((category, commit))
    return entries, next_version


def render(entries, repo_url):
    """渲染标记之间的完整文本（含标记与说明行）。"""
    lines = [BEGIN_MARKER,
             "",
             "> 本小节由 `ci/gen_changelog.py` 从 `git log` 生成，**勿手改**。",
             "> 提交信息首词决定分类；判错就在正文加一行 `Category: 修复`",
             "> （可选值：新增 / 变更 / 修复 / 内部），不想收录就加",
             "> `Changelog-Skip: yes`。改完提交信息后跑一次",
             "> `python ci/gen_changelog.py` 刷新。",
             "",
             "### 提交清单"]
    if not entries:
        lines.append("")
        lines.append("（暂无提交）")
    else:
        for category in CATEGORY_ORDER:
            picked = [c for cat, c in entries if cat == category]
            if not picked:
                continue
            lines.append("")
            lines.append("#### " + category)
            lines.append("")
            for commit in picked:
                subject = BULLET_ESCAPE.sub(" ", commit.subject).strip()
                link = "[%s](%s/commit/%s)" % (commit.short_sha, repo_url,
                                               commit.sha)
                lines.append("- %s（%s）" % (subject, link))
    lines.append("")
    lines.append(END_MARKER)
    return "\n".join(lines)


def normalize_newlines(text):
    """统一为 LF。Windows 上 core.autocrlf 会把工作区文件重写成 CRLF，
    而 git 历史的提交信息永远用 LF——不做归一，--check 会把行尾差异当成漂移。"""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def dominant_newline(text):
    """跟随文件已有的行尾风格写回，避免刷新一次就把整个文件改行尾。"""
    return "\r\n" if "\r\n" in text else "\n"


def splice(text, block):
    """用 block 替换标记之间的内容；标记不存在则追加到文件末尾。"""
    newline = dominant_newline(text)
    if newline != "\n":
        block = block.replace("\n", newline)
    begin = text.find(BEGIN_MARKER)
    end = text.find(END_MARKER)
    if begin < 0 or end < 0 or end < begin:
        return text.rstrip("\r\n") + newline + newline + block + newline, False
    return text[:begin] + block + text[end + len(END_MARKER):], True


def current_block(text):
    begin = text.find(BEGIN_MARKER)
    end = text.find(END_MARKER)
    if begin < 0 or end < 0 or end < begin:
        return None
    return normalize_newlines(
        text[begin:end + len(END_MARKER)]).strip()


def find_version_heading(text):
    """返回 [(版本号, 标题起点, 段落到下一个 '## ' 之前)]，按文件顺序。"""
    matches = list(VERSION_HEADING_RE.finditer(text))
    sections = []
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        sections.append((m.group("version"), m.start(), end))
    return sections


def read_libmini_version(cwd):
    """从 CMakeLists.txt 读 LIBMINI_VERSION，用于防止 CHANGELOG 与包名不一致。"""
    path = os.path.join(cwd, "CMakeLists.txt")
    if not os.path.isfile(path):
        return None
    try:
        text = read_text(path)
    except (IOError, OSError, UnicodeDecodeError):
        return None
    m = re.search(r"^set\(LIBMINI_VERSION\s+([0-9.]+)\)", text, re.M)
    return m.group(1) if m else None


def release_notes(text, version):
    """抽出某个已冻结版本段落（发布页正文用）。找不到返回 None。"""
    wanted = version[1:] if version.startswith("v") else version
    for name, start, end in find_version_heading(text):
        if name.lstrip("v") == wanted:
            return text[start:end].rstrip() + "\n"
    return None


def do_release(text, version, date_str, cwd):
    """把「未发布」段里的人工要点冻结成正式版本段落。

    刻意**保留**生成清单原样不动：打完 tag 后清单才重新基线，而 tag 之前
    CI 的校验区间是 <最近 tag>..HEAD~1，清单必须仍与那段历史一致。把清单
    挪走或清空都会让发布提交当场变红。
    """
    if not VERSION_RE.match(version):
        raise ChangelogError("版本号格式不对：%s（应为 x.y.z）" % version)

    sections = find_version_heading(text)
    if not sections or sections[0][0] != "未发布":
        raise ChangelogError("文件顶部的版本段落不是「未发布」，无法冻结")
    if any(name.lstrip("v") == version for name, _, _ in sections):
        raise ChangelogError("版本 %s 已在 CHANGELOG 里" % version)

    begin = text.find(BEGIN_MARKER)
    end = text.find(END_MARKER)
    if begin < 0 or end < 0 or end < begin:
        raise ChangelogError("找不到生成标记，无法确定要点与清单的分界")
    if begin > sections[0][2]:
        raise ChangelogError("生成标记不在「未发布」段内，请检查文件结构")

    prev_version = sections[1][0] if len(sections) > 1 else None
    if not prev_version:
        raise ChangelogError("没有更早的版本段落可比对，无法生成 compare 链接")

    # 要点 = 未发布标题之后、生成标记之前的全部 '###' 小节
    head = text[:begin]
    i_prose = head.find("\n### ")
    if i_prose < 0:
        raise ChangelogError("「未发布」段里没有人工要点，只有生成清单")
    note_part = head[:i_prose].rstrip("\n")
    prose = head[i_prose:].strip("\n")

    frozen = sections[1]
    rest = text[frozen[1]:]
    out = (note_part + "\n\n"
           + text[begin:end + len(END_MARKER)] + "\n\n"
           + "## [%s] - %s\n\n" % (version, date_str)
           + prose + "\n\n"
           + rest)

    # compare 链接插在上一版本链接之前，保持倒序。tag 一律带 v 前缀：
    # 版本标题里没有 v（## [0.3.1]），但 compare URL 用的是 tag 名。
    link = "[%s]: %s/v%s...v%s\n" % (version, COMPARE_BASE, prev_version,
                                    version)
    idx = out.find("[%s]: " % prev_version)
    if idx < 0:
        out = out.rstrip("\n") + "\n" + link
    else:
        out = out[:idx] + link + out[idx:]

    libmini_version = read_libmini_version(cwd)
    if libmini_version and libmini_version != version:
        raise ChangelogError(
            "CHANGELOG 版本 %s 与 CMakeLists 的 LIBMINI_VERSION %s 不一致"
            % (version, libmini_version))
    return out


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def run_release(args, cwd):
    """--release：冻结段落 + 打印可当发布页正文的要点。"""
    import datetime

    date_str = args.date or datetime.date.today().isoformat()
    if not re.match(r"^\d{4}-\d{2}-\d{2}$", date_str):
        sys.stderr.write("gen_changelog: 日期格式应为 YYYY-MM-DD：%s\n" % date_str)
        return 2
    try:
        text = read_text(args.path)
        out = do_release(text, args.release, date_str, cwd)
    except (IOError, OSError) as exc:
        sys.stderr.write("gen_changelog: 读取 %s 失败：%s\n" % (args.path, exc))
        return 2
    except UnicodeDecodeError:
        sys.stderr.write("gen_changelog: %s 不是 UTF-8 编码，无法处理\n"
                         % args.path)
        return 2
    except ChangelogError as exc:
        sys.stderr.write("gen_changelog: %s\n" % exc)
        return 2

    notes = release_notes(out, args.release)
    if notes is None:
        sys.stderr.write("gen_changelog: 冻结后找不到 %s 段落\n" % args.release)
        return 2

    if args.dry_run:
        sys.stderr.write("gen_changelog: --dry-run，未写文件\n")
        sys.stdout.write(notes)
        return 0
    try:
        write_text(args.path, out)
    except (IOError, OSError) as exc:
        sys.stderr.write("gen_changelog: 写入 %s 失败：%s\n" % (args.path, exc))
        return 2
    sys.stderr.write("gen_changelog: 已冻结 %s（%s），生成清单保持原样；"
                     "打完 tag 后再跑一次刷新即可重新基线\n"
                     % (args.release, date_str))
    sys.stdout.write(notes)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="从 git log 生成 / 校验 CHANGELOG.md 的未发布提交清单")
    parser.add_argument("--path", default=DEFAULT_CHANGELOG,
                        help="CHANGELOG.md 路径（默认仓库根目录）")
    parser.add_argument("--since", default=None,
                        help="起点（默认最近一个 tag）")
    parser.add_argument("--until", default=None, help="终点（默认 HEAD）")
    parser.add_argument("--print", dest="print_only", action="store_true",
                        help="只打印生成结果，不写文件")
    parser.add_argument("--check", dest="check", action="store_true",
                        help="校验清单是否与历史一致；不一致时退出码 1")
    parser.add_argument("--force", dest="force", action="store_true",
                        help="即使 HEAD 在 tag 上也照常处理（发版后重新基线用）")
    parser.add_argument("--release", dest="release", default=None,
                        metavar="X.Y.Z",
                        help="把未发布段冻结成正式版本段落（发版前跑）")
    parser.add_argument("--date", dest="date", default=None,
                        help="版本段落日期 YYYY-MM-DD（--release 用，默认今天）")
    parser.add_argument("--notes", dest="notes", default=None, metavar="X.Y.Z",
                        help="只打印指定版本的段落（发布页正文用，不改文件）")
    parser.add_argument("--dry-run", dest="dry_run", action="store_true",
                        help="与 --release 搭配：只校验与打印，不写文件")
    args = parser.parse_args(argv)

    if hasattr(sys.stdout, "reconfigure"):
        # Windows 控制台默认 GBK，中文报错会变乱码
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")

    cwd = os.path.dirname(os.path.abspath(args.path)) or "."
    try:
        if args.notes:
            text = read_text(args.path)
            body = release_notes(text, args.notes)
            if body is None:
                raise ChangelogError("CHANGELOG 里没有版本 %s 的段落" % args.notes)
            sys.stdout.write(body)
            return 0

        if args.release:
            return run_release(args, cwd)

        if head_is_tagged(cwd) and not args.print_only and not args.force:
            sys.stderr.write("gen_changelog: HEAD 落在 tag 上，"
                             "未发布段已冻结为正式版本段落，跳过。\n")
            return 0
        since = args.since or latest_tag(cwd)
        # 生成与校验必须用**同一个**区间终点，否则每次「刷新完就校验」
        # 都会误报漂移。终点固定为 content_tip~1：最新一条提交无法把自己
        # 写进 CHANGELOG（下一轮补上）。显式 --until 则原样尊重。
        until = args.until or (content_tip(cwd) + "~1")
        until = resolve_until(cwd, until)
        entries, next_version = collect(cwd, since, until)
        block = render(entries, commit_range_url(cwd))
    except ChangelogError as exc:
        sys.stderr.write("gen_changelog: %s\n" % exc)
        return 2

    footer = ""
    if next_version:
        footer = "\n下一版本：%s（来自提交 %s）" % next_version

    if args.print_only:
        sys.stdout.write(block + "\n" + footer + "\n")
        return 0

    try:
        text = read_text(args.path)
    except (IOError, OSError) as exc:
        sys.stderr.write("gen_changelog: 读取 %s 失败：%s\n" % (args.path, exc))
        return 2
    except UnicodeDecodeError:
        # Windows 上用 GBK 打开写过中文的文件很容易踩到
        sys.stderr.write("gen_changelog: %s 不是 UTF-8 编码，无法处理\n"
                         % args.path)
        return 2

    if args.check:
        have = current_block(text)
        if have == normalize_newlines(block).strip():
            return 0
        sys.stderr.write(
            "gen_changelog: %s 的提交清单与 %s..%s 的提交历史不一致。\n"
            "              跑 `python ci/gen_changelog.py` 刷新后与代码一并提交。\n"
            "              注：--check 校验的是 <tag>..<最新提交>~1，最新一条必然\n"
            "              未收录（它无法把自己写进 CHANGELOG），由下一次运行补上。\n"
            "              刚改完提交信息就想把新条目收进来，用 --until HEAD。\n"
            % (args.path, since, until))
        return 1

    new_text, replaced = splice(text, block)
    if new_text == text:
        return 0
    try:
        write_text(args.path, new_text)
    except (IOError, OSError) as exc:
        sys.stderr.write("gen_changelog: 写入 %s 失败：%s\n" % (args.path, exc))
        return 2
    sys.stdout.write("gen_changelog: %s %s\n"
                     % ("已更新" if replaced else "已追加提交清单",
                        args.path))
    if footer:
        sys.stdout.write(footer.lstrip() + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))