#include "app_common.h"

#include <cctype>
#include <cstdio>
#include <cstddef>
#include <iostream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>  // readlink：定位自身可执行文件
#endif

using namespace libmini;

namespace app {

std::string make_workspace(const std::string& app_name, const std::string& dir)
{
    const std::string path = unique_temp_directory(app_name + "_", dir);
    if (path.empty()) {
        return path;
    }
    return path;
}

std::string workspace_file(const std::string& dir, const std::string& name)
{
    return path_join(dir, name);
}

std::string module_dir(const std::string& argv0)
{
#if defined(_WIN32)
    char buffer[MAX_PATH];
    const DWORD got = GetModuleFileNameA(0, buffer, MAX_PATH);
    if (got > 0 && got < MAX_PATH) {
        const std::string full(buffer, static_cast<std::size_t>(got));
        const std::size_t slash = full.find_last_of("\\/");
        if (slash != std::string::npos) {
            return full.substr(0, slash);
        }
    }
#else
    char buffer[4096];
    const ssize_t got = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (got > 0) {
        buffer[got] = '\0';
        const std::string full(buffer);
        const std::size_t slash = full.find_last_of('/');
        if (slash != std::string::npos) {
            return full.substr(0, slash);
        }
    }
#endif
    // 退路：argv[0] 的目录部分（相对路径就按当前目录理解）
    const std::size_t slash = argv0.find_last_of("\\/");
    if (slash != std::string::npos) {
        return argv0.substr(0, slash);
    }
    return std::string(".");
}

std::string self_exe(const std::string& argv0)
{
    std::string name = argv0;
    const std::size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    if (name.empty()) {
        name = "app";
    }
    return path_join(module_dir(argv0), name);
}

std::string sibling_exe(const std::string& argv0, const std::string& name)
{
    std::string file = name;
#if defined(_WIN32)
    if (file.size() < 4 || file.substr(file.size() - 4) != ".exe") {
        file += ".exe";
    }
#else
    (void)argv0;
#endif
    return path_join(module_dir(argv0), file);
}

std::int64_t now_ms()
{
    return current_timestamp_ms();
}

std::string ms_text(std::int64_t ms)
{
    return format_duration_ms(ms);
}

bool to_int(const std::string& text, int* value)
{
    try {
        *value = lexical_cast<int>(text);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_duration_ms(const std::string& text, int* out_ms)
{
    const std::string t = trim(text);
    if (t.empty()) {
        return false;
    }
    std::size_t digits = 0;
    while (digits < t.size() && std::isdigit(static_cast<unsigned char>(t[digits])) != 0) {
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    int value = 0;
    if (!to_int(t.substr(0, digits), &value) || value <= 0) {
        return false;
    }
    const std::string unit = to_lower(t.substr(digits));
    int factor = 0;
    if (unit == "ms") {
        factor = 1;
    } else if (unit.empty() || unit == "s") {
        factor = 1000;
    } else if (unit == "m") {
        factor = 60 * 1000;
    } else if (unit == "h") {
        factor = 60 * 60 * 1000;
    } else if (unit == "d") {
        factor = 24 * 60 * 60 * 1000;
    } else {
        return false;
    }
    const std::int64_t total = static_cast<std::int64_t>(value) * factor;
    if (total <= 0 || total > 0x7FFFFFFF) {
        return false;
    }
    *out_ms = static_cast<int>(total);
    return true;
}

std::string jstr(const JsonValue& node, const char* key, const std::string& fallback)
{
    if (!node.is_object() || node.find(key) == node.end()) {
        return fallback;
    }
    const JsonValue& value = node[key];
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return fallback;
    }
    return value.dump();
}

std::int64_t jint(const JsonValue& node, const char* key, std::int64_t fallback)
{
    if (!node.is_object() || node.find(key) == node.end()) {
        return fallback;
    }
    const JsonValue& value = node[key];
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_float()) {
        return static_cast<std::int64_t>(value.get<double>());
    }
    if (value.is_string()) {
        int parsed = 0;
        if (to_int(value.get<std::string>(), &parsed)) {
            return parsed;
        }
    }
    return fallback;
}

bool jbool(const JsonValue& node, const char* key, bool fallback)
{
    if (!node.is_object() || node.find(key) == node.end()) {
        return fallback;
    }
    const JsonValue& value = node[key];
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_string()) {
        const std::string text = to_lower(value.get<std::string>());
        if (text == "1" || text == "true" || text == "yes") {
            return true;
        }
        if (text == "0" || text == "false" || text == "no") {
            return false;
        }
    }
    return fallback;
}

HttpReply json_ok(const JsonValue& body, int status)
{
    JsonValue root = body;
    if (!root.is_object()) {
        root = JsonValue::object();
        root["value"] = body;
    }
    root["ok"] = true;
    return HttpReply::json(status, to_json_string(root));
}

HttpReply json_ok()
{
    return json_ok(JsonValue::object());
}

HttpReply json_error(int status, const std::string& message)
{
    JsonValue root = JsonValue::object();
    root["ok"] = false;
    root["error"] = message;
    return HttpReply::json(status, to_json_string(root));
}

DemoReport::DemoReport(const std::string& title) : title_(title), passed_(0), failed_(0) {}

void DemoReport::check(bool ok, const std::string& what)
{
    if (ok) {
        ++passed_;
        std::cout << "  [通过] " << what << "\n";
    } else {
        ++failed_;
        std::cout << "  [失败] " << what << "\n";
    }
    std::cout.flush();
}

void DemoReport::info(const std::string& text) const
{
    std::cout << "  " << text << "\n";
}

int DemoReport::finish() const
{
    std::cout << "\n" << title_ << "：通过 " << passed_ << " 项，失败 " << failed_ << " 项\n";
    if (failed_ == 0) {
        std::cout << "结果：ok\n";
        return 0;
    }
    std::cout << "结果：failed\n";
    return 1;
}

void print_banner(const std::string& title, const std::string& detail)
{
    std::cout << "=== " << title << " ===\n";
    if (!detail.empty()) {
        std::cout << detail << "\n";
    }
    std::cout.flush();
}

}  // namespace app
