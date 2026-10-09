#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ci/gen_grapheme_tables.py 的自测。

与 ci/test_gen_changelog.py 同样刻意写成「一次性脚本」而不是测试框架用例：
本仓库的单元测试都在 C++ 侧，为一个构建期脚本引入 pytest 不划算。

关键是**不联网**：用内联的 UCD 片段造出一个临时数据目录，再断言解析、合并、
渲染与 --check 闸门的行为。这样即使 unicode.org 不可达或改了文件布局，
这里也能明确报出「是生成器坏了」而不是让人误以为属性表漂移。

跑法：

    python ci/test_gen_grapheme_tables.py

仅依赖 Python 3 标准库。
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'gen_grapheme_tables.py')
sys.path.insert(0, HERE)

import gen_grapheme_tables as gen  # noqa: E402  （上面刚把 HERE 加进 sys.path）

VERSION = '9.9.9'

GCB_FIXTURE = """\
# fixture GraphemeBreakProperty.txt
0000..0009    ; Control # <control>
000A          ; LF
000D          ; CR
0300..0301    ; Extend # combining
0302..036F    ; Extend # 与上一段相邻且同类，应当被合并
1100..115F    ; L
1F1E6..1F1FF  ; Regional_Indicator
200D          ; ZWJ
AC00          ; LV   # 刻意省略进表（可由码点算出）
AC01          ; LVT
0041..005A    ; Other
"""

EMOJI_FIXTURE = """\
# fixture emoji-data.txt
1F600..1F64F  ; Emoji_Presentation # 不是本次要收集的属性
1F600..1F64F  ; Extended_Pictographic
2702          ; Extended_Pictographic
"""

CORE_FIXTURE = """\
# fixture DerivedCoreProperties.txt
0041..005A    ; Alphabetic # 与 InCB 无关，必须被跳过
0300..036F    ; InCB; Extend # <combining>
0915..0939    ; InCB; Consonant # <devanagari>
094D          ; InCB; Linker # virama
FFFF          ; InCB; None # 显式的 None 不产生区间
"""


def write_ucd(directory):
    with open(os.path.join(directory, 'GraphemeBreakProperty.txt'), 'w',
              encoding='utf-8', newline='') as handle:
        handle.write(GCB_FIXTURE)
    with open(os.path.join(directory, 'emoji-data.txt'), 'w',
              encoding='utf-8', newline='') as handle:
        handle.write(EMOJI_FIXTURE)
    with open(os.path.join(directory, 'DerivedCoreProperties.txt'), 'w',
              encoding='utf-8', newline='') as handle:
        handle.write(CORE_FIXTURE)


def collect_fixture(ucd_dir):
    return gen.collect(VERSION, ucd_dir, cache_dir=ucd_dir)


def case_parse_and_merge():
    tmp = tempfile.mkdtemp(prefix='grapheme-gen-')
    try:
        write_ucd(tmp)
        tables = collect_fixture(tmp)

        # Other / LV / LVT 刻意不进表；相邻同类的 Extend 应被合并
        assert tables['gcb'] == [
            (0x0000, 0x0009, 'kGcbControl'),
            (0x000A, 0x000A, 'kGcbLF'),
            (0x000D, 0x000D, 'kGcbCR'),
            (0x0300, 0x036F, 'kGcbExtend'),
            (0x1100, 0x115F, 'kGcbL'),
            (0x200D, 0x200D, 'kGcbZWJ'),
            (0x1F1E6, 0x1F1FF, 'kGcbRegionalIndicator'),
        ], tables['gcb']

        assert tables['extended_pictographic'] == [
            (0x2702, 0x2702, '1'),
            (0x1F600, 0x1F64F, '1'),
        ], tables['extended_pictographic']

        # InCB：None 被丢弃，非 InCB 行被跳过
        assert tables['incb'] == [
            (0x0300, 0x036F, 'kIncbExtend'),
            (0x0915, 0x0939, 'kIncbConsonant'),
            (0x094D, 0x094D, 'kIncbLinker'),
        ], tables['incb']
        print('  ok  parse/merge: Other/LV/LVT 被省略，相邻同类被合并')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def case_unknown_property_rejected():
    tmp = tempfile.mkdtemp(prefix='grapheme-gen-')
    try:
        write_ucd(tmp)
        # 新增一个 UCD 里出现、但映射表里没有的属性值：必须明确报错，
        # 而不是静默按 Other 处理（否则升级 Unicode 版本时会悄悄丢属性）
        with open(os.path.join(tmp, 'GraphemeBreakProperty.txt'), 'a',
                  encoding='utf-8', newline='') as handle:
            handle.write('0600..0605 ; PrependFancy\n')
        try:
            collect_fixture(tmp)
        except gen.UcdError as exc:
            assert 'PrependFancy' in str(exc), exc
            print('  ok  unknown property value is rejected loudly')
        else:
            raise AssertionError('未知属性值应当抛出 UcdError')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run_tool(ucd_dir, output, *extra):
    proc = subprocess.run(
        [sys.executable, TOOL, '--ucd-dir', ucd_dir, '--output', output,
         '--version', VERSION] + list(extra),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return (proc.returncode,
            proc.stdout.decode('utf-8', 'replace'),
            proc.stderr.decode('utf-8', 'replace'))


def case_render_and_check_gate():
    tmp = tempfile.mkdtemp(prefix='grapheme-gen-')
    try:
        write_ucd(tmp)
        output = os.path.join(tmp, 'grapheme_tables_full.inc')

        rc, out, err = run_tool(tmp, output)
        assert rc == 0, (rc, err)
        with open(output, 'r', encoding='utf-8', newline='') as handle:
            text = handle.read()
        assert 'kGraphemeTableUnicodeVersion[] = "%s"' % VERSION in text, text
        assert 'static const GraphemeRange kGcbRanges[]' in text
        assert 'kGcbExtend' in text and 'kIncbLinker' in text
        assert '{0x000300u, 0x00036Fu, kGcbExtend},' in text

        # 内容一致时闸门放行
        rc, out, err = run_tool(tmp, output, '--check')
        assert rc == 0, (rc, err)

        # 内容漂移时闸门必须失败（退出码 1，与 gen_changelog 一致）
        with open(output, 'a', encoding='utf-8', newline='') as handle:
            handle.write('// 手工改动\n')
        rc, out, err = run_tool(tmp, output, '--check')
        assert rc == 1, (rc, out, err)
        assert '不一致' in err, err
        print('  ok  render + --check gate')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def case_missing_input_is_usage_error():
    tmp = tempfile.mkdtemp(prefix='grapheme-gen-')
    try:
        rc, out, err = run_tool(tmp, os.path.join(tmp, 'out.inc'))
        assert rc == 2, (rc, out, err)
        assert '找不到' in err, err
        print('  ok  missing UCD file reports a usage error (exit 2)')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        # Windows 控制台默认 GBK，中文断言信息会变乱码
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    case_parse_and_merge()
    case_unknown_property_rejected()
    case_render_and_check_gate()
    case_missing_input_is_usage_error()
    print('gen_grapheme_tables self-test: ALL OK')


if __name__ == '__main__':
    main()
