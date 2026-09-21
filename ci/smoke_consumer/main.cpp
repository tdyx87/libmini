// 安装冒烟测试：证明 find_package(libmini) 的安装产物可用——
// 编译（头文件/宏传播正确）、链接（传递依赖解析正确）、运行（行为正确）。
// 任何断言失败以非零码退出；仅标准库，无 GTest 依赖。
#include <iostream>
#include <string>

#include "libmini.h"
#include "utils/json_utils.h"
#include "utils/rpc.h"
#include "utils/sqlite.h"

using namespace libmini;

static int g_failures = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            std::cout << "FAIL: " << msg << std::endl;      \
            ++g_failures;                                   \
        } else {                                            \
            std::cout << "ok  : " << msg << std::endl;      \
        }                                                   \
    } while (0)

int main()
{
    // 1) 字符串 + JSON（含 nlohmann 传递依赖）
    CHECK(to_upper("find_package ok") == "FIND_PACKAGE OK", "string/upper");
    CHECK(to_json_string_simple("say hi") == "\"say hi\"", "json/escape");

    // 2) UUID
    Uuid u = Uuid::generate();
    Uuid u2;
    CHECK(Uuid::parse(u.to_string(), u2) && u.to_string() == u2.to_string(),
          "uuid/roundtrip");

    // 3) SQLite 内存库（传递依赖 + 语句/行遍历）
    SqliteDatabase db(":memory:");
    db.exec("CREATE TABLE t (v TEXT)");
    SqliteStatement ins(db, "INSERT INTO t VALUES ('hello-from-sqlite')");
    ins.step();
    SqliteStatement q(db, "SELECT v FROM t");
    CHECK(q.step() == SqliteStatement::StepRow &&
              std::string(q.column_text(0)) == "hello-from-sqlite",
          "sqlite/memory roundtrip");

    // 4) 真实 RPC 回环（Tcp 传输，端口 0 自动分配）
    {
        RpcServer server(RpcTransport::Tcp, "127.0.0.1:0");
        server.register_method("echo", [](const std::string& p) { return p; });
        server.start_background();
        CHECK(server.wait_until_ready(5000), "rpc/server ready");

        RpcClient client(RpcTransport::Tcp, server.endpoint());
        const std::string expect = "\"ping\"";
        const std::string r = client.call("echo", expect);
        if (r != expect) {
            // 失败诊断输出（endpoint/错误码/错误信息）
            std::cout << "      endpoint=" << server.endpoint()
                      << " err=" << static_cast<int>(client.last_error())
                      << " msg=" << client.last_error_message() << std::endl;
        }
        CHECK(r == expect, "rpc/echo roundtrip");
        server.stop();
    }

    if (g_failures == 0) {
        std::cout << "consumer OK" << std::endl;
        return 0;
    }
    std::cout << "consumer FAILED (" << g_failures << ")" << std::endl;
    return 1;
}
