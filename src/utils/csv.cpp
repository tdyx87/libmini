#include "csv.h"

#include "file_utils.h"

namespace libmini {

namespace {

constexpr char kUtf8Bom[3] = { '\xEF', '\xBB', '\xBF' };

bool has_bom(const std::string& text)
{
    return text.size() >= 3 && text[0] == kUtf8Bom[0] &&
           text[1] == kUtf8Bom[1] && text[2] == kUtf8Bom[2];
}

// 字段是否需要加引号。
//
// 只看四个字符：分隔符、引号、CR、LF。前两个是 RFC 明文要求；后两个是
// 实务必需——字段里含换行时，不加引号的输出根本无法再解析回来（解析器会
// 在那里断行），格式本身就损坏了。
//
// 刻意**不**因为首尾空格而加引号。RFC 明确说空格是字段内容，而引号并不
// 改变这一点（Excel 显示层是否 trim 是另一回事），所以按空格加引号只会
// 让输出多出无意义的引号，还和「parse 后值不变」的保证无关。
bool needs_quotes(const std::string& field, const CsvOptions& options)
{
    if (options.quote == '\0') {
        return false;
    }
    for (std::size_t i = 0; i < field.size(); ++i) {
        const char c = field[i];
        if (c == options.delimiter || c == options.quote || c == '\r' ||
            c == '\n') {
            return true;
        }
    }
    return false;
}

void append_field(std::string& out, const std::string& field,
                  const CsvOptions& options)
{
    if (!needs_quotes(field, options)) {
        out.append(field);
        return;
    }
    out.push_back(options.quote);
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == options.quote) {
            out.push_back(options.quote);  // RFC 4180：引号写作两个
        }
        out.push_back(field[i]);
    }
    out.push_back(options.quote);
}

}  // namespace

bool csv_parse(const std::string& text, std::vector<std::vector<std::string>>& out,
               const CsvOptions& options)
{
    out.clear();
    if (options.delimiter == '\0') {
        return false;
    }
    if (options.quote != '\0' && options.quote == options.delimiter) {
        return false;  // 分隔符与引号同字符，状态机无解
    }

    std::size_t pos = 0;
    if (options.skip_bom && has_bom(text)) {
        pos = 3;
    }

    std::vector<std::string> row;
    std::string field;
    bool in_quotes = false;
    bool quote_closed = false;  // 已闭合，期望分隔符 / 行尾 / EOF
    // 当前字段是否已开始写入内容。用于两处判断：
    //   * 只有字段开头的引号才是引号（字段中间的 " 是字面量）
    //   * 区分「空行」（跳过）与「只有空字段的行」（保留）
    // 分隔符必须重置它，否则本行第二个及之后的字段的首个引号会被误判成字面量。
    bool field_started = false;

    const std::size_t size = text.size();
    while (pos <= size) {
        if (pos == size) {
            // 走到文本末尾：若还处于引号内说明引号未闭合。
            // 这必须报错而不是悄悄收尾——截断的 CSV 与完整 CSV 长得一样，
            // 悄悄通过等于把上游的 bug 变成下游的脏数据。
            if (in_quotes) {
                return false;
            }
            if (field_started || !field.empty() || !row.empty()) {
                row.push_back(field);
                out.push_back(row);
            }
            break;
        }

        const char c = text[pos];

        if (in_quotes) {
            if (c == options.quote) {
                // 连续两个引号 = 字面引号；否则闭合
                if (pos + 1 < size && text[pos + 1] == options.quote) {
                    field.push_back(options.quote);
                    pos += 2;
                    continue;
                }
                in_quotes = false;
                quote_closed = true;
                ++pos;
                continue;
            }
            // 引号内的 CR/LF 都是字段内容（RFC 规则 4）
            field.push_back(c);
            ++pos;
            continue;
        }

        if (quote_closed) {
            // 闭合引号后只接受分隔符、行尾或 EOF。
            // 其余字符按字面追加（容忍 Excel 写出的 `"a" ,b` 这类脏数据），
            // 而不是整行判否——解析器一遇到非规范输出就拒绝，等于让
            // 调用方自己重写一遍清洗逻辑。
            if (c == options.delimiter || c == '\n' || c == '\r') {
                quote_closed = false;
                continue;  // 交给下一轮按分隔符 / 行尾处理
            }
            field.push_back(c);
            field_started = true;
            ++pos;
            continue;
        }

        if (c == options.quote && field.empty() && !field_started) {
            // 只在字段起始处才把引号当引号。字段中间的 " 是字面量，
            // 否则 `a"b` 会被误判成非法数据。
            in_quotes = true;
            field_started = true;
            ++pos;
            continue;
        }

        if (c == options.delimiter) {
            row.push_back(field);
            field.clear();
            field_started = false;  // 新字段：引号重新可以当引号
            ++pos;
            continue;
        }

        if (c == '\r' || c == '\n') {
            // 整行为空时产出「零字段的行」，而不是「一个空字段的行」——
            // 两种表示都常见（各工具不同），但 Python csv / pandas 读出的是
            // 前者。混用会让调用方的列数判断在尾随空行处突然 +1。
            const bool line_is_empty =
                !field_started && field.empty() && row.empty();
            if (!line_is_empty) {
                row.push_back(field);
            }
            if (!options.skip_blank_lines || !line_is_empty) {
                out.push_back(row);
            }
            row.clear();
            field.clear();
            field_started = false;
            // CRLF 作为一个行尾吃掉两个字符；裸 CR / LF 各自算一个行尾
            // （裸 CR 是老 Mac 格式，虽然 RFC 不提但真实文件里存在）
            if (c == '\r' && pos + 1 < size && text[pos + 1] == '\n') {
                pos += 2;
            } else {
                ++pos;
            }
            continue;
        }

        field.push_back(c);
        field_started = true;
        ++pos;
    }

    // 解析成功。这里刻意**不**校验「各行字段数一致」（RFC 规则 6）：
    // 现实中的 CSV 常有尾部空列或缺列的 ragged 行，直接判否会让模块
    // 在真实数据上完全不可用。字段数对齐是调用方的语义问题。
    return true;
}

std::string csv_serialize(const std::vector<std::vector<std::string>>& rows,
                          bool use_crlf, const CsvOptions& options)
{
    if (options.delimiter == '\0') {
        return std::string();
    }
    if (options.quote != '\0' && options.quote == options.delimiter) {
        return std::string();
    }

    const std::string eol = use_crlf ? "\r\n" : "\n";
    std::string out;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const std::vector<std::string>& row = rows[r];
        for (std::size_t c = 0; c < row.size(); ++c) {
            if (c > 0) {
                out.push_back(options.delimiter);
            }
            append_field(out, row[c], options);
        }
        // 每行都带行尾（含最后一行）。少了最后一个换行的话，`cat` 的
        // 提示符会粘在数据后面，拼接两个 CSV 也会把末行与首行粘在一起。
        out.append(eol);
    }
    return out;
}

bool csv_read_file(const std::string& path,
                   std::vector<std::vector<std::string>>& out,
                   const CsvOptions& options)
{
    out.clear();
    // 先判存在性：read_file 对「文件不存在」和「文件存在但为空」都返回
    // 空串，直接往下走会把 I/O 失败报成「成功读到 0 行」——调用方拿到
    // false 之外的信号会以为文件是空文件，把缺失当成了正常状态。
    if (!file_exists(path)) {
        return false;
    }
    return csv_parse(read_file(path), out, options);
}

bool csv_write_file(const std::string& path,
                    const std::vector<std::vector<std::string>>& rows,
                    bool use_crlf, const CsvOptions& options)
{
    const std::string text = csv_serialize(rows, use_crlf, options);
    if (text.empty() && !rows.empty()) {
        return false;  // 参数非法导致序列化失败（分隔符为 NUL 等）
    }
    return write_file(path, text);
}

bool csv_read_file_with_header(const std::string& path,
                               std::vector<std::string>& header,
                               std::vector<std::vector<std::string>>& rows,
                               const CsvOptions& options)
{
    header.clear();
    rows.clear();

    std::vector<std::vector<std::string>> all;
    if (!csv_read_file(path, all, options)) {
        return false;
    }
    if (all.empty()) {
        return true;  // 空文件：无表头也无数据，这不是错误
    }
    header = all[0];
    rows.assign(all.begin() + 1, all.end());
    return true;
}

int csv_column_index(const std::vector<std::string>& header,
                     const std::string& name)
{
    for (std::size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace libmini
