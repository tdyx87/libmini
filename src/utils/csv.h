#ifndef LIBMINI_CSV_H
#define LIBMINI_CSV_H

#include <cstddef>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {

// CSV 读写（RFC 4180）。
//
// 库名已经带 mini，所以这里**不做**通用 CSV 库的活：不猜分隔符、不猜
// 引号风格、不读 Excel 的 BOM 之外的花活。只做 RFC 4180 规定的那一套，
// 把「方言」参数留给调用方自己决定。
//
// RFC 4180 的完整规则（这七条就是全部）：
//   1. 每条记录位于独立的一行，行以 CRLF 分隔；
//   2. 字段可用双引号包裹；
//   3. 字段内的双引号写作两个双引号（""）；
//   4. 字段内可以出现逗号、CR、LF；
//   5. 记录首尾的空格属于字段内容，不做 trim；
//   6. 每条记录的字段数应相同；
//   7. 文件可选地以 UTF-8 BOM 开头。
//
// 「不 trim 空格」这条最容易被想当然地违反：RFC 明确说空格属于值，
// 而且 trim 掉之后再写出去就与原文件不等价了。同理，解析不做任何类型
// 推断——"00123" 必须还是字符串，转成数字是调用方的决定。
//
// 与「教科书 CSV」的两处有意偏离（都朝宽松方向）：
//   1. 行尾接受 LF、CRLF 与裸 CR。RFC 4180 只规定 CRLF，但只认 CRLF 的
//      解析器在 Unix 工具链产出的文件上会得到「整份文件一行」的荒谬结果，
//      而报错更糟。裸 CR 是老 Mac 格式，真实文件里偶尔能碰到。
//      代价是未加引号的字段内无法表达裸 CR —— 需要时请加引号。
//   2. 闭合引号后若出现非分隔符/非行尾的字符（如 Excel 的 `"a" ,b`），
//      按字面追加而不是判否。解析器一遇到非规范输出就拒绝，等于把清洗
//      逻辑的活推给每个调用方。
//
// 失败语义：与库内其他模块一致，返回 bool / 空串，不抛异常。

// 一条记录（一行）= 若干字段。
typedef std::vector<std::string> CsvRow;
typedef std::vector<CsvRow> CsvTable;

// 解析选项
struct LIBMINI_API CsvOptions {
    // 字段分隔符，RFC 规定为逗号。改成 ';' 是欧洲 Excel 的方言，
    // 但那不是 RFC 4180，调用方要自己知道自己在偏离标准。
    char delimiter = ',';
    // 引号字符。置 0 表示禁用引号解析（此时所有字符都是字面量，
    // 含引号和分隔符），适合「本就不是 CSV」的管道数据。
    char quote = '"';
    // 跳过 UTF-8 BOM。RFC 允许文件带 BOM，但不带 BOM 才是主流；
    // 默认跳过是因为带 BOM 的文件在第一字段里会出现一个肉眼看不见的
    // U+FEFF，排查起来极其痛苦。
    bool skip_bom = true;
    // 丢弃完全空白的记录行（只有分隔符和空白的行）。
    // 默认开启：多数工具在文件末尾留一个换行，若当成记录会多出一行
    // 全空字段的数据。
    bool skip_blank_lines = true;
};

// 解析 CSV 文本。成功返回 true。
// 失败（引号未闭合、字段内出现裸的换行符而未加引号等）返回 false，
// 此时 out 内容未定义，调用方必须检查返回值。
LIBMINI_API bool csv_parse(const std::string& text,
                           std::vector<std::vector<std::string>>& out,
                           const CsvOptions& options = CsvOptions());

// 序列化。第二个参数为 true 时输出 CRLF（RFC 4180 规定的行结束符），
// false 时输出 LF（更贴近 Unix 工具链的默认，也便于塞进 JSON）。
// 引号、逗号、CR、LF 出现在字段中时自动加引号并按规则转义。
LIBMINI_API std::string csv_serialize(const std::vector<std::vector<std::string>>& rows,
                                      bool use_crlf = false,
                                      const CsvOptions& options = CsvOptions());

// 文件版便捷封装。读失败（不存在、无权限、含 BOM 之外的非法字节）返回 false。
LIBMINI_API bool csv_read_file(const std::string& path,
                               std::vector<std::vector<std::string>>& out,
                               const CsvOptions& options = CsvOptions());
LIBMINI_API bool csv_write_file(const std::string& path,
                                const std::vector<std::vector<std::string>>& rows,
                                bool use_crlf = false,
                                const CsvOptions& options = CsvOptions());

// 带表头的便捷读写：读时用第一行做列名，返回列名与数据行（不含表头）。
//
// 列名重复时不做合并也不去重——保持「有几列就有几个下标」的一致性，
// 否则按列名取值的调用方会拿到错位的字段。重复列名的判别交给调用方。
LIBMINI_API bool csv_read_file_with_header(const std::string& path,
                                           std::vector<std::string>& header,
                                           std::vector<std::vector<std::string>>& rows,
                                           const CsvOptions& options = CsvOptions());

// 在 rows 中查找列名对应的下标；不存在返回 -1。
// 大小写敏感，且不做 trim（与解析语义保持一致）。
LIBMINI_API int csv_column_index(const std::vector<std::string>& header,
                                 const std::string& name);

}  // namespace libmini

#endif  // LIBMINI_CSV_H
