// passvault —— libmini 应用示例 5：加密笔记 / 密钥保管库
//
// 一个「口令保护的本地保险库」：条目（任意文本）以 JSON 组织，整体用
// PasswordSeal（口令 → PBKDF2-HMAC-SHA256 → AES-256-GCM）密封成一个自描述、
// 可版本化的二进制文件。口令错、文件被改一个字节、格式不认识，都表现为同一种
// 失败——这正是 AEAD 该有的样子（不区分「密码错」和「文件坏了」，免得给攻击者
// 免费信息）。支持导入/导出 JSON（明文，用于跨设备迁移或审计）。
//
// 用法：
//   passvault --demo                                    自检：走完所有环节
//   passvault --vault me.vault init                      建库（口令见下）
//   passvault --vault me.vault set github --value 'ghp_xxx'
//   passvault --vault me.vault get github
//   passvault --vault me.vault list
//   passvault --vault me.vault remove github
//   passvault --vault me.vault export plain.json
//   passvault --vault plain.json import new.vault
//   passvault --vault me.vault info                      只读文件头（KDF 参数）
//   passvault --vault me.vault verify                    校验可解密
//
// 口令来源：--master 参数 / 环境变量 LIBMINI_VAULT_MASTER / 从 stdin 读一行。
// 注意：命令行参数会留在进程列表里，生产使用建议走环境变量或标准输入。
//
// 用到的 libmini 模块：kdf（PasswordSeal/Pbkdf2HmacSha256）、aes_gcm、secure_random、
// json_utils、file_utils、encoding（Hex）、args、console。
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "app_common.h"
#include "libmini.h"
#include "utils/ui_panels.h"

using namespace libmini;
using namespace app;

namespace {

const char* kMagic = "LMVAULT1\n";
const std::size_t kMagicSize = 9;
const std::size_t kDefaultIterations = Pbkdf2HmacSha256::kDefaultIterations;

std::string master_password(const Args& args)
{
    const std::string from_flag = trim(args.get_string("master"));
    if (!from_flag.empty()) {
        return from_flag;
    }
    const std::string from_env = env_get("LIBMINI_VAULT_MASTER", "");
    if (!from_env.empty()) {
        return from_env;
    }
    std::cout << "主口令（输入会回显，生产请用 --master 之外的方式）: " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return trim(line);
}

// ---------------- 保险库 ----------------

struct Entry
{
    std::string name;
    std::string value;
    std::string tags;
    std::int64_t updated_ms = 0;
};

class Vault
{
public:
    bool exists() const { return file_exists(path_); }

    void set_path(const std::string& path) { path_ = path; }
    const std::string& path() const { return path_; }

    // 建库：写一个只含元信息的空库
    bool create(const std::string& master, std::size_t iterations, std::string* error)
    {
        payload_ = JsonValue::object();
        payload_["version"] = 1;
        payload_["created_at"] = current_time_string();
        payload_["entries"] = JsonValue::object();
        iterations_ = iterations;
        return save(master, error);
    }

    bool load(const std::string& master, std::string* error)
    {
        const std::string raw = read_file(path_);
        if (raw.empty()) {
            *error = "保险库不存在或为空: " + path_;
            return false;
        }
        if (raw.size() <= kMagicSize || raw.compare(0, kMagicSize, kMagic) != 0) {
            *error = "不是 libmini 保险库文件（魔数不符）";
            return false;
        }
        const std::string sealed = raw.substr(kMagicSize);
        std::string plaintext;
        if (!PasswordSeal::open(master, sealed, plaintext)) {
            // 口令错 / 文件被篡改 / 格式不认识，在密码学上无法区分，也不该区分
            *error = "解密失败：口令错误或文件已被篡改";
            return false;
        }
        try {
            payload_ = parse_json(plaintext);
        } catch (const std::exception& e) {
            *error = std::string("内容不是合法 JSON: ") + e.what();
            return false;
        }
        PasswordSeal::Info info;
        iterations_ = PasswordSeal::inspect(sealed, info) ? info.iterations : kDefaultIterations;
        return true;
    }

    bool save(const std::string& master, std::string* error)
    {
        const std::string plaintext = to_json_string(payload_);
        const std::string sealed = PasswordSeal::seal(master, plaintext, "", iterations_);
        if (sealed.empty()) {
            *error = "密封失败（熵源不可用或参数非法）";
            return false;
        }
        const std::string blob = std::string(kMagic, kMagicSize) + sealed;
        if (!write_file_atomic(path_, blob)) {
            *error = "写入失败: " + path_;
            return false;
        }
        return true;
    }

    std::vector<Entry> entries() const
    {
        std::vector<Entry> out;
        if (!payload_.is_object() || payload_.find("entries") == payload_.end() ||
            !payload_["entries"].is_object()) {
            return out;
        }
        const JsonValue& object = payload_["entries"];
        for (JsonValue::const_iterator it = object.begin(); it != object.end(); ++it) {
            Entry entry;
            entry.name = it.key();
            entry.value = jstr(*it, "value");
            entry.tags = jstr(*it, "tags");
            entry.updated_ms = jint(*it, "updated_ms", 0);
            out.push_back(entry);
        }
        return out;
    }

    bool set_entry(const std::string& name, const std::string& value, const std::string& tags)
    {
        if (name.empty()) {
            return false;
        }
        if (payload_.find("entries") == payload_.end()) {
            payload_["entries"] = JsonValue::object();
        }
        JsonValue entry = JsonValue::object();
        entry["value"] = value;
        entry["tags"] = tags;
        entry["updated_ms"] = current_timestamp_ms();
        payload_["entries"][name] = entry;
        return true;
    }

    bool get_entry(const std::string& name, std::string* value) const
    {
        if (!payload_.is_object() || payload_.find("entries") == payload_.end()) {
            return false;
        }
        const JsonValue& object = payload_["entries"];
        if (!object.is_object() || object.find(name) == object.end()) {
            return false;
        }
        *value = jstr(object[name], "value");
        return true;
    }

    bool remove_entry(const std::string& name, std::string* error)
    {
        if (!payload_.is_object() || payload_.find("entries") == payload_.end() ||
            payload_["entries"].find(name) == payload_["entries"].end()) {
            *error = "条目不存在: " + name;
            return false;
        }
        payload_["entries"].erase(name);
        return true;
    }

    // 只读文件头：不解密也能看出用的是哪套 KDF 参数（迁移/审计用）
    bool inspect_header(JsonValue* out, std::string* error) const
    {
        const std::string raw = read_file(path_);
        if (raw.size() <= kMagicSize || raw.compare(0, kMagicSize, kMagic) != 0) {
            *error = "不是 libmini 保险库文件";
            return false;
        }
        const std::string sealed = raw.substr(kMagicSize);
        PasswordSeal::Info info;
        if (!PasswordSeal::inspect(sealed, info)) {
            *error = "文件头无法识别";
            return false;
        }
        JsonValue node = JsonValue::object();
        node["version"] = static_cast<std::int64_t>(info.version);
        node["kdf_id"] = static_cast<std::int64_t>(info.kdf_id);
        node["iterations"] = static_cast<std::int64_t>(info.iterations);
        node["salt_hex"] = Hex::encode(info.salt);
        node["nonce_hex"] = Hex::encode(info.nonce);
        node["ciphertext_size"] = static_cast<std::int64_t>(info.ciphertext_size);
        node["needs_reseal"] = PasswordSeal::needs_reseal(sealed);
        *out = node;
        return true;
    }

    JsonValue export_payload() const { return payload_; }

private:
    std::string path_;
    JsonValue payload_;
    std::size_t iterations_ = kDefaultIterations;
};

// ---------------- demo ----------------

// 把文件里某个字节取反，用于证明「改一个字节就解不开」
bool flip_one_byte(const std::string& path)
{
    std::string content = read_file(path);
    if (content.size() < kMagicSize + 24) {
        return false;
    }
    const std::size_t index = content.size() - 8;  // 落在密文/认证标签里
    content[index] = static_cast<char>(content[index] ^ 0x5A);
    return write_file(path, content);
}

int run_demo()
{
    DemoReport report("passvault demo");

    // 1) 原语层：PBKDF2 与 AES-GCM
    const std::string salt_a = Pbkdf2HmacSha256::random_salt();
    const std::string salt_b = Pbkdf2HmacSha256::random_salt();
    report.check(salt_a.size() == Pbkdf2HmacSha256::kSaltSize, "生成 16 字节随机盐");
    const std::string key_a = Pbkdf2HmacSha256::derive("pw", salt_a, 5000, 32);
    const std::string key_a2 = Pbkdf2HmacSha256::derive("pw", salt_a, 5000, 32);
    const std::string key_b = Pbkdf2HmacSha256::derive("pw", salt_b, 5000, 32);
    report.check(key_a.size() == 32 && key_a == key_a2 && key_a != key_b,
                 "PBKDF2：同口令同盐得同密钥，换盐即不同");

    const std::string sealed_once = Aes256Gcm::seal(key_a, "top secret");
    report.check(Aes256Gcm::open(key_a, sealed_once) == "top secret", "AES-256-GCM 密封/解封");
    std::string tampered = sealed_once;
    tampered[tampered.size() - 1] = static_cast<char>(tampered[tampered.size() - 1] ^ 0x01);
    report.check(Aes256Gcm::open(key_a, tampered).empty(),
                 "AES-256-GCM 改一个字节即解密失败");

    // 2) 建库 / 写条目
    const std::string workspace = make_workspace("passvault_demo");
    report.check(!workspace.empty(), "创建临时工作目录");
    if (workspace.empty()) {
        return report.finish();
    }
    const std::string vault_path = workspace_file(workspace, "demo.vault");
    const std::string master = "correct horse battery staple";
    const std::size_t iterations = 20000;  // demo 用低迭代，保证秒级完成

    Vault vault;
    vault.set_path(vault_path);
    std::string error;
    report.check(vault.create(master, iterations, &error), "创建保险库: " + error);
    report.check(file_exists(vault_path) &&
                     read_file(vault_path).compare(0, kMagicSize, kMagic) == 0,
                 "落盘文件带 libmini 保险库魔数");

    report.check(vault.set_entry("github", "ghp_demo_token", "code") &&
                     vault.set_entry("router", "admin/12345", "home") &&
                     vault.set_entry("note", "备用线路走 10.0.0.2", "") &&
                     vault.save(master, &error),
                 "写入 3 条条目并保存");

    // 3) 重新加载并读取
    Vault reopened;
    reopened.set_path(vault_path);
    std::string value;
    report.check(reopened.load(master, &error) && reopened.entries().size() == 3,
                 "重新加载后仍是 3 条：" + error);
    report.check(reopened.get_entry("github", &value) && value == "ghp_demo_token",
                 "取回条目内容正确");

    // 4) 口令错
    Vault wrong;
    wrong.set_path(vault_path);
    report.check(!wrong.load("wrong password", &error), "口令错误时解密失败");

    // 5) 文件头可读（不解密）
    JsonValue header;
    report.check(reopened.inspect_header(&header, &error) &&
                     header["iterations"].get<std::int64_t>() ==
                         static_cast<std::int64_t>(iterations),
                 "只读文件头能拿到 KDF 迭代次数");

    // 6) 篡改检测
    const std::string original = read_file(vault_path);
    report.check(flip_one_byte(vault_path), "构造篡改后的拷贝");
    Vault tamper_probe;
    tamper_probe.set_path(vault_path);
    report.check(!tamper_probe.load(master, &error), "篡改一个字节后解密失败");
    report.check(write_file(vault_path, original), "恢复原始文件");

    // 7) 导出 / 导入
    Vault source;
    source.set_path(vault_path);
    report.check(source.load(master, &error), "重新打开保险库");
    const std::string exported_path = workspace_file(workspace, "export.json");
    report.check(write_file(exported_path, to_json_string(source.export_payload())),
                 "导出明文 JSON");
    const std::string imported_path = workspace_file(workspace, "imported.vault");
    Vault imported;
    imported.set_path(imported_path);
    report.check(imported.create("another master", iterations, &error), "建第二个保险库");
    const std::vector<Entry> entries = source.entries();
    bool imported_all = true;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!imported.set_entry(entries[i].name, entries[i].value, entries[i].tags)) {
            imported_all = false;
        }
    }
    report.check(imported_all && imported.save("another master", &error),
                 "把导出的条目导入新库（换主口令）");
    Vault check_import;
    check_import.set_path(imported_path);
    report.check(check_import.load("another master", &error) &&
                     check_import.entries().size() == 3 &&
                     check_import.get_entry("note", &value) &&
                     value == "备用线路走 10.0.0.2",
                 "新库可用新口令解出同样的 3 条");

    // 8) 低迭代参数的库应提示需要重封
    const std::string weak_path = workspace_file(workspace, "weak.vault");
    Vault weak;
    weak.set_path(weak_path);
    report.check(weak.create("pw", 1000, &error), "用 1000 次迭代建库（低参数）");
    JsonValue weak_header;
    report.check(weak.inspect_header(&weak_header, &error) &&
                     weak_header["needs_reseal"].get<bool>(),
                 "低迭代库被标记 needs_reseal");

    // 9) 删除条目
    Vault mutable_vault;
    mutable_vault.set_path(vault_path);
    report.check(mutable_vault.load(master, &error) &&
                     mutable_vault.remove_entry("router", &error) &&
                     mutable_vault.save(master, &error),
                 "删除条目并保存");
    Vault after_delete;
    after_delete.set_path(vault_path);
    report.check(after_delete.load(master, &error) && after_delete.entries().size() == 2,
                 "删除后只剩 2 条");
    report.check(!after_delete.remove_entry("router", &error), "删除不存在的条目返回失败");

    remove_tree(workspace);
    return report.finish();
}

// ---------------- 原生窗口模式 ----------------

// `--ui`：保管库是本地文件工具（没有常驻服务面），窗口用「运行器」形状：
// 拉起 `passvault --demo` 子进程，实时展示建库/读写/篡改检测/导入导出的进展。
// 保险库路径与主口令透传给子进程，保证窗口里跑的就是同一份数据。
int run_ui(const std::string& argv0, const std::vector<std::string>& extra_args, bool selftest)
{
    ui::AppWindowSpec spec;
    spec.title = "passvault 加密保管库";
    spec.subtitle = "PBKDF2 + AES-256-GCM（窗口内运行的是 --demo 全流程）";
    spec.port = 0;
    spec.child_args.push_back(app::self_exe(argv0));

    bool has_demo = false;
    for (std::size_t i = 0; i < extra_args.size(); ++i) {
        spec.child_args.push_back(extra_args[i]);
        if (extra_args[i] == "--demo") {
            has_demo = true;
        }
    }
    if (!has_demo) {
        spec.child_args.push_back("--demo");
    }

    if (selftest) {
        spec.frames = 40;
        spec.report = true;
        spec.shot_path = app::workspace_file(app::make_workspace("passvault_ui"), "window.bmp");
    }

    std::string error;
    const int code = ui::run_app_window(spec, &error);
    if (code == 2) {
        std::cerr << "无法打开原生窗口（" << error << "），请直接用命令行模式。\n";
    }
    return code;
}

// ---------------- 正常模式 ----------------

int fail(const std::string& message)
{
    std::cerr << message << "\n";
    return 1;
}

int run_cli(Args& args)
{
    const std::vector<std::string> rest = args.remaining();
    if (rest.empty()) {
        std::cerr << "需要命令：init / set / get / list / remove / export / import / info / verify\n";
        return 2;
    }
    const std::string command = to_lower(rest[0]);
    const std::string vault_path = trim(args.get_string("vault"));
    const std::size_t iterations =
        args.get_int("iterations") > 0
            ? static_cast<std::size_t>(args.get_int("iterations"))
            : kDefaultIterations;

    // 明文 JSON 的导出/导入不需要口令
    if (command == "export") {
        if (vault_path.empty() || rest.size() < 2) {
            return fail("用法: passvault --vault <库> export <输出.json>");
        }
        Vault vault;
        vault.set_path(vault_path);
        std::string error;
        if (!vault.load(master_password(args), &error)) {
            return fail(error);
        }
        if (!write_file(rest[1], to_json_string(vault.export_payload()))) {
            return fail("写文件失败: " + rest[1]);
        }
        std::cout << "已导出 " << vault.entries().size() << " 条到 " << rest[1] << "\n";
        return 0;
    }
    if (command == "import") {
        if (rest.size() < 3) {
            return fail("用法: passvault import <明文.json> <新库.vault>");
        }
        const std::string text = read_file(rest[1]);
        if (text.empty()) {
            return fail("读不到: " + rest[1]);
        }
        JsonValue payload;
        try {
            payload = parse_json(text);
        } catch (const std::exception& e) {
            return fail(std::string("JSON 非法: ") + e.what());
        }
        Vault vault;
        vault.set_path(rest[2]);
        std::string error;
        const std::string master = master_password(args);
        if (!vault.create(master, iterations, &error)) {
            return fail(error);
        }
        std::size_t added = 0;
        if (payload.is_object() && payload.find("entries") != payload.end() &&
            payload["entries"].is_object()) {
            const JsonValue& object = payload["entries"];
            for (JsonValue::const_iterator it = object.begin(); it != object.end(); ++it) {
                if (vault.set_entry(it.key(), jstr(*it, "value"), jstr(*it, "tags"))) {
                    ++added;
                }
            }
        }
        if (!vault.save(master, &error)) {
            return fail(error);
        }
        std::cout << "已导入 " << added << " 条到 " << rest[2] << "\n";
        return 0;
    }

    if (vault_path.empty()) {
        return fail("需要 --vault <库文件>");
    }
    Vault vault;
    vault.set_path(vault_path);
    std::string error;

    if (command == "init") {
        if (vault.exists()) {
            return fail("库已存在: " + vault_path);
        }
        if (!vault.create(master_password(args), iterations, &error)) {
            return fail(error);
        }
        std::cout << "已创建 " << vault_path << "（KDF 迭代 " << iterations << "）\n";
        return 0;
    }
    if (command == "info") {
        JsonValue header;
        if (!vault.inspect_header(&header, &error)) {
            return fail(error);
        }
        std::cout << to_json_string(header) << "\n";
        return 0;
    }
    if (command == "verify") {
        if (!vault.load(master_password(args), &error)) {
            return fail(error);
        }
        std::cout << "可解密，共 " << vault.entries().size() << " 条\n";
        return 0;
    }
    if (!vault.load(master_password(args), &error)) {
        return fail(error);
    }
    if (command == "list") {
        const std::vector<Entry> entries = vault.entries();
        if (entries.empty()) {
            std::cout << "（空库）\n";
            return 0;
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            std::cout << entries[i].name;
            if (!entries[i].tags.empty()) {
                std::cout << "  [" << entries[i].tags << "]";
            }
            std::cout << "  " << format_time(
                             std::chrono::system_clock::time_point(
                                 std::chrono::milliseconds(entries[i].updated_ms)))
                      << "  " << entries[i].value.size() << " 字节\n";
        }
        return 0;
    }
    if (command == "get") {
        if (rest.size() < 2) {
            return fail("用法: passvault --vault <库> get <条目>");
        }
        std::string value;
        if (!vault.get_entry(rest[1], &value)) {
            return fail("条目不存在: " + rest[1]);
        }
        std::cout << value << "\n";
        return 0;
    }
    if (command == "set") {
        if (rest.size() < 2) {
            return fail("用法: passvault --vault <库> set <条目> [--value <值>]");
        }
        std::string value = args.get_string("value");
        if (trim(args.get_string("value")).empty() && !args.has_flag("allow-empty")) {
            std::cout << "值: " << std::flush;
            std::getline(std::cin, value);
        }
        if (!vault.set_entry(rest[1], value, args.get_string("tags"))) {
            return fail("条目名不能为空");
        }
        if (!vault.save(master_password(args), &error)) {
            return fail(error);
        }
        std::cout << "已保存条目 " << rest[1] << "\n";
        return 0;
    }
    if (command == "remove" || command == "rm") {
        if (rest.size() < 2) {
            return fail("用法: passvault --vault <库> remove <条目>");
        }
        if (!vault.remove_entry(rest[1], &error)) {
            return fail(error);
        }
        if (!vault.save(master_password(args), &error)) {
            return fail(error);
        }
        std::cout << "已删除条目 " << rest[1] << "\n";
        return 0;
    }
    return fail("未知命令: " + command);
}

}  // namespace

int main(int argc, char** argv)
{
    Args args("passvault", "1.0",
              "libmini 应用：加密笔记/密钥保管库（PBKDF2 + AES-256-GCM 密封容器）");
    args.add_flag("demo", "", "自检模式：走完建库/读写/篡改检测/导入导出");
    args.add_option("vault", "v", "保险库文件路径", std::string(""));
    args.add_option("master", "m", "主口令（不给则读环境变量或 stdin）", std::string(""));
    args.add_option("value", "", "set 的值（不给则从 stdin 读一行）", std::string(""));
    args.add_option("tags", "", "条目标签（自由文本）", std::string(""));
    args.add_int("iterations", "", "KDF 迭代次数（仅建库时有效）", 0);
    args.add_flag("allow-empty", "", "允许写入空值而不提示输入");
    args.add_flag("ui", "", "打开原生窗口（运行器视图，Windows）");
    args.add_flag("ui-selftest", "", "窗口自检：限帧渲染 + 截图 + 结论（CI 用）");
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 2;
    }
    if (args.has_flag("demo")) {
        return run_demo();
    }
    if (args.has_flag("ui") || args.has_flag("ui-selftest")) {
        const std::string argv0 = (argc > 0 && argv[0] != 0) ? std::string(argv[0])
                                                             : std::string("passvault");
        std::vector<std::string> passthrough;
        const std::string vault_path = trim(args.get_string("vault"));
        if (!vault_path.empty()) {
            passthrough.push_back("--vault");
            passthrough.push_back(vault_path);
        }
        const std::string master = trim(args.get_string("master"));
        if (!master.empty()) {
            passthrough.push_back("--master");
            passthrough.push_back(master);
        }
        return run_ui(argv0, passthrough, args.has_flag("ui-selftest"));
    }
    return run_cli(args);
}
