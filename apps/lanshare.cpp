// lanshare —— libmini 应用示例 3：局域网文件中转站
//
// 一个「把文件拖上去、别人再取下来」的小服务：分块上传 + 断点续传（偏移不符时
// 返回当前已收字节，客户端据此续传）、sha256 完整性校验、目录打包下载、按 TTL
// 过期清理，索引落在 SQLite。上传走「原始 body + 偏移查询参数」，因此不需要库
// 目前还没有的 multipart 支持；网页端的拖拽上传用 fetch 分片 PUT，同样不依赖
// multipart。
//
// 用法：
//   lanshare --demo                                    自检：分块上传/下载/校验全流程
//   lanshare --dir ./share --port 8800 --ttl 24h       正常模式
//
// 协议（都是 JSON，除下载）：
//   POST   /api/files/:name/init      {"size": N}      创建/清空，返回当前状态
//   PUT    /api/files/:name?offset=N  原始字节          写入分片；偏移不符返回 409
//   GET    /api/files/:name/status                     当前已收字节与校验值
//   POST   /api/files/:name/commit                     收尾：算 sha256 并入库
//   GET    /api/files                                  列出全部文件
//   GET    /files/:name                                下载
//   DELETE /api/files/:name                            删除
//   GET    /api/bundle.zip?prefix=                     打包下载（ZipWriter）
//   POST   /api/cleanup?max_age_ms=N                   清理过期文件
//
// 用到的 libmini 模块：http_server、http_client（demo 自测）、file_utils(sha256_file_hex)、
// zip、sqlite、machine_fingerprint、json_utils、args、console、path_utils。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_common.h"
#include "libmini.h"
#include "utils/ui_panels.h"

using namespace libmini;
using namespace app;

namespace {

// ---------------- 文件名校验 ----------------

// 只允许安全字符，拒绝任何路径成分（这是对外暴露的服务，必须挡目录穿越）
bool safe_file_name(const std::string& name, std::string* error)
{
    if (name.empty()) {
        *error = "文件名不能为空";
        return false;
    }
    if (name == "." || name == ".." || name.size() > 200) {
        *error = "文件名非法";
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
                        c == '+';
        if (!ok) {
            *error = "文件名只允许字母/数字/._-+";
            return false;
        }
    }
    return true;
}

// ---------------- 索引（SQLite） ----------------

struct FileRecord
{
    std::string name;
    std::int64_t size = 0;
    std::string sha256;
    bool complete = false;
    std::int64_t created_ms = 0;
    std::int64_t updated_ms = 0;
};

const char* kSchema =
    "CREATE TABLE IF NOT EXISTS files("
    " name TEXT PRIMARY KEY, size INTEGER NOT NULL DEFAULT 0, sha256 TEXT NOT NULL DEFAULT '',"
    " complete INTEGER NOT NULL DEFAULT 0, created_ms INTEGER NOT NULL DEFAULT 0,"
    " updated_ms INTEGER NOT NULL DEFAULT 0);";

class ShareStore
{
public:
    bool open(const std::string& path, std::string* error)
    {
        Status status = db_.try_open(path, SqliteDatabase::OpenReadWrite |
                                               SqliteDatabase::OpenCreate);
        if (!status.ok()) {
            *error = status.to_string();
            return false;
        }
        db_.set_busy_timeout_ms(5000);
        if (!db_.exec(kSchema)) {
            *error = db_.last_error("ShareStore::schema").to_string();
            return false;
        }
        return true;
    }

    bool upsert(const FileRecord& record)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_,
                           "INSERT INTO files(name, size, sha256, complete, created_ms,"
                           " updated_ms) VALUES(?, ?, ?, ?, ?, ?)"
                           " ON CONFLICT(name) DO UPDATE SET size=excluded.size,"
                           " sha256=excluded.sha256, complete=excluded.complete,"
                           " updated_ms=excluded.updated_ms");
        if (!st.is_prepared() || !st.bind_text(1, record.name) ||
            !st.bind_int64(2, record.size) || !st.bind_text(3, record.sha256) ||
            !st.bind_int(4, record.complete ? 1 : 0) ||
            !st.bind_int64(5, record.created_ms) || !st.bind_int64(6, record.updated_ms)) {
            return false;
        }
        return st.step() == SqliteStatement::StepDone;
    }

    bool touch(const std::string& name, std::int64_t size)
    {
        FileRecord record;
        if (!get(name, &record)) {
            record.name = name;
            record.created_ms = current_timestamp_ms();
        }
        record.size = size;
        record.complete = false;
        record.updated_ms = current_timestamp_ms();
        return upsert(record);
    }

    bool get(const std::string& name, FileRecord* out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_,
                           "SELECT name, size, sha256, complete, created_ms, updated_ms"
                           " FROM files WHERE name = ?");
        if (!st.is_prepared() || !st.bind_text(1, name) ||
            st.step() != SqliteStatement::StepRow) {
            return false;
        }
        out->name = st.column_text(0);
        out->size = st.column_int64(1);
        out->sha256 = st.column_text(2);
        out->complete = st.column_int(3) != 0;
        out->created_ms = st.column_int64(4);
        out->updated_ms = st.column_int64(5);
        return true;
    }

    std::vector<FileRecord> list()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<FileRecord> out;
        SqliteStatement st(db_,
                           "SELECT name, size, sha256, complete, created_ms, updated_ms"
                           " FROM files ORDER BY updated_ms DESC");
        if (!st.is_prepared()) {
            return out;
        }
        while (st.step() == SqliteStatement::StepRow) {
            FileRecord record;
            record.name = st.column_text(0);
            record.size = st.column_int64(1);
            record.sha256 = st.column_text(2);
            record.complete = st.column_int(3) != 0;
            record.created_ms = st.column_int64(4);
            record.updated_ms = st.column_int64(5);
            out.push_back(record);
        }
        return out;
    }

    bool remove(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SqliteStatement st(db_, "DELETE FROM files WHERE name = ?");
        if (!st.is_prepared() || !st.bind_text(1, name)) {
            return false;
        }
        return st.step() == SqliteStatement::StepDone;
    }

private:
    SqliteDatabase db_;
    std::mutex mutex_;
};

// ---------------- HTTP 服务 ----------------

JsonValue record_to_json(const FileRecord& record)
{
    JsonValue node = JsonValue::object();
    node["name"] = record.name;
    node["size"] = record.size;
    node["sha256"] = record.sha256;
    node["complete"] = record.complete;
    node["created_ms"] = record.created_ms;
    node["updated_ms"] = record.updated_ms;
    return node;
}

const char kUploadPage[] =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<title>lanshare</title><style>"
    "body{font:14px/1.5 ui-sans-serif,system-ui,'Segoe UI',sans-serif;background:#0e1015;"
    "color:#e7e9ef;margin:24px}h1{font-size:19px;margin:0 0 4px}.dim{color:#8b93a7}"
    "#drop{border:2px dashed #2c3547;border-radius:12px;padding:26px;text-align:center;"
    "margin:14px 0;color:#8b93a7}#drop.hot{border-color:#4aa8ff;color:#e7e9ef}"
    "table{border-collapse:collapse;width:100%;font-size:13px}"
    "th,td{border-bottom:1px solid #242938;padding:6px 8px;text-align:left}"
    "th{color:#8b93a7;font-weight:500}code{color:#7db4ff}"
    "a{color:#7db4ff}#log{margin-top:10px;font-size:12px;color:#8b93a7;white-space:pre-wrap}"
    "button{background:#232a3a;color:#e7e9ef;border:1px solid #242938;border-radius:7px;"
    "padding:5px 11px;cursor:pointer}</style></head><body>"
    "<h1>lanshare 文件中转站</h1><div class=\"dim\" id=\"meta\"></div>"
    "<div id=\"drop\">把文件拖到这里，或点击选择文件（分片上传，可续传）"
    "<input type=\"file\" id=\"picker\" multiple style=\"display:none\"></div>"
    "<div id=\"log\"></div><h3>已共享文件</h3><div id=\"files\"></div>"
    "<script>"
    "var CHUNK=1024*1024;"
    "function $(id){return document.getElementById(id);}"
    "function esc(v){return String(v==null?'':v).replace(/[&<>\"]/g,function(c){"
    "return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c];});}"
    "function fmtBytes(v){var u=['B','KiB','MiB','GiB'],i=0;while(v>=1024&&i<u.length-1){"
    "v/=1024;++i;}return v.toFixed(1)+' '+u[i];}"
    "function log(t){$('log').textContent+=t+'\\n';}"
    "async function upload(file){"
    "log('开始上传 '+file.name+' ('+fmtBytes(file.size)+')');"
    "var init=await fetch('/api/files/'+encodeURIComponent(file.name)+'/init',{method:'POST',"
    "headers:{'Content-Type':'application/json'},body:JSON.stringify({size:file.size})});"
    "var st=await(await fetch('/api/files/'+encodeURIComponent(file.name)+'/status')).json();"
    "var offset=(st.file&&st.file.received)||0;if(offset>file.size)offset=0;"
    "if(offset>0)log('检测到已上传 '+fmtBytes(offset)+'，续传');"
    "while(offset<file.size){var blob=file.slice(offset,Math.min(offset+CHUNK,file.size));"
    "var r=await fetch('/api/files/'+encodeURIComponent(file.name)+'?offset='+offset,"
    "{method:'PUT',headers:{'Content-Type':'application/octet-stream'},body:blob});"
    "var j=await r.json();if(!r.ok){log('分片失败：'+(j.error||r.status));if(j.received!=null&&"
    "j.received<=file.size){offset=j.received;continue;}return;}"
    "offset=j.received;log('已上传 '+fmtBytes(offset)+' / '+fmtBytes(file.size));}"
    "var done=await(await fetch('/api/files/'+encodeURIComponent(file.name)+'/commit',"
    "{method:'POST'})).json();"
    "log(done.file?('完成 sha256='+done.file.sha256):('提交失败：'+done.error));poll();}"
    "function pick(files){for(var i=0;i<files.length;++i){upload(files[i]);}}"
    "$('drop').addEventListener('click',function(){$('picker').click();});"
    "$('picker').addEventListener('change',function(){$('picker').value&&pick(this.files);});"
    "$('drop').addEventListener('dragover',function(e){e.preventDefault();"
    "this.classList.add('hot');});"
    "$('drop').addEventListener('dragleave',function(){this.classList.remove('hot');});"
    "$('drop').addEventListener('drop',function(e){e.preventDefault();"
    "this.classList.remove('hot');pick(e.dataTransfer.files);});"
    "function poll(){fetch('/api/files',{cache:'no-store'}).then(function(r){return r.json();})"
    ".then(function(d){$('meta').innerHTML='节点指纹 '+esc(d.fingerprint)+' · 共享目录 '+esc(d.dir)+"
    "' · 共 '+d.files.length+' 个文件  '+'<a href=\"/api/bundle.zip\">打包下载全部</a>';"
    "var h='<table><tr><th>文件名</th><th>大小</th><th>状态</th><th>sha256</th><th>操作</th>'"
    "'</tr>';d.files.forEach(function(f){h+='<tr><td><a href=\"/files/'+"
    "encodeURIComponent(f.name)+'\">'+esc(f.name)+'</a></td><td>'+fmtBytes(f.size)+"
    "'</td><td>'+(f.complete?'<span style=\"color:#3ddc84\">完成</span>':'<span style=\""
    "color:#ffb454\">上传中</span>')+'</td><td><code>'+(f.sha256||'-').slice(0,16)+"
    "'</code></td><td><button data-del=\"'+esc(f.name)+'\">删除</button></td></tr>';});"
    "$('files').innerHTML=h+'</table>';"
    "Array.prototype.forEach.call(document.querySelectorAll('button[data-del]'),function(b){"
    "b.onclick=function(){del(this.getAttribute('data-del'));};});"
    "}).catch(function(e){$('meta').textContent=e;});}"
    "function del(name){fetch('/api/files/'+encodeURIComponent(name),{method:'DELETE'})"
    ".then(function(){poll();});}"
    "poll();setInterval(poll,2000);</script></body></html>";

class FileApi
{
public:
    FileApi(ShareStore* store, const std::string& dir, std::int64_t ttl_ms)
        : store_(store), dir_(dir), ttl_ms_(ttl_ms)
    {
    }

    void register_routes(HttpServer* server)
    {
        server->get("/", [this](const HttpRequest&) { return index(); });
        server->get("/api/files", [this](const HttpRequest&) { return list(); });
        server->get("/api/files/:name/status", [this](const HttpRequest& req) {
            return status(req);
        });
        server->post("/api/files/:name/init", [this](const HttpRequest& req) {
            return init(req);
        });
        server->put("/api/files/:name", [this](const HttpRequest& req) { return write(req); });
        server->post("/api/files/:name/commit", [this](const HttpRequest& req) {
            return commit(req);
        });
        server->del("/api/files/:name", [this](const HttpRequest& req) { return remove(req); });
        server->get("/files/:name", [this](const HttpRequest& req) { return download(req); });
        server->get("/api/bundle.zip", [this](const HttpRequest& req) { return bundle(req); });
        server->post("/api/cleanup", [this](const HttpRequest& req) { return cleanup(req); });
    }

private:
    std::string path_of(const std::string& name) const { return path_join(dir_, name); }

    HttpReply index()
    {
        HttpReply reply = HttpReply::text(200, kUploadPage);
        reply.headers["Content-Type"] = "text/html; charset=utf-8";
        return reply;
    }

    HttpReply list()
    {
        const std::vector<FileRecord> records = store_->list();
        JsonValue array = JsonValue::array();
        for (std::size_t i = 0; i < records.size(); ++i) {
            array.push_back(record_to_json(records[i]));
        }
        JsonValue root = JsonValue::object();
        root["files"] = array;
        root["dir"] = dir_;
        root["fingerprint"] = machine_fingerprint().id;
        root["ttl_ms"] = ttl_ms_;
        return json_ok(root);
    }

    HttpReply status(const HttpRequest& req)
    {
        const std::string name = req.param("name");
        std::string error;
        if (!safe_file_name(name, &error)) {
            return json_error(400, error);
        }
        FileRecord record;
        const bool indexed = store_->get(name, &record);
        const std::size_t on_disk = file_size(path_of(name));
        JsonValue node = JsonValue::object();
        node["name"] = name;
        node["received"] = static_cast<std::int64_t>(on_disk);
        if (indexed) {
            node["size"] = record.size;
            node["complete"] = record.complete;
            node["sha256"] = record.sha256;
        } else {
            node["size"] = 0;
            node["complete"] = false;
            node["sha256"] = "";
        }
        JsonValue root = JsonValue::object();
        root["file"] = node;
        return json_ok(root);
    }

    HttpReply init(const HttpRequest& req)
    {
        const std::string name = req.param("name");
        std::string error;
        if (!safe_file_name(name, &error)) {
            return json_error(400, error);
        }
        JsonValue body;
        try {
            body = parse_json(req.body.empty() ? std::string("{}") : req.body);
        } catch (const std::exception& e) {
            return json_error(400, std::string("请求体不是合法 JSON: ") + e.what());
        }
        const std::int64_t declared = jint(body, "size", 0);
        // 已经存在且大小一致时保留内容，让客户端自然续传；否则清空重来
        const std::size_t existing = file_size(path_of(name));
        if (declared > 0 && static_cast<std::size_t>(declared) == existing) {
            // 保留
        } else if (!write_file(path_of(name), std::string())) {
            return json_error(500, "无法创建文件");
        }
        FileRecord record;
        if (!store_->get(name, &record)) {
            record.name = name;
            record.created_ms = current_timestamp_ms();
        }
        record.size = declared;
        record.complete = false;
        record.sha256.clear();
        record.updated_ms = current_timestamp_ms();
        store_->upsert(record);

        JsonValue node = JsonValue::object();
        node["name"] = name;
        node["declared_size"] = declared;
        node["received"] = static_cast<std::int64_t>(file_size(path_of(name)));
        JsonValue root = JsonValue::object();
        root["file"] = node;
        return json_ok(root, 201);
    }

    HttpReply write(const HttpRequest& req)
    {
        const std::string name = req.param("name");
        std::string error;
        if (!safe_file_name(name, &error)) {
            return json_error(400, error);
        }
        std::int64_t offset = 0;
        const std::map<std::string, std::string>::const_iterator it = req.query.find("offset");
        if (it != req.query.end()) {
            int parsed = 0;
            if (!to_int(it->second, &parsed) || parsed < 0) {
                return json_error(400, "offset 非法");
            }
            offset = parsed;
        }
        const std::size_t current = file_size(path_of(name));
        if (static_cast<std::size_t>(offset) != current) {
            // 断点续传：告诉客户端当前已收到的字节数
            JsonValue body = JsonValue::object();
            body["ok"] = false;
            body["error"] = "偏移不符";
            body["received"] = static_cast<std::int64_t>(current);
            body["expected_offset"] = static_cast<std::int64_t>(current);
            return HttpReply::json(409, to_json_string(body));
        }
        const bool written = current == 0 ? write_file(path_of(name), req.body)
                                          : append_file(path_of(name), req.body);
        if (!written) {
            return json_error(500, "写入失败");
        }
        const std::size_t after = file_size(path_of(name));
        store_->touch(name, static_cast<std::int64_t>(after));

        JsonValue node = JsonValue::object();
        node["name"] = name;
        node["received"] = static_cast<std::int64_t>(after);
        node["chunk_bytes"] = static_cast<std::int64_t>(req.body.size());
        JsonValue root = JsonValue::object();
        root["file"] = node;
        return json_ok(root);
    }

    HttpReply commit(const HttpRequest& req)
    {
        const std::string name = req.param("name");
        std::string error;
        if (!safe_file_name(name, &error)) {
            return json_error(400, error);
        }
        const std::string path = path_of(name);
        if (!file_exists(path)) {
            return json_error(404, "文件不存在（先 init 并写入分片）");
        }
        FileRecord record;
        if (!store_->get(name, &record)) {
            record.name = name;
            record.created_ms = current_timestamp_ms();
        }
        record.size = static_cast<std::int64_t>(file_size(path));
        record.sha256 = sha256_file_hex(path);
        record.complete = true;
        record.updated_ms = current_timestamp_ms();
        if (!store_->upsert(record)) {
            return json_error(500, "索引写入失败");
        }
        JsonValue root = JsonValue::object();
        root["file"] = record_to_json(record);
        return json_ok(root);
    }

    HttpReply remove(const HttpRequest& req)
    {
        const std::string name = req.param("name");
        std::string error;
        if (!safe_file_name(name, &error)) {
            return json_error(400, error);
        }
        const bool removed_file = remove_file(path_of(name));
        const bool removed_row = store_->remove(name);
        if (!removed_file && !removed_row) {
            return json_error(404, "文件不存在");
        }
        JsonValue root = JsonValue::object();
        root["removed"] = name;
        return json_ok(root);
    }

    HttpReply download(const HttpRequest& req)
    {
        const std::string name = req.param("name");
        std::string error;
        if (!safe_file_name(name, &error)) {
            return json_error(400, error);
        }
        const std::string path = path_of(name);
        if (!file_exists(path)) {
            return json_error(404, "文件不存在");
        }
        HttpReply reply = HttpReply::text(200, read_file(path));
        reply.headers["Content-Type"] = "application/octet-stream";
        reply.headers["Content-Disposition"] = "attachment; filename=\"" + name + "\"";
        return reply;
    }

    HttpReply bundle(const HttpRequest& req)
    {
        std::string prefix;
        const std::map<std::string, std::string>::const_iterator it = req.query.find("prefix");
        if (it != req.query.end()) {
            prefix = it->second;
        }
        const std::vector<FileRecord> records = store_->list();
        ZipWriter writer;
        int added = 0;
        for (std::size_t i = 0; i < records.size(); ++i) {
            const FileRecord& record = records[i];
            if (!record.complete) {
                continue;  // 只打包已完成校验的文件
            }
            if (!prefix.empty() && record.name.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            const std::string content = read_file(path_of(record.name));
            if (writer.add_file(record.name, content, ZipMethod::Deflate)) {
                ++added;
            }
        }
        const std::string zip = writer.finish();
        if (added == 0) {
            return json_error(404, "没有可打包的文件");
        }
        HttpReply reply = HttpReply::text(200, zip);
        reply.headers["Content-Type"] = "application/zip";
        reply.headers["Content-Disposition"] = "attachment; filename=\"lanshare-bundle.zip\"";
        return reply;
    }

    HttpReply cleanup(const HttpRequest& req)
    {
        std::int64_t max_age_ms = ttl_ms_;
        const std::map<std::string, std::string>::const_iterator it = req.query.find("max_age_ms");
        if (it != req.query.end()) {
            int parsed = 0;
            if (!to_int(it->second, &parsed)) {
                return json_error(400, "max_age_ms 非法");
            }
            max_age_ms = parsed;
        }
        const std::int64_t now = current_timestamp_ms();
        const std::vector<FileRecord> records = store_->list();
        JsonValue removed = JsonValue::array();
        for (std::size_t i = 0; i < records.size(); ++i) {
            const std::int64_t age = now - records[i].updated_ms;
            if (age >= max_age_ms) {
                remove_file(path_of(records[i].name));
                store_->remove(records[i].name);
                removed.push_back(records[i].name);
            }
        }
        JsonValue root = JsonValue::object();
        root["removed"] = removed;
        root["removed_count"] = static_cast<std::int64_t>(removed.size());
        root["max_age_ms"] = max_age_ms;
        return json_ok(root);
    }

    ShareStore* store_;
    std::string dir_;
    std::int64_t ttl_ms_;
};

// ---------------- demo ----------------

int run_demo()
{
    DemoReport report("lanshare demo");

    // 1) 文件名白名单
    std::string error;
    report.check(safe_file_name("report.pdf", &error), "接受普通文件名");
    report.check(!safe_file_name("../escape.txt", &error), "拒绝目录穿越（../escape.txt）");
    report.check(!safe_file_name("a/b.txt", &error), "拒绝路径分隔符");

    // 2) 起服务
    const std::string workspace = make_workspace("lanshare_demo");
    report.check(!workspace.empty(), "创建临时工作目录");
    if (workspace.empty()) {
        return report.finish();
    }
    const std::string share_dir = workspace_file(workspace, "share");
    make_directories(share_dir);

    ShareStore store;
    if (!store.open(workspace_file(workspace, "index.db"), &error)) {
        report.check(false, "打开 SQLite: " + error);
        remove_tree(workspace);
        return report.finish();
    }
    report.check(true, "打开索引库并建表");

    FileApi api(&store, share_dir, 24 * 60 * 60 * 1000);
    HttpServer server;
    server.enable_health_endpoints();
    api.register_routes(&server);
    const bool listening = server.start_background(0) && server.wait_until_ready(5000);
    report.check(listening, "服务监听成功（自动分配端口）");
    if (!listening) {
        remove_tree(workspace);
        return report.finish();
    }
    const std::string base = "http://127.0.0.1:" + lexical_cast<std::string>(server.port());
    report.info("服务地址: " + base + "/");

    // 3) 造一个 3 MiB 的本地文件
    const std::string payload = random_string(3 * 1024 * 1024);
    const std::string local_path = workspace_file(workspace, "payload.bin");
    report.check(write_file(local_path, payload), "生成 3 MiB 本地测试文件");
    const std::string local_sha = sha256_file_hex(local_path);

    HttpClient http(base);
    const std::string name = "payload.bin";

    // 4) init + 分片上传
    Result<HttpResponse> init = http.try_post_json("/api/files/" + name + "/init",
                                                  "{\"size\":3145728}");
    report.check(init.ok() && init.value().status == 201, "POST /init 创建上传会话");

    const std::size_t chunk = 1024 * 1024;
    bool chunk_ok = true;
    for (std::size_t offset = 0; offset < payload.size(); offset += chunk) {
        const std::string part = payload.substr(offset, chunk);
        const std::string path = "/api/files/" + name + "?offset=" +
                                 lexical_cast<std::string>(static_cast<std::int64_t>(offset));
        Result<HttpResponse> written = http.try_request("PUT", path, part,
                                                        "application/octet-stream");
        if (!written.ok() || written.value().status != 200) {
            chunk_ok = false;
        }
    }
    report.check(chunk_ok, "分 3 片 PUT 上传成功");

    // 5) 断点续传：故意用错误偏移，服务端应回 409 + 已收字节
    Result<HttpResponse> conflict = http.try_request(
        "PUT", "/api/files/" + name + "?offset=100", "x", "application/octet-stream");
    bool conflict_ok = false;
    if (conflict.ok() && conflict.value().status == 409) {
        try {
            const JsonValue doc = parse_json(conflict.value().body);
            conflict_ok = doc["received"].is_number_integer() &&
                          doc["received"].get<std::int64_t>() == 3145728;
        } catch (const std::exception&) {
        }
    }
    report.check(conflict_ok, "偏移不符返回 409 并告知已收字节（断点续传依据）");

    Result<HttpResponse> status = http.try_get("/api/files/" + name + "/status");
    bool status_ok = false;
    if (status.ok()) {
        try {
            status_ok = parse_json(status.value().body)["file"]["received"].get<std::int64_t>() ==
                        3145728;
        } catch (const std::exception&) {
        }
    }
    report.check(status_ok, "GET /status 报告已收 3145728 字节");

    // 6) commit → sha256 与本地一致
    Result<HttpResponse> commit = http.try_post_json("/api/files/" + name + "/commit", "{}");
    std::string server_sha;
    if (commit.ok()) {
        try {
            server_sha = parse_json(commit.value().body)["file"]["sha256"].get<std::string>();
        } catch (const std::exception&) {
        }
    }
    report.check(!server_sha.empty() && server_sha == local_sha,
                 "commit 计算的 sha256 与本地一致");

    // 7) 下载回来再算一次 sha256
    Result<HttpResponse> download = http.try_get("/files/" + name);
    const std::string downloaded_path = workspace_file(workspace, "downloaded.bin");
    bool download_ok = false;
    if (download.ok() && download.value().status == 200) {
        download_ok = write_file(downloaded_path, download.value().body) &&
                      sha256_file_hex(downloaded_path) == local_sha;
    }
    report.check(download_ok, "GET /files/:name 下载内容 sha256 一致");

    // 8) 列表
    Result<HttpResponse> list = http.try_get("/api/files");
    bool list_ok = false;
    if (list.ok()) {
        try {
            const JsonValue doc = parse_json(list.value().body);
            list_ok = doc["files"].is_array() && doc["files"].size() == 1 &&
                      doc["files"][0]["complete"].get<bool>();
        } catch (const std::exception&) {
        }
    }
    report.check(list_ok, "GET /api/files 列出已完成文件");

    // 9) 打包下载
    Result<HttpResponse> bundle = http.try_get("/api/bundle.zip");
    bool bundle_ok = false;
    if (bundle.ok() && bundle.value().status == 200) {
        ZipReader reader;
        if (reader.open(bundle.value().body)) {
            bundle_ok = reader.contains(name) && reader.extract(name).size() == payload.size();
        }
    }
    report.check(bundle_ok, "GET /api/bundle.zip 打包内容与原文件等长");

    // 10) 清理：年龄阈值很大 → 不删；阈值 1ms → 删
    Result<HttpResponse> keep = http.try_request("POST", "/api/cleanup?max_age_ms=3600000", "", "");
    bool keep_ok = false;
    if (keep.ok()) {
        try {
            keep_ok = parse_json(keep.value().body)["removed_count"].get<std::int64_t>() == 0;
        } catch (const std::exception&) {
        }
    }
    report.check(keep_ok, "清理阈值未到时不删除文件");

    Result<HttpResponse> purge = http.try_request("POST", "/api/cleanup?max_age_ms=1", "", "");
    bool purge_ok = false;
    if (purge.ok()) {
        try {
            purge_ok = parse_json(purge.value().body)["removed_count"].get<std::int64_t>() == 1;
        } catch (const std::exception&) {
        }
    }
    report.check(purge_ok && !file_exists(path_join(share_dir, name)),
                 "清理阈值到点后删除文件与索引");

    Result<HttpResponse> ready = http.try_get("/readyz");
    report.check(ready.ok() && ready.value().status == 200, "GET /readyz 就绪探针通过");

    server.stop();
    remove_tree(workspace);
    return report.finish();
}

// ---------------- 原生窗口模式 ----------------

// `--ui`：把本应用的服务拉起来（子进程，与启动器同一套托管机制），再开原生
// 窗口盯文件列表/指标，并提供「清理过期文件」动作。
int run_ui(const std::string& argv0, const std::string& dir, const int port,
           const std::string& ttl, bool selftest)
{
    ui::AppWindowSpec spec;
    spec.title = "lanshare 文件中转站";
    spec.subtitle = "文件索引 / 校验状态 / 清理";
    spec.port = port;
    spec.child_args.push_back(app::self_exe(argv0));
    spec.child_args.push_back("--dir");
    spec.child_args.push_back(dir);
    spec.child_args.push_back("--port");
    spec.child_args.push_back(lexical_cast<std::string>(port));
    if (!trim(ttl).empty()) {
        spec.child_args.push_back("--ttl");
        spec.child_args.push_back(ttl);
    }

    spec.panel.json_views.push_back("/api/files");
    spec.panel.metric_filters.push_back("lanshare");

    ui::PanelAction cleanup;
    cleanup.label = "清理过期文件";
    cleanup.method = "POST";
    cleanup.path = "/api/cleanup";
    cleanup.body = "{}";
    spec.panel.actions.push_back(cleanup);

    if (selftest) {
        spec.frames = 40;
        spec.report = true;
        spec.shot_path = app::workspace_file(app::make_workspace("lanshare_ui"), "window.bmp");
    }

    std::string error;
    const int code = ui::run_app_window(spec, &error);
    if (code == 2) {
        std::cerr << "无法打开原生窗口（" << error << "），请改用 `lanshare --dir ...`。\n";
    }
    return code;
}

// ---------------- 正常模式 ----------------

int run_service(Args& args)
{
    const std::string dir = args.get_string("dir");
    if (!make_directories(dir)) {
        std::cerr << "无法创建共享目录: " << dir << "\n";
        return 1;
    }
    std::string error;
    ShareStore store;
    if (!store.open(workspace_file(dir, "index.db"), &error)) {
        std::cerr << "打开索引库失败: " << error << "\n";
        return 1;
    }

    int ttl_ms = 24 * 60 * 60 * 1000;
    if (!parse_duration_ms(args.get_string("ttl"), &ttl_ms)) {
        std::cerr << "TTL 非法（示例 24h / 30m）\n";
        return 1;
    }

    LogFacade::init();
    FileApi api(&store, dir, ttl_ms);
    HttpServer server;
    server.enable_health_endpoints();
    api.register_routes(&server);
    if (!server.start_background(args.get_int("port")) || !server.wait_until_ready(5000)) {
        std::cerr << "监听失败: " << server.last_error() << "\n";
        return 1;
    }

    print_banner("lanshare", "共享目录: " + dir + "  TTL: " + ms_text(ttl_ms));
    std::cout << "上传页面: http://127.0.0.1:" << server.port() << "/\n"
              << "节点指纹: " << machine_fingerprint().id << "\n"
              << "按 Ctrl+C 优雅退出\n"
              << std::flush;

    ConsoleExit exit_signal;
    exit_signal.wait();
    std::cout << "\n正在停止…\n";
    server.stop();
    LogFacade::shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args("lanshare", "1.0",
              "libmini 应用：局域网文件中转站（分块续传 + sha256 校验 + 打包下载 + 过期清理）");
    args.add_flag("demo", "", "自检模式：完整跑一遍上传/下载/校验");
    args.add_option("dir", "", "共享目录", std::string("lanshare-data"));
    args.add_int("port", "p", "服务端口（0 = 自动分配）", 8800);
    args.add_option("ttl", "", "文件过期时长（如 24h / 30m）", std::string("24h"));
    args.add_flag("ui", "", "打开原生窗口（文件面板，Windows）");
    args.add_flag("ui-selftest", "", "窗口自检：限帧渲染 + 截图 + 结论（CI 用）");
    if (!args.parse(argc, argv)) {
        return args.help_requested() ? 0 : 2;
    }
    if (args.has_flag("demo")) {
        return run_demo();
    }
    if (args.has_flag("ui") || args.has_flag("ui-selftest")) {
        const int port = args.get_int("port") > 0 ? args.get_int("port") : 8800;
        const std::string argv0 = (argc > 0 && argv[0] != 0) ? std::string(argv[0])
                                                             : std::string("lanshare");
        return run_ui(argv0, args.get_string("dir"), port, args.get_string("ttl"),
                      args.has_flag("ui-selftest"));
    }
    return run_service(args);
}
