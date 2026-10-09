#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 Unicode 字符数据库（UCD）生成字形簇分割所需的属性表。

为什么需要它
------------
libmini 默认用一份手写的「启发式」属性表做扩展字形簇（extended grapheme
cluster）分割——它体积小、无外部依赖，覆盖组合字符、emoji ZWJ 序列、区域
指示符、变体选择符与 Hangul 音节，但不含完整属性表。打开
`-DLIBMINI_UNICODE_FULL_GRAPHEME=ON` 时改用本脚本生成的完整表，与 UAX #29
逐条对齐（含 Indic 连字 GB9c 所需的 InCB 属性）。

生成的是**源码**（`src/utils/grapheme_tables_full.inc`），随仓库提交：这样
开关打开时无需联网也能编译，而想要更新 Unicode 版本时重跑本脚本即可。

数据来源（UCD 版本可指定，默认钉在一个确定版本上以保证可复现）
--------------------------------------------------------------
    ucd/auxiliary/GraphemeBreakProperty.txt   Grapheme_Cluster_Break 属性
    ucd/emoji/emoji-data.txt                  Extended_Pictographic 属性
    ucd/DerivedCoreProperties.txt             InCB（Indic_Conjunct_Break）

刻意不生成的类别：`Other`（全表 0 值就是它）、`LV` / `LVT`（Hangul 音节可由
码点算术精确推出，见 grapheme.cpp），这样表能小一大截。

用法
----
    python ci/gen_grapheme_tables.py                     # 联网下载并重写 .inc
    python ci/gen_grapheme_tables.py --print             # 只打印，不写文件
    python ci/gen_grapheme_tables.py --check             # 校验 .inc 是否最新
    python ci/gen_grapheme_tables.py --ucd-dir DIR       # 用本地已下载的 UCD
    python ci/gen_grapheme_tables.py --version 17.0.0    # 换一个 UCD 版本

退出码：0 成功 / 1 已漂移（仅 --check）/ 2 用法或环境错误。

仅依赖 Python 3 标准库。
"""

import argparse
import io
import os
import re
import sys
import tempfile
import urllib.request

# 钉死默认版本：属性表按 Unicode 版本演进，显式指定才能保证「同样输入 → 同样输出」
DEFAULT_VERSION = "18.0.0"
MIRROR = "https://www.unicode.org/Public/{version}/ucd/{path}"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUTPUT = os.path.join(REPO_ROOT, "src", "utils", "grapheme_tables_full.inc")

# UCD 属性值 → C++ 枚举常量（与 src/utils/grapheme.cpp 保持一致）
GCB_ENUM = {
    "CR": "kGcbCR",
    "LF": "kGcbLF",
    "Control": "kGcbControl",
    "Extend": "kGcbExtend",
    "ZWJ": "kGcbZWJ",
    "Regional_Indicator": "kGcbRegionalIndicator",
    "Prepend": "kGcbPrepend",
    "SpacingMark": "kGcbSpacingMark",
    "L": "kGcbL",
    "V": "kGcbV",
    "T": "kGcbT",
}
# 需要生成、但用 0 值（Other）表示的类别：C++ 侧把它们留作默认分支
GCB_SKIP = frozenset(["Other", "LV", "LVT"])

INCB_ENUM = {
    "Consonant": "kIncbConsonant",
    "Extend": "kIncbExtend",
    "Linker": "kIncbLinker",
}

RANGE_RE = re.compile(
    r"^([0-9A-Fa-f]{4,6})(?:\.\.([0-9A-Fa-f]{4,6}))?\s*;\s*([A-Za-z_]+)")
INCB_RE = re.compile(
    r"^([0-9A-Fa-f]{4,6})(?:\.\.([0-9A-Fa-f]{4,6}))?\s*;\s*InCB\s*;\s*([A-Za-z]+)")


class UcdError(Exception):
    """用法或环境错误（退出码 2）。"""


def fetch(version, rel_path, cache_dir):
    """取一份 UCD 文件：命中缓存就直接读，否则下载后写缓存。"""
    local = os.path.join(cache_dir, os.path.basename(rel_path))
    if os.path.isfile(local):
        with io.open(local, "r", encoding="utf-8") as handle:
            return handle.read()
    url = MIRROR.format(version=version, path=rel_path)
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
    except OSError as exc:  # URLError 是 OSError 的子类
        raise UcdError("下载 %s 失败：%s\n（可用 --ucd-dir 指向本地 UCD 目录）"
                       % (url, exc))
    text = data.decode("utf-8")
    try:
        os.makedirs(cache_dir, exist_ok=True)
        with io.open(local, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
    except OSError:
        pass  # 缓存目录只读不算错误，下次重新下载即可
    return text


def load_source(version, rel_path, ucd_dir, cache_dir):
    if ucd_dir:
        local = os.path.join(ucd_dir, os.path.basename(rel_path))
        if not os.path.isfile(local):
            raise UcdError("--ucd-dir 里找不到 %s" % os.path.basename(rel_path))
        with io.open(local, "r", encoding="utf-8") as handle:
            return handle.read()
    return fetch(version, rel_path, cache_dir)


def parse_ranges(text, value_of, pattern=RANGE_RE, skip_unmatched=False):
    """把 UCD 行解析成 [(lo, hi, 属性值)]；`#` 之后是注释，忽略。

    skip_unmatched=True 用于 DerivedCoreProperties.txt 这类「一个文件里混了
    多组属性」的数据：只挑出匹配 pattern 的行，其余原样跳过。
    """
    out = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = pattern.match(line)
        if not m:
            if skip_unmatched:
                continue
            raise UcdError("无法解析的 UCD 行：%r" % raw)
        lo = int(m.group(1), 16)
        hi = int(m.group(2), 16) if m.group(2) else lo
        value = value_of(m)
        if value is None:
            continue
        out.append((lo, hi, value))
    return out


def merge(entries):
    """按码点排序并合并相邻/同类区间，让表尽量短（二进制查找因此更快）。"""
    entries = sorted(entries)
    merged = []
    for lo, hi, value in entries:
        if merged and merged[-1][1] + 1 == lo and merged[-1][2] == value:
            merged[-1] = (merged[-1][0], hi, value)
        else:
            merged.append((lo, hi, value))
    return merged


def collect(version, ucd_dir, cache_dir):
    gcb_text = load_source(version, "auxiliary/GraphemeBreakProperty.txt",
                           ucd_dir, cache_dir)
    emoji_text = load_source(version, "emoji/emoji-data.txt", ucd_dir, cache_dir)
    core_text = load_source(version, "DerivedCoreProperties.txt", ucd_dir,
                            cache_dir)

    gcb = parse_ranges(
        gcb_text,
        lambda m: None if m.group(3) in GCB_SKIP
        else GCB_ENUM.get(m.group(3)) or _unknown(m.group(3)))
    extended_pictographic = parse_ranges(
        emoji_text,
        lambda m: "1" if m.group(3) == "Extended_Pictographic" else None)
    incb = parse_ranges(
        core_text,
        lambda m: None if m.group(3) == "None"
        else INCB_ENUM.get(m.group(3)) or _unknown(m.group(3)),
        INCB_RE, skip_unmatched=True)

    return {
        "gcb": merge(gcb),
        "extended_pictographic": merge(extended_pictographic),
        "incb": merge(incb),
    }


def _unknown(value):
    raise UcdError("UCD 里出现未预期的属性值 %r，请同步更新本脚本的映射表"
                   % value)


def render_table(name, entries, comment):
    lines = ["", "// %s" % comment, "static const GraphemeRange %s[] = {" % name]
    for lo, hi, value in entries:
        lines.append("    {0x%06Xu, 0x%06Xu, %s}," % (lo, hi, value))
    lines.append("};")
    lines.append("static const std::size_t %sCount = sizeof(%s) / sizeof(%s[0]);"
                 % (name, name, name))
    return lines


def render(version, tables):
    header = [
        "// 本文件由 ci/gen_grapheme_tables.py 从 Unicode %s 的 UCD 生成，请勿手改。"
        % version,
        "// 重新生成：python ci/gen_grapheme_tables.py",
        "// 仅在 -DLIBMINI_UNICODE_FULL_GRAPHEME=ON 时被 grapheme.cpp 包含。",
        "// 依赖调用方在此之前定义的 GraphemeRange 结构体与枚举常量。",
        "",
        "static const char kGraphemeTableUnicodeVersion[] = \"%s\";" % version,
    ]
    body = []
    body += render_table(
        "kGcbRanges", tables["gcb"],
        "Grapheme_Cluster_Break（Other / LV / LVT 已省略：前者是全表默认值，"
        "后两者可由码点算出）")
    body += render_table(
        "kExtendedPictographicRanges", tables["extended_pictographic"],
        "Extended_Pictographic（emoji ZWJ 序列的 GB11 规则用）")
    body += render_table(
        "kIncbRanges", tables["incb"],
        "Indic_Conjunct_Break（GB9c 规则用）")
    return "\n".join(header + body) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(
        description="从 UCD 生成字形簇分割属性表（完整模式用）")
    parser.add_argument("--output", default=DEFAULT_OUTPUT,
                        help="输出路径（默认 src/utils/grapheme_tables_full.inc）")
    parser.add_argument("--version", default=DEFAULT_VERSION,
                        help="UCD 版本（默认 %s）" % DEFAULT_VERSION)
    parser.add_argument("--ucd-dir", default=None,
                        help="本地 UCD 目录（含 GraphemeBreakProperty.txt 等）")
    parser.add_argument("--cache-dir", default=None,
                        help="下载缓存目录（默认系统临时目录）")
    parser.add_argument("--print", dest="print_only", action="store_true",
                        help="只打印结果，不写文件")
    parser.add_argument("--check", dest="check", action="store_true",
                        help="校验已提交的 .inc 是否与本脚本一致；不一致退出码 1")
    args = parser.parse_args(argv)

    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")

    cache_dir = args.cache_dir or os.path.join(tempfile.gettempdir(),
                                               "libmini-ucd", args.version)
    try:
        tables = collect(args.version, args.ucd_dir, cache_dir)
    except UcdError as exc:
        sys.stderr.write("gen_grapheme_tables: %s\n" % exc)
        return 2

    text = render(args.version, tables)
    counts = "GCB %d / ExtPict %d / InCB %d 段" % (
        len(tables["gcb"]), len(tables["extended_pictographic"]),
        len(tables["incb"]))

    if args.print_only:
        sys.stdout.write(text)
        sys.stderr.write("gen_grapheme_tables: %s\n" % counts)
        return 0

    if args.check:
        try:
            with io.open(args.output, "r", encoding="utf-8", newline="") as handle:
                current = handle.read()
        except OSError as exc:
            sys.stderr.write("gen_grapheme_tables: 读取 %s 失败：%s\n"
                             % (args.output, exc))
            return 2
        if current.replace("\r\n", "\n") == text.replace("\r\n", "\n"):
            return 0
        sys.stderr.write(
            "gen_grapheme_tables: %s 与 Unicode %s 的 UCD 不一致，"
            "请跑 `python ci/gen_grapheme_tables.py` 刷新。\n"
            % (args.output, args.version))
        return 1

    try:
        with io.open(args.output, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    except OSError as exc:
        sys.stderr.write("gen_grapheme_tables: 写入 %s 失败：%s\n"
                         % (args.output, exc))
        return 2
    sys.stdout.write("gen_grapheme_tables: 已更新 %s（Unicode %s，%s）\n"
                     % (args.output, args.version, counts))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
