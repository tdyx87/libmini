// 浏览器仪表板实现：用库本体的 HttpServer 把 RunState 暴露成网页 + JSON API。
#include "tr_dashboard.h"

#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libmini.h"
#include "tr_model.h"
#include "utils/process.h"

using namespace libmini;

namespace tr {
namespace {

// 单页仪表板（内联，无外部资源，离线可用）
const char kIndexHtml[] =
    R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>libmini 测试仪表板</title>
<style>
* { box-sizing: border-box; }
:root { color-scheme: dark; --bg:#0e1015; --panel:#161923; --line:#242938; --fg:#e7e9ef;
        --dim:#8b93a7; --ok:#3ddc84; --bad:#ff5d5d; --warn:#ffb454; --run:#4aa8ff; }
body { margin:0; padding:24px; background:var(--bg); color:var(--fg);
       font:14px/1.55 ui-sans-serif,system-ui,"Segoe UI",Roboto,"Microsoft YaHei",sans-serif; }
h1 { font-size:20px; margin:0 0 4px; font-weight:650; }
.dim { color:var(--dim); }
.row { display:flex; align-items:center; gap:12px; flex-wrap:wrap; }
header { margin-bottom:16px; border-bottom:1px solid var(--line); padding-bottom:14px; }
.panel { background:var(--panel); border:1px solid var(--line); border-radius:10px;
         padding:14px 16px; margin-bottom:16px; }
button { background:#232a3a; color:var(--fg); border:1px solid var(--line); border-radius:8px;
         padding:7px 14px; font:inherit; cursor:pointer; }
button:hover:not(:disabled) { background:#2c3547; }
button:disabled { opacity:.45; cursor:not-allowed; }
button.primary { background:#1d4ed8; border-color:#2a5cf0; }
button.danger { background:#7f1d1d; border-color:#a12a2a; }
input[type=text], input[type=number] { background:#0d1017; color:var(--fg);
         border:1px solid var(--line); border-radius:7px; padding:6px 9px; font:inherit; }
label { display:inline-flex; align-items:center; gap:6px; }
.chip { display:inline-flex; align-items:center; gap:6px; background:#0d1017;
        border:1px solid var(--line); border-radius:999px; padding:4px 12px; cursor:pointer; }
.chip input { accent-color:#4aa8ff; }
.bar { height:10px; background:#0d1017; border:1px solid var(--line); border-radius:999px; overflow:hidden; }
.bar > span { display:block; height:100%; width:0; transition:width .3s;
              background:linear-gradient(90deg,#1d4ed8,#3ddc84); }
.kpi { display:flex; gap:26px; flex-wrap:wrap; }
.kpi b { font-size:18px; font-weight:650; display:block; }
.kpi span { font-size:12px; }
.badge { font-size:12px; padding:2px 9px; border-radius:999px; white-space:nowrap; }
.b-ok { background:rgba(61,220,132,.14); color:var(--ok); }
.b-bad { background:rgba(255,93,93,.14); color:var(--bad); }
.b-run { background:rgba(74,168,255,.16); color:var(--run); }
.b-crash { background:rgba(255,180,84,.16); color:var(--warn); }
.b-idle { background:rgba(139,147,167,.14); color:var(--dim); }
.suite { border:1px solid var(--line); border-radius:10px; margin-bottom:10px;
         overflow:hidden; background:var(--panel); }
.suite-head { display:flex; align-items:center; gap:12px; padding:11px 14px; cursor:pointer; }
.suite-head:hover { background:#1b2030; }
.suite-head .name { font-weight:600; min-width:140px; }
.meta { color:var(--dim); font-size:13px; }
.suite-body { border-top:1px solid var(--line); padding:10px 14px; background:#11141c; }
.case { display:flex; gap:10px; padding:2px 0;
        font-family:ui-monospace,Consolas,"Courier New",monospace; font-size:12.5px; }
.case .t { flex:1; word-break:break-all; }
.case.failed .t { color:var(--bad); }
pre.fail { background:#1a1013; border:1px solid #3a1f24; border-radius:8px; padding:9px 11px;
           overflow:auto; max-height:280px; white-space:pre-wrap; margin:6px 0 12px;
           font-size:12px; color:#ffd9d9; }
.empty { color:var(--dim); font-size:13px; }
</style>
</head>
<body>
<header>
  <div class="row" style="justify-content:space-between">
    <div>
      <h1>libmini 测试仪表板</h1>
      <div class="dim" id="meta">正在连接…</div>
    </div>
    <div class="row">
      <button id="btn-run" class="primary">运行选中</button>
      <button id="btn-rerun">重跑失败</button>
      <button id="btn-stop" class="danger" disabled>停止</button>
    </div>
  </div>
</header>

<section class="panel">
  <div class="row" id="suite-picker"><span class="dim">套件：</span></div>
  <div class="row" style="margin-top:10px">
    <label>过滤 <input type="text" id="filter" size="24" placeholder="如 ResultTest.* 或 SqliteTest.*" /></label>
    <label>并发 <input type="number" id="jobs" min="1" max="8" value="1" style="width:64px" /></label>
    <label class="chip"><input type="checkbox" id="only-failed" /> 只看失败用例</label>
  </div>
</section>

<section class="panel">
  <div class="row" style="justify-content:space-between">
    <div class="dim" id="progress-label">尚未运行</div>
    <div class="dim" id="elapsed"></div>
  </div>
  <div class="bar" style="margin:9px 0 13px"><span id="progress"></span></div>
  <div class="kpi" id="kpi"></div>
</section>

<section id="list"></section>

<script>
"use strict";
var $ = function (id) { return document.getElementById(id); };
var st = null;
var built = false;
var expanded = {};

function esc(v) {
  return String(v === null || v === undefined ? "" : v).replace(/[&<>"]/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
  });
}
function secs(v) { return (Math.round((v || 0) * 100) / 100).toFixed(2) + "s"; }
function liveSecs(s) {
  return (s.state === "running" && s.started_at_ms) ? (Date.now() - s.started_at_ms) / 1000
                                                    : (s.seconds || 0);
}
var ST = {
  pending: ["待运行", "b-idle"], running: ["运行中", "b-run"], passed: ["通过", "b-ok"],
  failed: ["失败", "b-bad"], crashed: ["崩溃", "b-crash"], not_run: ["未运行", "b-idle"]
};
function badge(s) {
  var m = ST[s.state] || [s.state, "b-idle"];
  return '<span class="badge ' + m[1] + '">' + m[0] + "</span>";
}

function buildPicker(suites) {
  $("suite-picker").innerHTML = '<span class="dim">套件：</span>' + suites.map(function (s) {
    return '<label class="chip"><input type="checkbox" class="suite-toggle" value="' +
           esc(s.id) + '" checked /> ' + esc(s.id) + "</label>";
  }).join("");
  built = true;
}

function caseRow(c) {
  var mark = c.status === "failed" ? "\u2717" : (c.status === "skipped" ? "\u25cb" : "\u2713");
  return '<div class="case ' + (c.status === "failed" ? "failed" : "") + '"><span class="t">' +
         mark + " " + esc(c.full_name) + '</span><span class="meta">' + secs(c.seconds) +
         "</span></div>";
}

function suiteHtml(s) {
  var open = !!expanded[s.id];
  var cases = s.cases || [];
  var only = $("only-failed").checked;
  var shown = only ? cases.filter(function (c) { return c.status === "failed"; }) : cases;
  var counts = "";
  if (s.cases_total) {
    counts = s.cases_passed + " / " + s.cases_total + " 通过";
  }
  if (s.cases_failed) { counts += ' · <b style="color:var(--bad)">' + s.cases_failed + " 失败</b>"; }
  if (s.cases_skipped) { counts += " · " + s.cases_skipped + " 跳过"; }
  var live = s.state === "running" ? "运行中 " + secs(liveSecs(s)) : "";
  var dur = (s.state === "pending" || s.state === "not_run") ? "" : "用时 " + secs(liveSecs(s));

  var html = '<div class="suite-head" data-id="' + esc(s.id) + '">' + badge(s) +
             '<span class="name">' + esc(s.label || s.id) + "</span>" +
             '<span class="meta">' + esc(live || s.note || "") + "</span><span style=\"flex:1\"></span>" +
             '<span class="meta">' + counts + "</span>" +
             '<span class="meta">' + esc(dur) + "</span>" +
             '<span class="meta">' + (open ? "\u25be" : "\u25b8") + "</span></div>";
  if (!open) { return '<div class="suite">' + html + "</div>"; }

  var body = "";
  if (s.output_tail) { body += '<pre class="fail">' + esc(s.output_tail) + "</pre>"; }
  var failed = shown.filter(function (c) { return c.status === "failed"; });
  failed.forEach(function (c) {
    body += caseRow(c);
    (c.failures || []).forEach(function (f) { body += '<pre class="fail">' + esc(f) + "</pre>"; });
  });
  var rest = shown.filter(function (c) { return c.status !== "failed"; });
  var limit = 500;
  if (rest.length) {
    body += '<div class="empty" style="margin:8px 0 4px">其余用例（' + rest.length + "）</div>";
    body += rest.slice(0, limit).map(caseRow).join("");
    if (rest.length > limit) {
      body += '<div class="empty">… 还有 ' + (rest.length - limit) + " 条未显示</div>";
    }
  }
  if (!failed.length && !rest.length) { body += '<div class="empty">没有匹配的用例</div>'; }
  return '<div class="suite">' + html + '<div class="suite-body">' + body + "</div></div>";
}

function render() {
  if (!st) { return; }
  if (!built) { buildPicker(st.suites); }
  var sum = st.summary;
  var flag = st.running ? ' · <span style="color:var(--run)">运行中</span>'
                        : (st.stop_requested ? ' · <span style="color:var(--warn)">已请求停止</span>' : "");
  $("meta").innerHTML = "起始 " + esc(st.started_at || "—") + " · " + esc(st.options || "") + flag;
  $("elapsed").textContent = "用时 " + secs(st.elapsed_seconds);
  var pct = sum.suites_total ? (sum.suites_finished / sum.suites_total * 100) : 0;
  $("progress").style.width = pct.toFixed(1) + "%";
  $("progress-label").textContent =
      "套件 " + sum.suites_finished + "/" + sum.suites_total + " · 用例 " +
      sum.cases_passed + " 通过 / " + sum.cases_failed + " 失败 / " + sum.cases_skipped + " 跳过";
  var kpis = [["套件通过", sum.suites_passed, "var(--ok)"],
              ["套件失败", sum.suites_failed, "var(--bad)"],
              ["套件崩溃", sum.suites_crashed, "var(--warn)"],
              ["用例通过", sum.cases_passed, "var(--ok)"],
              ["用例失败", sum.cases_failed, "var(--bad)"],
              ["用例跳过", sum.cases_skipped, "var(--dim)"]];
  $("kpi").innerHTML = kpis.map(function (k) {
    return "<div><b style=\"color:" + k[2] + '">' + k[1] + '</b><span class="dim">' +
           k[0] + "</span></div>";
  }).join("");
  $("btn-run").disabled = st.running;
  $("btn-rerun").disabled = st.running;
  $("btn-stop").disabled = !st.running;
  var selected = st.suites.filter(function (s) { return s.selected; });
  $("list").innerHTML = selected.length ? selected.map(suiteHtml).join("")
                                        : '<div class="panel empty">本次没有选中的套件</div>';
}

function poll() {
  fetch("/api/state", { cache: "no-store" }).then(function (r) { return r.json(); })
    .then(function (j) { st = j; render(); })
    .catch(function (e) { $("meta").textContent = "与测试进程失联：" + e; });
}

function pickedSuites() {
  var out = [];
  Array.prototype.forEach.call(document.querySelectorAll(".suite-toggle"), function (b) {
    if (b.checked) { out.push(b.value); }
  });
  return out;
}

function startRun(suites) {
  var body = {
    suites: suites,
    filter: $("filter").value.trim(),
    jobs: parseInt($("jobs").value || "1", 10)
  };
  fetch("/api/run", { method: "POST", headers: { "Content-Type": "application/json" },
                      body: JSON.stringify(body) })
    .then(function (r) { return r.json(); })
    .then(function (j) { if (!j.ok) { window.alert("启动失败：" + (j.error || "未知原因")); } poll(); });
}

$("btn-run").addEventListener("click", function () { startRun(pickedSuites()); });
$("btn-rerun").addEventListener("click", function () {
  var bad = (st ? st.suites : []).filter(function (s) {
    return s.selected && (s.state === "failed" || s.state === "crashed");
  }).map(function (s) { return s.id; });
  if (!bad.length) { window.alert("没有失败或崩溃的套件"); return; }
  startRun(bad);
});
$("btn-stop").addEventListener("click", function () {
  fetch("/api/stop", { method: "POST" }).then(function () { poll(); });
});
$("list").addEventListener("click", function (ev) {
  var node = ev.target;
  while (node && node !== document.body && !node.classList.contains("suite-head")) {
    node = node.parentNode;
  }
  if (!node || !node.classList || !node.classList.contains("suite-head")) { return; }
  var id = node.getAttribute("data-id");
  expanded[id] = !expanded[id];
  render();
});
$("only-failed").addEventListener("change", render);
poll();
window.setInterval(poll, 500);
</script>
</body>
</html>
)HTML";

std::unique_ptr<HttpServer> g_server;
RunState* g_state = nullptr;

std::string ok_body(bool ok, const std::string& error)
{
    JsonValue node = JsonValue::object();
    node["ok"] = ok;
    if (!error.empty()) {
        node["error"] = error;
    }
    return to_json_string(node);
}

HttpReply handle_index(const HttpRequest&)
{
    HttpReply reply = HttpReply::text(200, kIndexHtml);
    reply.headers["Content-Type"] = "text/html; charset=utf-8";
    return reply;
}

HttpReply handle_state(const HttpRequest&)
{
    if (g_state == nullptr) {
        return HttpReply::error(503, "未绑定运行状态");
    }
    return HttpReply::json(200, snapshot_to_json(g_state->snapshot()));
}

HttpReply handle_suites(const HttpRequest&)
{
    JsonValue list = JsonValue::array();
    if (g_state != nullptr) {
        const std::vector<SuiteSpec>& specs = g_state->specs();
        for (std::size_t i = 0; i < specs.size(); ++i) {
            JsonValue node = JsonValue::object();
            node["id"] = specs[i].id;
            node["label"] = specs[i].label;
            node["exe"] = specs[i].exe;
            node["exists"] = file_exists(specs[i].exe);
            list.push_back(node);
        }
    }
    JsonValue root = JsonValue::object();
    root["suites"] = list;
    return HttpReply::json(200, to_json_string(root));
}

HttpReply handle_run(const HttpRequest& req)
{
    if (g_state == nullptr) {
        return HttpReply::json(503, ok_body(false, "未绑定运行状态"));
    }
    if (g_state->running()) {
        return HttpReply::json(409, ok_body(false, "已有一轮测试在运行"));
    }

    RunOptions opts;
    // 1) query 参数（便于 curl）
    std::map<std::string, std::string>::const_iterator it = req.query.find("filter");
    if (it != req.query.end()) {
        opts.gtest_filter = it->second;
    }
    it = req.query.find("jobs");
    if (it != req.query.end()) {
        opts.jobs = lexical_cast_or<int>(it->second, 1);
    }
    it = req.query.find("timeout_ms");
    if (it != req.query.end()) {
        opts.timeout_ms = lexical_cast_or<int>(it->second, opts.timeout_ms);
    }
    it = req.query.find("suites");
    if (it != req.query.end()) {
        std::vector<std::string> parts = split(it->second, ',');
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const std::string id = trim(parts[i]);
            if (!id.empty()) {
                opts.suites.push_back(id);
            }
        }
    }
    // 2) JSON 请求体（网页端用）
    if (!req.body.empty()) {
        JsonValue body;
        try {
            body = parse_json(req.body);
        } catch (const std::exception& e) {
            return HttpReply::json(
                400, ok_body(false, std::string("请求体不是合法 JSON: ") + e.what()));
        }
        if (body.is_object()) {
            if (body.find("filter") != body.end() && body["filter"].is_string()) {
                opts.gtest_filter = body["filter"].get<std::string>();
            }
            if (body.find("jobs") != body.end() && body["jobs"].is_number_integer()) {
                opts.jobs = body["jobs"].get<int>();
            }
            if (body.find("timeout_ms") != body.end() && body["timeout_ms"].is_number_integer()) {
                opts.timeout_ms = body["timeout_ms"].get<int>();
            }
            if (body.find("suites") != body.end() && body["suites"].is_array()) {
                opts.suites.clear();
                const JsonValue& arr = body["suites"];
                for (JsonValue::const_iterator sit = arr.begin(); sit != arr.end(); ++sit) {
                    if (sit->is_string()) {
                        const std::string id = trim(sit->get<std::string>());
                        if (!id.empty()) {
                            opts.suites.push_back(id);
                        }
                    }
                }
            }
        }
    }

    std::string error;
    if (!g_state->start(opts, &error)) {
        return HttpReply::json(409, ok_body(false, error));
    }
    return HttpReply::json(200, ok_body(true, std::string()));
}

HttpReply handle_stop(const HttpRequest&)
{
    if (g_state == nullptr) {
        return HttpReply::json(503, ok_body(false, "未绑定运行状态"));
    }
    g_state->request_stop();
    return HttpReply::json(200, ok_body(true, std::string()));
}

}  // namespace

std::string start_dashboard(RunState* state, int port, std::string* error)
{
    static const char* kInternalError = "内部错误：运行状态为空";
    stop_dashboard();
    g_state = state;
    if (g_state == nullptr) {
        if (error != nullptr) {
            *error = kInternalError;
        }
        return std::string();
    }

    std::unique_ptr<HttpServer> server(new HttpServer());
    server->get("/", handle_index);
    server->get("/index.html", handle_index);
    server->get("/api/state", handle_state);
    server->get("/api/suites", handle_suites);
    server->post("/api/run", handle_run);
    server->post("/api/stop", handle_stop);

    if (!server->start_background(port)) {
        if (error != nullptr) {
            *error = server->last_error();
        }
        g_state = nullptr;
        return std::string();
    }
    if (!server->wait_until_ready(5000)) {
        if (error != nullptr) {
            *error = "等待监听就绪超时";
        }
        server->stop();
        g_state = nullptr;
        return std::string();
    }
    const int bound_port = server->port();
    g_server = std::move(server);
    return "http://127.0.0.1:" + lexical_cast<std::string>(bound_port) + "/";
}

void stop_dashboard()
{
    if (g_server) {
        g_server->stop();
        g_server.reset();
    }
    g_state = nullptr;
}

bool open_in_browser(const std::string& url)
{
#if defined(_WIN32)
    const std::string command = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    const std::string command = "open \"" + url + "\"";
#else
    const std::string command = "xdg-open \"" + url + "\"";
#endif
    ProcessResult result = run_shell(command, 15000);
    return result.exit_code == 0;
}

}  // namespace tr
