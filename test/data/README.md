# 测试数据

## GraphemeBreakTest.txt

Unicode 官方的 UAX #29 符合性用例集（extended grapheme cluster）。文件随仓库提交，
由 `utils_test` 里的 `GraphemeTest.Uax29OfficialTestFile` 逐条断言，所以「符合性」
是每次构建都会重跑的回归测试，而不是一次性的手工验证。

- 来源：<https://www.unicode.org/Public/18.0.0/ucd/auxiliary/GraphemeBreakTest.txt>
- 版本：Unicode 18.0.0，与 `src/utils/grapheme_tables_full.inc` /
  `src/utils/grapheme_tables_heuristic.inc` 用的属性表同一版
- 版权：© Unicode, Inc.，条款见文件头部注释与
  <https://www.unicode.org/terms_of_use.html>
- 本仓库未改动原文件内容（头部注释、`# Lines:` 行都保留）

线路由图注入给测试：`test/CMakeLists.txt` 把绝对路径写进
`LIBMINI_GRAPHEME_TEST_DATA`（cache 变量 `LIBMINI_GRAPHEME_TEST_DATA_FILE`
可以指向别处的副本，用来试跑新版 UCD）。

### 换一版 UCD 时

1. 下载新版文件覆盖本目录的 `GraphemeBreakTest.txt`（保留头部注释）。
2. 用同一版 UCD 重新生成完整表：
   `python ci/gen_grapheme_tables.py --version <x.y.z>`。
3. 跑 `utils_test`。测试会校验「属性表版本 == 用例文件版本」，对不上会直接失败，
   提示该同步哪一边；迁移期间的临时偏差也能从失败信息里看到具体是哪条用例
   （码点 + 期望断点 + 实际断点）。
4. 启发式表（`src/utils/grapheme_tables_heuristic.inc`，人工维护）若因新版用例
   出现新的偏差，需要同步更新表与 `test_utils.cpp` 里的白名单，以及 README
   里的说明。
