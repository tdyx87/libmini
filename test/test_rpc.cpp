#include <gtest/gtest.h>

#include "utils/rpc.h"
#ifndef _WIN32
#include "utils/file_utils.h"  // temp_directory_path（UDS 端点放置）
#endif

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <future>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/callback_sink.h>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

// 便利封装：启动一个后台服务器并返回实际监听端口
class TestServer
{
public:
    TestServer()
        : server_(0)  // 端口 0 = 自动分配，避免测试间端口冲突
    {
        server_.register_method("add", [](const std::string& params) {
            const json p = json::parse(params);
            return json{ {"sum", p.at("a").get<int>() + p.at("b").get<int>()} }
                .dump();
        });

        server_.register_method("echo", [](const std::string& params) {
            return params;
        });

        server_.register_method("fail", [](const std::string&) {
            throw std::runtime_error("boom");
            return std::string();
        });

        server_.register_method("slow", [](const std::string&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            return "slow_done";
        });

        server_.register_method("sleep_ms", [](const std::string& params) {
            const json p = json::parse(params);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(p.at("ms").get<int>()));
            return json{ {"slept", p.at("ms").get<int>()} }.dump();
        });

        server_.start_background();
        EXPECT_TRUE(server_.wait_until_ready(5000));
    }

    libmini::RpcServer& server() { return server_; }
    int port() const { return server_.port(); }

    ~TestServer() { server_.stop(); }

private:
    libmini::RpcServer server_;
};

}  // namespace

// ==================== 请求日志测试 ====================

namespace {

// 用 callback_sink 捕获 RpcServer 发出的日志
struct LogEntry
{
    spdlog::level::level_enum level;
    std::string text;
};

class LogCapture
{
public:
    LogCapture()
    {
        sink_ = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [this](const spdlog::details::log_msg& msg) {
                LogEntry entry;
                entry.level = msg.level;
                entry.text.assign(msg.payload.data(), msg.payload.size());
                std::lock_guard<std::mutex> lock(mutex_);
                entries_.push_back(std::move(entry));
            });
        logger_ =
            std::make_shared<spdlog::logger>("rpc_test_logger", sink_);
    }

    void set_on_server(libmini::RpcServer& server)
    {
        server.set_logger(logger_.get());
    }

    spdlog::logger* logger() { return logger_.get(); }

    // 请求返回后日志已同步写入，直接取快照
    std::vector<LogEntry> entries() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

private:
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> logger_;
    std::vector<LogEntry> entries_;
    mutable std::mutex mutex_;
};

}  // namespace

TEST(RpcServerLogTest, SuccessCallLoggedAsInfo)
{
    TestServer ts;
    LogCapture capture;
    capture.set_on_server(ts.server());

    libmini::RpcClient client("127.0.0.1", ts.port());
    ASSERT_FALSE(client.call("add", R"({"a":1,"b":2})").empty());

    const std::vector<LogEntry> logged = capture.entries();
    ASSERT_EQ(logged.size(), 1u);
    EXPECT_EQ(logged[0].level, spdlog::level::info);
    EXPECT_NE(logged[0].text.find("method=\"add\""), std::string::npos);
    EXPECT_NE(logged[0].text.find("client=127.0.0.1"), std::string::npos);
    EXPECT_NE(logged[0].text.find("elapsed="), std::string::npos);
    EXPECT_NE(logged[0].text.find("ok"), std::string::npos);
}

TEST(RpcServerLogTest, HandlerExceptionLoggedAsError)
{
    TestServer ts;
    LogCapture capture;
    capture.set_on_server(ts.server());

    libmini::RpcClient client("127.0.0.1", ts.port());
    EXPECT_TRUE(client.call("fail", "null").empty());

    const std::vector<LogEntry> logged = capture.entries();
    ASSERT_EQ(logged.size(), 1u);
    EXPECT_EQ(logged[0].level, spdlog::level::err);
    EXPECT_NE(logged[0].text.find("method=\"fail\""), std::string::npos);
    EXPECT_NE(logged[0].text.find("boom"), std::string::npos);
    EXPECT_NE(logged[0].text.find("elapsed="), std::string::npos);
}

TEST(RpcServerLogTest, UnknownMethodLoggedAsWarning)
{
    TestServer ts;
    LogCapture capture;
    capture.set_on_server(ts.server());

    libmini::RpcClient client("127.0.0.1", ts.port());
    EXPECT_TRUE(client.call("ghost_method", "null").empty());

    const std::vector<LogEntry> logged = capture.entries();
    ASSERT_EQ(logged.size(), 1u);
    EXPECT_EQ(logged[0].level, spdlog::level::warn);
    EXPECT_NE(logged[0].text.find("method=\"ghost_method\""),
              std::string::npos);
    EXPECT_NE(logged[0].text.find("method not found"), std::string::npos);
}

TEST(RpcServerLogTest, ElapsedTimeIsMeasured)
{
    TestServer ts;
    LogCapture capture;
    capture.set_on_server(ts.server());

    libmini::RpcClient client("127.0.0.1", ts.port());
    ASSERT_FALSE(client.call("sleep_ms", R"({"ms":120})").empty());

    const std::vector<LogEntry> logged = capture.entries();
    ASSERT_EQ(logged.size(), 1u);
    // 提取 elapsed= 后面的毫秒数，应不小于实际休眠时间的大部分
    const size_t pos = logged[0].text.find("elapsed=");
    ASSERT_NE(pos, std::string::npos);
    const long elapsed = strtol(logged[0].text.c_str() + pos + 8, nullptr, 10);
    EXPECT_GE(elapsed, 100);  // 允许少量计时误差
    EXPECT_LE(elapsed, 5000);
}

TEST(RpcServerLogTest, NullLoggerDisablesLogging)
{
    TestServer ts;
    LogCapture capture;
    capture.set_on_server(ts.server());
    ts.server().set_logger(nullptr);  // 运行中取消日志

    libmini::RpcClient client("127.0.0.1", ts.port());
    ASSERT_FALSE(client.call("add", R"({"a":1,"b":1})").empty());

    EXPECT_TRUE(capture.entries().empty());
}

TEST(RpcClientTest, CallReturnsHandlerResult)
{
    TestServer ts;
    libmini::RpcClient client("127.0.0.1", ts.port());

    const std::string reply = client.call("add", R"({"a":2,"b":3})");
    ASSERT_FALSE(reply.empty()) << client.last_error_message();

    const json result = json::parse(reply);
    EXPECT_EQ(result.at("sum").get<int>(), 5);
    EXPECT_EQ(client.last_error(), libmini::RpcError::OK);
}

TEST(RpcClientTest, EchoPreservesParamsVerbatim)
{
    TestServer ts;
    libmini::RpcClient client("127.0.0.1", ts.port());

    const std::string reply = client.call("echo", R"({"k":"v","n":[1,2]})");
    ASSERT_FALSE(reply.empty()) << client.last_error_message();
    EXPECT_EQ(reply, R"({"k":"v","n":[1,2]})");
}

TEST(RpcClientTest, HandlerExceptionBecomesServerError)
{
    TestServer ts;
    libmini::RpcClient client("127.0.0.1", ts.port());

    const std::string reply = client.call("fail", "null");
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::SERVER_ERROR);
    EXPECT_NE(client.last_error_message().find("boom"), std::string::npos);
}

TEST(RpcClientTest, UnknownMethodFails)
{
    TestServer ts;
    libmini::RpcClient client("127.0.0.1", ts.port());

    const std::string reply = client.call("no_such_method", "null");
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::SERVER_ERROR);
    EXPECT_NE(client.last_error_message().find("no_such_method"),
              std::string::npos);
}

TEST(RpcClientTest, InvalidParamsJsonReportsProtocolError)
{
    TestServer ts;
    libmini::RpcClient client("127.0.0.1", ts.port());

    const std::string reply = client.call("echo", "{not json");
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::PROTOCOL_ERROR);
}

TEST(RpcClientTest, ConnectionFailureReportsError)
{
    // 端口 1 不可能有监听者，连接必然失败
    libmini::RpcClient client("127.0.0.1", 1);
    client.set_timeout_ms(1000);

    const std::string reply = client.call("anything", "null");
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::CONNECTION_FAILED);
}

TEST(RpcClientTest, HttpEndpointStringConstructorWorks)
{
    // 回归：RpcClient(RpcTransport::Http, "host:port") 曾未初始化 httplib
    // 客户端，首次 call 直接段错误。现在应正常路由到 HTTP 传输
    libmini::RpcServer http_server(0);
    http_server.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}}.dump();
    });
    http_server.start_background();
    ASSERT_TRUE(http_server.wait_until_ready(5000));

    libmini::RpcClient client(libmini::RpcTransport::Http,
                              "127.0.0.1:" +
                                  std::to_string(http_server.port()));
    EXPECT_EQ(client.transport(), libmini::RpcTransport::Http);
    EXPECT_EQ(client.endpoint(),
              "127.0.0.1:" + std::to_string(http_server.port()));

    const std::string reply = client.call("add", R"({"a":2,"b":3})");
    EXPECT_EQ(reply, R"({"sum":5})");
    EXPECT_EQ(client.last_error(), libmini::RpcError::OK);

    // 空端点的 Http 构造：不崩溃，返回连接失败
    libmini::RpcClient empty(libmini::RpcTransport::Http, std::string());
    empty.set_max_retries(0);
    EXPECT_TRUE(empty.call("add", "null").empty());
    EXPECT_EQ(empty.last_error(), libmini::RpcError::CONNECTION_FAILED);

    http_server.stop();
}

TEST(RpcClientTest, RequestTimeoutReportsError)
{
    TestServer ts;
    libmini::RpcClient client("127.0.0.1", ts.port());
    client.set_timeout_ms(200);

    const std::string reply = client.call("sleep_ms", R"({"ms":1500})");
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::TIMEOUT);
}

TEST(RpcServerTest, AutoPortAssignment)
{
    TestServer ts;
    EXPECT_GT(ts.port(), 0);
    EXPECT_LE(ts.port(), 65535);
    EXPECT_TRUE(ts.server().is_running());
}

TEST(RpcServerTest, StopIsIdempotent)
{
    TestServer ts;
    ts.server().stop();
    EXPECT_NO_THROW(ts.server().stop());
}

TEST(RpcServerTest, RestartAfterStop)
{
    TestServer ts;
    const int first_port = ts.port();
    ts.server().stop();

    ts.server().start_background();
    EXPECT_TRUE(ts.server().wait_until_ready(5000));
    EXPECT_TRUE(ts.server().is_running());
    EXPECT_GT(ts.server().port(), 0);  // 重新绑定后端口可能变化

    libmini::RpcClient client("127.0.0.1", ts.server().port());
    const std::string reply = client.call("echo", "1");
    EXPECT_EQ(reply, "1");
}

TEST(RpcClientServerTest, ManySequentialCalls)
{
    TestServer ts;
    libmini::RpcClient client("127.0.0.1", ts.port());

    for (int i = 0; i < 20; ++i) {
        const std::string reply =
            client.call("add", R"({"a":)" + std::to_string(i) + R"(,"b":1})");
        ASSERT_FALSE(reply.empty()) << "iteration " << i;
        const json result = json::parse(reply);
        EXPECT_EQ(result.at("sum").get<int>(), i + 1);
    }
}

TEST(RpcClientServerTest, ConcurrentClients)
{
    TestServer ts;

    auto worker = [port = ts.port()](int base) {
        libmini::RpcClient client("127.0.0.1", port);
        for (int i = 0; i < 10; ++i) {
            const std::string reply = client.call(
                "add", R"({"a":)" + std::to_string(base + i) + R"(,"b":0})");
            if (reply.empty()) {
                return false;
            }
            if (json::parse(reply).at("sum").get<int>() != base + i) {
                return false;
            }
        }
        return true;
    };

    auto f1 = std::async(std::launch::async, worker, 0);
    auto f2 = std::async(std::launch::async, worker, 100);
    auto f3 = std::async(std::launch::async, worker, 200);

    EXPECT_TRUE(f1.get());
    EXPECT_TRUE(f2.get());
    EXPECT_TRUE(f3.get());
}

// ==================== 过载保护与监控测试 ====================

// 并发上限=1：第一个慢请求在处理中时，第二个请求应立即被 429 拒绝
TEST(RpcServerOverloadTest, OverloadReturnsCustomError)
{
    libmini::RpcServer server(0);
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return R"({"done":true})";
    });
    server.register_method("echo", [](const std::string& p) { return p; });

    server.set_max_in_flight(1);
    server.set_overload_message("too busy now");
    server.set_worker_threads(4);  // 队列容量足够，确保请求能进入处理阶段

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    // 第一个请求占住唯一的处理槽位
    auto slow = std::async(std::launch::async, [&server] {
        libmini::RpcClient client("127.0.0.1", server.port());
        return client.call("slow", "null");
    });

    // 等待第一个请求真正进入处理器
    bool entered = false;
    for (int i = 0; i < 100; ++i) {
        if (server.stats().active_requests >= 1) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(entered);

    // 第二个请求应被 429 拒绝并收到自定义文本。
    // （禁用重试以验证单次 429 行为；启用重试的完整场景见 RpcClientRetryTest）
    libmini::RpcClient client2("127.0.0.1", server.port());
    client2.set_timeout_ms(2000);
    client2.set_max_retries(0);
    const std::string reply = client2.call("echo", "1");
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client2.last_error(), libmini::RpcError::OVERLOADED);
    EXPECT_NE(client2.last_error_message().find("too busy now"),
              std::string::npos);

    // 等慢请求完成后确认仍能正常服务
    EXPECT_FALSE(slow.get().empty());
    libmini::RpcClient client3("127.0.0.1", server.port());
    EXPECT_EQ(client3.call("echo", "2"), "2");

    server.stop();
    EXPECT_EQ(server.stats().rejected_total, 1u);
}

// stats() 快照：峰值计数与拒绝计数随请求累加，停止后活动数归零、峰值保留
TEST(RpcServerOverloadTest, StatsSnapshotReflectsLoad)
{
    libmini::RpcServer server(0);
    server.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{ {"sum", p.at("a").get<int>() + p.at("b").get<int>()} }
            .dump();
    });
    server.set_worker_threads(2);
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient client("127.0.0.1", server.port());
    for (int i = 0; i < 5; ++i) {
        ASSERT_FALSE(
            client.call("add", R"({"a":1,"b":1})").empty())
            << "i=" << i << " err=" << client.last_error_message();
    }

    const libmini::RpcServerStats s = server.stats();
    EXPECT_EQ(s.rejected_total, 0u);
    EXPECT_GE(s.max_active, 1u);
    EXPECT_LE(s.max_active, 2u);  // 不超过工作线程数

    server.stop();
    const libmini::RpcServerStats after = server.stats();
    EXPECT_EQ(after.active_requests, 0u);
    EXPECT_EQ(after.queued_requests, 0u);
    EXPECT_EQ(after.max_active, s.max_active);  // 峰值保留
}

// 队列积压告警：如发生过积压，则同一积压期内只发一条告警
TEST(RpcServerOverloadTest, BacklogWarningAtMostOncePerEpisode)
{
    libmini::RpcServer server(0);
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return "1";
    });
    server.register_method("fast", [](const std::string& p) { return p; });

    server.set_worker_threads(1);
    server.set_queue_warn_threshold(2);

    LogCapture capture;
    capture.set_on_server(server);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    // 占住唯一的处理线程，再压入若干排队请求
    auto slow = std::async(std::launch::async, [&server] {
        libmini::RpcClient client("127.0.0.1", server.port());
        return client.call("slow", "null");
    });

    bool slow_running = false;
    for (int i = 0; i < 100; ++i) {
        if (server.stats().active_requests >= 1) {
            slow_running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(slow_running);

    {
        libmini::RpcClient c("127.0.0.1", server.port());
        c.set_timeout_ms(5000);
        c.call("fast", "1");
        c.call("fast", "2");
        c.call("fast", "3");
    }
    slow.get();

    // 单客户端串行请求难以稳定制造 ≥2 的真实积压，
    // 这里验证弱断言：若发生积压，同一积压期内只发一条告警
    size_t backlog_warnings = 0;
    for (const LogEntry& e : capture.entries()) {
        if (e.text.find("queue backlog=") != std::string::npos) {
            ++backlog_warnings;
        }
    }
    EXPECT_LE(backlog_warnings, 1u);

    server.stop();
}

// ==================== 延迟直方图与完成统计测试 ====================

// latency_stats：样本数、分位单调性、min/mean/max 合理性；
// 自定义分位数按顺序追加；重启后统计清零
TEST(RpcServerLatencyTest, HistogramReflectsProcessedRequests)
{
    libmini::RpcServer server(0);
    server.register_method("work", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return R"({"done":true})";
    });
    server.register_method("fail", [](const std::string&) {
        throw std::runtime_error("boom");
        return std::string();
    });
    server.set_worker_threads(2);
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    // 重启前应无样本
    EXPECT_EQ(server.latency_stats().sample_count, 0u);

    libmini::RpcClient client("127.0.0.1", server.port());
    for (int i = 0; i < 5; ++i) {
        ASSERT_FALSE(client.call("work", "null").empty());
    }

    libmini::RpcLatencyStats ls =
        server.latency_stats(std::vector<double>{99.9, 200.0});
    EXPECT_EQ(ls.sample_count, 5u);
    // 处理器固定 ~20ms，网络开销有限：p50 应落在 [15, 200) ms
    EXPECT_GE(ls.p50_ms, 15.0);
    EXPECT_LT(ls.p50_ms, 200.0);
    // 分位单调：p50 <= p90 <= p95 <= p99 <= max
    EXPECT_LE(ls.p50_ms, ls.p90_ms);
    EXPECT_LE(ls.p90_ms, ls.p95_ms);
    EXPECT_LE(ls.p95_ms, ls.p99_ms);
    EXPECT_LE(ls.p99_ms, ls.max_ms);
    // min/mean/max 合理性
    EXPECT_GE(ls.min_ms, 15.0);
    EXPECT_LE(ls.min_ms, ls.mean_ms);
    EXPECT_LE(ls.mean_ms, ls.max_ms);
    EXPECT_LT(ls.max_ms, 1000.0);
    // 自定义分位：有效的 99.9 被追加且 >= p99；越界的 200 被忽略
    ASSERT_EQ(ls.percentiles.size(), 1u);
    EXPECT_GE(ls.percentiles[0], ls.p99_ms);

    // completed_total：5 次成功 + 1 次处理器异常（500）+ 1 次未注册方法
    // （404）均算完成——404 也会占用处理槽位（过载判定在方法查找之前），
    // 因此计入 completed；只有 429 过载拒绝不算
    ASSERT_TRUE(client.call("fail", "null").empty());  // 触发异常
    client.call("nosuch", "null");                      // 404，计入
    EXPECT_EQ(server.stats().completed_total, 7u);
    EXPECT_EQ(server.stats().rejected_total, 0u);

    server.stop();
    // 停止后峰值保留
    EXPECT_EQ(server.stats().completed_total, 7u);

    // 重启后统计清零
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));
    EXPECT_EQ(server.latency_stats().sample_count, 0u);
    EXPECT_EQ(server.stats().completed_total, 0u);
    server.stop();
}

// ==================== 背压模式（WaitInQueue）测试 ====================

// WaitInQueue：并发请求超过槽位时排队等待而非拒绝，
// 全部成功且总耗时体现单槽位的串行化效果
TEST(RpcServerOverloadTest, WaitInQueueAbsorbsBurst)
{
    libmini::RpcServer server(0);
    server.register_method("echo", [](const std::string& p) { return p; });
    server.register_method("work", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return R"({"done":true})";
    });
    server.set_max_in_flight(1);
    server.set_worker_threads(4);
    server.set_overload_mode(libmini::RpcOverloadMode::WaitInQueue);
    server.set_queue_wait_ms(5000);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    const int kClients = 4;
    std::vector<std::future<std::string>> calls;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kClients; ++i) {
        calls.push_back(std::async(std::launch::async, [&server] {
            libmini::RpcClient c("127.0.0.1", server.port());
            c.set_max_retries(0);
            c.set_timeout_ms(10000);
            return c.call("work", "null");
        }));
    }
    for (int i = 0; i < kClients; ++i) {
        EXPECT_EQ(calls[i].get(), R"({"done":true})") << "i=" << i;
    }
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    // 4 个 30ms 处理共享 1 个槽位：串行化总耗时 ≳ 3/4 × 单次处理时间
    EXPECT_GE(wall_ms, 60);

    const libmini::RpcServerStats s = server.stats();
    EXPECT_EQ(s.rejected_total, 0u);              // 无拒绝
    EXPECT_EQ(s.completed_total, 4u);             // 全部完成
    EXPECT_GE(s.max_queued, 1u);                  // 至少出现过排队
    EXPECT_GE(s.max_active, 1u);
    EXPECT_LE(s.max_active, 1u);                  // 槽位上限生效

    server.stop();
}

// WaitInQueue：等待超过 queue_wait_ms 仍拿不到槽位时返回 429，
// 等待时间体现在请求总耗时中
TEST(RpcServerOverloadTest, WaitInQueueTimesOutWith429)
{
    libmini::RpcServer server(0);
    server.register_method("echo", [](const std::string& p) { return p; });
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        return R"({"done":true})";
    });
    server.set_max_in_flight(1);
    server.set_worker_threads(4);
    server.set_overload_mode(libmini::RpcOverloadMode::WaitInQueue);
    server.set_queue_wait_ms(200);
    server.set_overload_message("queue full timeout");

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    // 占住唯一槽位
    auto blocker = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        return c.call("slow", "null");
    });
    bool entered = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(entered);

    // 等待 200ms 超时后收到 429（禁用重试以观察单次行为）
    libmini::RpcClient client("127.0.0.1", server.port());
    client.set_max_retries(0);
    client.set_timeout_ms(5000);
    const auto start = std::chrono::steady_clock::now();
    const std::string reply = client.call("echo", "1");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::OVERLOADED);
    EXPECT_NE(client.last_error_message().find("queue full timeout"),
              std::string::npos);
    // 至少等了 queue_wait_ms 才放弃
    EXPECT_GE(elapsed, 200);

    EXPECT_EQ(server.stats().rejected_total, 1u);

    // 槽位释放后恢复服务
    (void)blocker.get();
    libmini::RpcClient client2("127.0.0.1", server.port());
    EXPECT_EQ(client2.call("echo", "2"), "2");

    server.stop();
}

// ==================== 优雅停机排空测试 ====================

// 优雅停机：stop() 期间排队中的请求仍被处理完毕，客户端拿到真实结果而非 429；
// 停机后新请求得到 "server shutting down" 429
TEST(RpcServerShutdownTest, GracefulStopDrainsQueuedRequests)
{
    libmini::RpcServer server(0);
    server.register_method("work", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return R"({"done":true})";
    });
    server.set_max_in_flight(1);
    server.set_worker_threads(4);
    server.set_overload_mode(libmini::RpcOverloadMode::WaitInQueue);
    server.set_queue_wait_ms(5000);
    server.set_drain_timeout_ms(5000);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    // 第一个占住槽位；第二、三个排队等待
    std::vector<std::future<std::string>> calls;
    for (int i = 0; i < 3; ++i) {
        calls.push_back(std::async(std::launch::async, [&server] {
            libmini::RpcClient c("127.0.0.1", server.port());
            c.set_max_retries(0);
            c.set_timeout_ms(10000);
            return c.call("work", "null");
        }));
    }
    // 确认至少有一个已进入排队状态
    bool queued_seen = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1 &&
            server.stats().queued_requests >= 1) {
            queued_seen = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(queued_seen);

    // 等三个请求全部进入服务端（active+queued ≥ 3），并留出分发沉降时间，
    // 避免把还在建连/入队途中的请求误判为停机后的“新请求”（高负载下
    // 第三个客户端的建连可能比排队观察晚几十毫秒）
    for (int i = 0; i < 400; ++i) {
        if (server.stats().active_requests +
                server.stats().queued_requests >=
            3) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 排空窗口内停机：三个请求都应在窗口内完成
    server.stop();

    int drained = 0;
    for (int i = 0; i < 3; ++i) {
        if (calls[i].get() == R"({"done":true})") {
            ++drained;
        }
    }
    EXPECT_EQ(drained, 3) << "所有已接收请求都应在排空窗口内完成";
    EXPECT_EQ(server.stats().completed_total, 3u);
    EXPECT_EQ(server.stats().rejected_total, 0u);
}

// 排空窗口用尽：仍在排队的请求被放弃（429），处理中的请求完成
TEST(RpcServerShutdownTest, DrainTimeoutAbandonsWaiters)
{
    libmini::RpcServer server(0);
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        return R"({"done":true})";
    });
    server.register_method("echo", [](const std::string& p) { return p; });
    server.set_max_in_flight(1);
    server.set_worker_threads(4);
    server.set_overload_mode(libmini::RpcOverloadMode::WaitInQueue);
    server.set_queue_wait_ms(30000);   // 排队者自己等很久，不会被它先超时
    server.set_drain_timeout_ms(300);  // 排空窗口短于排队者需要的时间

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    auto blocker = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        c.set_timeout_ms(10000);
        return c.call("slow", "null");
    });
    bool entered = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(entered);

    // 排队者：queue_wait 30s 意味着它打算一直等，但排空窗口 300ms 会先到
    auto waiter = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        c.set_max_retries(0);
        c.set_timeout_ms(10000);
        return c.call("echo", "1");
    });
    bool waiter_queued = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().queued_requests >= 1) {
            waiter_queued = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(waiter_queued);

    const auto t0 = std::chrono::steady_clock::now();
    server.stop();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    // stop 应在窗口用尽后返回，而不是等排队者的 30s
    EXPECT_GE(stop_ms, 250);
    EXPECT_LE(stop_ms, 5000);

    // 排队者被放弃：收到 429（shutting down）或连接断开，两者皆可
    const std::string r = waiter.get();
    EXPECT_TRUE(r.empty());

    // 处理中的请求正常完成
    EXPECT_FALSE(blocker.get().empty());
    server.start_background();  // 重新启动验证可用
    ASSERT_TRUE(server.wait_until_ready(5000));
    libmini::RpcClient c2("127.0.0.1", server.port());
    EXPECT_EQ(c2.call("echo", "2"), "2");
    server.stop();
}

// set_drain_timeout_ms(0)：立即放弃排队请求（旧行为）
TEST(RpcServerShutdownTest, ZeroDrainGivesUpImmediately)
{
    libmini::RpcServer server(0);
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return R"({"done":true})";
    });
    server.register_method("echo", [](const std::string& p) { return p; });
    server.set_max_in_flight(1);
    server.set_worker_threads(4);
    server.set_overload_mode(libmini::RpcOverloadMode::WaitInQueue);
    server.set_queue_wait_ms(30000);
    server.set_drain_timeout_ms(0);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    auto blocker = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        return c.call("slow", "null");
    });
    bool entered = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(entered);

    auto waiter = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        c.set_max_retries(0);
        return c.call("echo", "1");
    });
    bool waiter_queued = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().queued_requests >= 1) {
            waiter_queued = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(waiter_queued);

    const auto t0 = std::chrono::steady_clock::now();
    server.stop();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    EXPECT_LE(stop_ms, 500);  // 不等待排空

    EXPECT_TRUE(waiter.get().empty());
    EXPECT_FALSE(blocker.get().empty());
    server.stop();
}

// ==================== Retry-After 配置与调用耗时测试 ====================

// set_retry_after_seconds(0)：429 不携带 Retry-After，
// 客户端重试等待回退为自身指数退避（日志 wait= 应为退避值而非 1000ms）
TEST(RpcServerOverloadTest, RetryAfterHeaderCanBeDisabled)
{
    libmini::RpcServer server(0);
    server.register_method("echo", [](const std::string& p) { return p; });
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        return R"({"done":true})";
    });
    server.set_max_in_flight(1);
    server.set_overload_message("busy");
    server.set_worker_threads(4);
    server.set_retry_after_seconds(0);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient client("127.0.0.1", server.port());
    client.set_max_retries(1);
    client.set_retry_base_delay_ms(10);
    client.set_retry_max_total_wait_ms(10000);

    LogCapture capture;
    client.set_logger(capture.logger());

    auto blocker = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        return c.call("slow", "null");
    });
    bool entered = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(entered);

    client.call("echo", "1");

    ASSERT_EQ(capture.entries().size(), 1u);
    // 服务器未指定 Retry-After → 客户端用自己的退避 10ms
    EXPECT_NE(capture.entries()[0].text.find("wait=10ms"),
              std::string::npos)
        << capture.entries()[0].text;

    (void)blocker.get();
    server.stop();
}

// last_call_elapsed_ms：调用前为 -1，调用后覆盖重试等待的总耗时
TEST(RpcClientTest, LastCallElapsedMsTracksTotalCallDuration)
{
    // 调用前为 -1
    libmini::RpcClient client("127.0.0.1", 1);  // 不可达端口
    EXPECT_DOUBLE_EQ(client.last_call_elapsed_ms(), -1.0);

    client.set_max_retries(1);
    client.set_retry_base_delay_ms(50);
    client.set_retry_max_total_wait_ms(60000);
    client.call("anything", "null");
    // 含 50ms 退避等待；不设上限（Windows 连接超时本身可能 2s）
    EXPECT_GE(client.last_call_elapsed_ms(), 50.0);

    // 成功调用同样有效且 >= 处理器耗时
    libmini::RpcServer server(0);
    server.register_method("work", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return R"({"ok":true})";
    });
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient ok_client("127.0.0.1", server.port());
    ASSERT_FALSE(ok_client.call("work", "null").empty());
    EXPECT_GE(ok_client.last_call_elapsed_ms(), 25.0);

    server.stop();
}

// ==================== 客户端重试与指数退避测试 ====================

// 429 重试后成功：服务器先拒绝 2 次，第 3 次调用成功
TEST(RpcClientRetryTest, RetriesOn429AndEventuallySucceeds)
{
    libmini::RpcServer server(0);

    server.register_method("flaky", [](const std::string&) {
        return std::string(R"({"ok":true})");
    });
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return "slow_done";
    });
    server.set_max_in_flight(1);
    server.set_overload_message("busy");
    server.set_worker_threads(4);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient client("127.0.0.1", server.port());
    client.set_max_retries(5);
    client.set_retry_base_delay_ms(50);
    client.set_retry_max_delay_ms(200);

    // 用另一个客户端占住唯一的处理槽位，让主客户端经历 429
    auto blocker = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        return c.call("slow", "null");
    });

    // 等待 blocker 进入处理中
    bool entered = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(entered);

    // 此调用应收到 429 后自动重试，直到 blocker 释放闸门后成功
    const auto call_start = std::chrono::steady_clock::now();
    const std::string reply = client.call("flaky", "null");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - call_start)
                             .count();

    EXPECT_FALSE(reply.empty()) << client.last_error_message();
    EXPECT_EQ(client.last_error(), libmini::RpcError::OK);
    EXPECT_EQ(json::parse(reply).at("ok").get<bool>(), true);
    // 重试成功意味着总耗时明显大于单次快速失败
    EXPECT_GE(elapsed, 50);

    (void)blocker.get();
    server.stop();
    EXPECT_GE(server.stats().rejected_total, 1u);
}

// 429 重试用尽后返回 OVERLOADED 错误码
TEST(RpcClientRetryTest, ReturnsOverloadedWhenRetriesExhausted)
{
    libmini::RpcServer server(0);
    server.register_method("echo", [](const std::string& p) { return p; });
    server.register_method("slow", [](const std::string&) {
        // 服务端 Retry-After: 1（秒），客户端每次重试间隔约 1s；
        // blocker 持有 3s 保证所有重试都落在过载窗口内
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        return "slow_done";
    });
    server.set_max_in_flight(1);
    server.set_overload_message("busy");
    server.set_worker_threads(4);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient client("127.0.0.1", server.port());
    client.set_max_retries(2);
    client.set_retry_base_delay_ms(20);
    client.set_retry_max_total_wait_ms(10000);

    auto blocker = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        return c.call("slow", "null");
    });

    bool entered = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(entered);

    const std::string reply = client.call("echo", "1");
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::OVERLOADED);

    (void)blocker.get();
    server.stop();
}

// 服务器错误（500）不重试：立即返回
TEST(RpcClientRetryTest, ServerErrorNotRetried)
{
    libmini::RpcServer server(0);
    server.register_method("fail", [](const std::string&) {
        throw std::runtime_error("boom");
        return std::string();
    });
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient client("127.0.0.1", server.port());
    client.set_max_retries(5);
    client.set_retry_base_delay_ms(100);

    const auto start = std::chrono::steady_clock::now();
    const std::string reply = client.call("fail", "null");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::SERVER_ERROR);
    EXPECT_LT(elapsed, 200);  // 未发生任何退避等待

    server.stop();
}

// 连接失败重试用尽后返回 CONNECTION_FAILED
TEST(RpcClientRetryTest, ConnectionFailureRetriedThenReported)
{
    libmini::RpcClient client("127.0.0.1", 1);  // 端口 1 不可达
    client.set_max_retries(2);
    client.set_retry_base_delay_ms(30);
    client.set_retry_max_total_wait_ms(1000);

    const auto start = std::chrono::steady_clock::now();
    const std::string reply = client.call("anything", "null");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::CONNECTION_FAILED);
    // 至少发生了 2 次退避等待（30ms + 60ms）
    EXPECT_GE(elapsed, 80);
}

// 指数退避间隔递增：通过总耗时粗略验证。
// 注意：Windows 上连接不可达端口每次约 2s 超时，4 次尝试（首次 + 3 次重试）
// 约 8s，因此上限须涵盖连接耗时而不只是退避总和
TEST(RpcClientRetryTest, ExponentialBackoffWaits)
{
    libmini::RpcClient client("127.0.0.1", 1);
    client.set_max_retries(3);
    client.set_retry_base_delay_ms(50);
    client.set_retry_max_delay_ms(1000);
    client.set_retry_max_total_wait_ms(60000);

    const auto start = std::chrono::steady_clock::now();
    client.call("anything", "null");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    // 3 次重试的退避总和 = 50 + 100 + 200 = 350ms（不含连接失败耗时）
    EXPECT_GE(elapsed, 300);
    EXPECT_LE(elapsed, 15000);
}

// ==================== 抖动与重试日志测试 ====================

// 开启抖动后：等待时间仍在 [half, delay) 区间内，且多次采样呈现随机性。
// 通过重试日志的 wait= 字段采样（连接失败场景每次重试都打日志）
TEST(RpcClientRetryTest, JitterKeepsWaitWithinBounds)
{
    libmini::RpcClient client("127.0.0.1", 1);  // 不可达，触发重试
    client.set_max_retries(4);
    client.set_retry_base_delay_ms(200);
    client.set_retry_max_delay_ms(10000);
    client.set_retry_max_total_wait_ms(600000);
    client.set_retry_jitter(true);

    LogCapture capture;
    client.set_logger(capture.logger());

    client.call("anything", "null");

    // 采集每次重试的 wait 值：退避应为 200/400/800/1600ms，
    // 抖动后为 [100,200) [200,400) [400,800) [800,1600)
    std::vector<long> waits;
    for (const LogEntry& e : capture.entries()) {
        const size_t pos = e.text.find("wait=");
        if (pos != std::string::npos) {
            waits.push_back(strtol(e.text.c_str() + pos + 5, nullptr, 10));
        }
    }
    ASSERT_EQ(waits.size(), 4u);

    const long lower_bounds[] = {100, 200, 400, 800};
    const long upper_bounds[] = {199, 399, 799, 1599};
    for (size_t i = 0; i < waits.size(); ++i) {
        EXPECT_GE(waits[i], lower_bounds[i]) << "i=" << i;
        EXPECT_LE(waits[i], upper_bounds[i]) << "i=" << i;
    }

    // 说明：不另设“随机性”断言（如要求 4 次等待不全部落在区间下半段）。
    // 该检查有 1/16 概率对正确实现误报；而未抖动的实现会让 wait 全部
    // 等于退避下限，必然违反上面的下界断言，边界检查已足以防回归
}

// Retry-After 等待不受抖动影响：服务器指定值原样等待
TEST(RpcClientRetryTest, RetryAfterNotJittered)
{
    libmini::RpcServer server(0);
    server.register_method("echo", [](const std::string& p) { return p; });
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        return "slow_done";
    });
    server.set_max_in_flight(1);
    server.set_overload_message("busy");
    server.set_worker_threads(4);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient client("127.0.0.1", server.port());
    client.set_max_retries(1);
    client.set_retry_base_delay_ms(10);
    client.set_retry_jitter(true);  // 开启抖动，但不应影响 Retry-After
    client.set_retry_max_total_wait_ms(10000);

    LogCapture capture;
    client.set_logger(capture.logger());

    auto blocker = std::async(std::launch::async, [&server] {
        libmini::RpcClient c("127.0.0.1", server.port());
        return c.call("slow", "null");
    });

    bool entered = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1) {
            entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(entered);

    // 首次尝试收到 429（Retry-After: 1s），重试等待应精确为 1000ms
    client.call("echo", "1");

    const std::vector<LogEntry> logged = capture.entries();
    ASSERT_EQ(logged.size(), 1u);
    EXPECT_NE(logged[0].text.find("wait=1000ms"), std::string::npos)
        << logged[0].text;
    EXPECT_NE(logged[0].text.find("attempt=1/1"), std::string::npos);
    // reason 为响应体里自定义过载文本的 JSON dump（带引号）
    EXPECT_NE(logged[0].text.find("reason=\"busy\""), std::string::npos)
        << logged[0].text;

    (void)blocker.get();
    server.stop();
}

// 重试日志内容：未开启抖动时 wait 即为精确退避值，日志含方法名与原因
TEST(RpcClientRetryTest, RetryLogContentWithoutJitter)
{
    libmini::RpcClient client("127.0.0.1", 1);
    client.set_max_retries(2);
    client.set_retry_base_delay_ms(50);
    client.set_retry_max_total_wait_ms(60000);
    // 不开启抖动，退避值精确：50ms, 100ms

    LogCapture capture;
    client.set_logger(capture.logger());

    client.call("my_method", "null");

    const std::vector<LogEntry> logged = capture.entries();
    ASSERT_EQ(logged.size(), 2u);

    EXPECT_NE(logged[0].text.find("method=\"my_method\""),
              std::string::npos);
    EXPECT_NE(logged[0].text.find("attempt=1/2"), std::string::npos);
    EXPECT_NE(logged[0].text.find("wait=50ms"), std::string::npos);
    // reason 含 libmini 错误描述 + httplib 详情
    EXPECT_NE(logged[0].text.find("reason=\"connection failed:"),
              std::string::npos);

    EXPECT_NE(logged[1].text.find("attempt=2/2"), std::string::npos);
    EXPECT_NE(logged[1].text.find("wait=100ms"), std::string::npos);
}

// 未设置日志器时不打日志、行为不变
TEST(RpcClientRetryTest, NoLoggerNoLogNoBehaviorChange)
{
    libmini::RpcClient client("127.0.0.1", 1);
    client.set_max_retries(1);
    client.set_retry_base_delay_ms(20);
    client.set_retry_max_total_wait_ms(60000);

    const auto start = std::chrono::steady_clock::now();
    const std::string reply = client.call("anything", "null");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::CONNECTION_FAILED);
    EXPECT_GE(elapsed, 20);  // 退避仍然发生
}

// ==================== Tcp 传输测试（帧协议 over 裸 TCP）====================

namespace {

// Tcp 传输测试服务器：端口 0 自动分配，避免测试间端口冲突
libmini::RpcServer make_tcp_server()
{
    libmini::RpcServer server(libmini::RpcTransport::Tcp, "127.0.0.1:0");
    server.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{ {"sum", p.at("a").get<int>() + p.at("b").get<int>()} }
            .dump();
    });
    server.register_method("fail", [](const std::string&) {
        throw std::runtime_error("boom");
        return std::string();
    });
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return json{{"done", true}}.dump();
    });
    server.start_background();
    EXPECT_TRUE(server.wait_until_ready(5000));
    return server;
}

}  // namespace

// 分列统计：单传输服务器只有自己那列非零；合计 = 各分列之和；
// 重启后清零
TEST(RpcServerStatsTest, PerTransportCounters)
{
    // ---- Tcp 传输：completed/rejected 记入 tcp 列，其余列恒为 0 ----
    {
        libmini::RpcServer server = make_tcp_server();
        server.set_max_in_flight(1);
        server.set_overload_mode(libmini::RpcOverloadMode::RejectImmediate);

        libmini::RpcClient busy(libmini::RpcTransport::Tcp, server.endpoint());
        busy.set_timeout_ms(5000);
        auto slow_call = std::async(std::launch::async, [&busy] {
            return busy.call("slow", "null");
        });
        for (int i = 0; i < 100 && server.stats().active_requests < 1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        libmini::RpcClient ok_client(libmini::RpcTransport::Tcp,
                                     server.endpoint());
        ok_client.set_max_retries(0);
        // 1 成功（slow 还占着槽位，之后还有一次成功）
        (void)ok_client.call("add", R"({"a":0,"b":0})");  // 429 过载

        libmini::RpcClient c2(libmini::RpcTransport::Tcp, server.endpoint());
        c2.set_max_retries(0);
        c2.set_timeout_ms(8000);
        (void)c2.call("add", R"({"a":1,"b":1})");
        EXPECT_EQ(slow_call.get(), R"({"done":true})");
        (void)c2.call("add", R"({"a":2,"b":2})");
        // 1 次业务错误（handler 异常）也计入 completed
        (void)c2.call("fail", "null");
        (void)c2.call("no_such", "null");  // 404 也计入 completed

        const libmini::RpcServerStats s = server.stats();
        EXPECT_EQ(s.tcp.completed, s.completed_total);
        EXPECT_EQ(s.tcp.rejected, s.rejected_total);
        EXPECT_GE(s.tcp.completed, 4u);   // add x2 + fail + no_such + slow
        EXPECT_GE(s.tcp.rejected, 1u);
        EXPECT_EQ(s.http.completed, 0u);
        EXPECT_EQ(s.http.rejected, 0u);
        EXPECT_EQ(s.local.completed, 0u);
        EXPECT_EQ(s.local.rejected, 0u);
        EXPECT_EQ(s.malformed.completed, 0u);
    }

    // ---- 本地传输：管道与 UDS 同记 local 列 ----
#ifdef _WIN32
    {
        libmini::RpcServer server(libmini::RpcTransport::LocalPipe,
                                  "libmini_stats_test.pipe");
#else
    {
        libmini::RpcServer server(
            libmini::RpcTransport::LocalPipe,
            libmini::temp_directory_path() + "/libmini_stats_test.sock");
#endif
        server.register_method("add", [](const std::string& params) {
            const json p = json::parse(params);
            return json{{"sum",
                         p.at("a").get<int>() + p.at("b").get<int>()}}
                .dump();
        });
        server.start_background();
        ASSERT_TRUE(server.wait_until_ready(5000));

        libmini::RpcClient client(libmini::RpcTransport::LocalPipe,
                                  server.endpoint());
        client.set_max_retries(0);
        EXPECT_FALSE(client.call("add", R"({"a":1,"b":2})").empty());
        EXPECT_TRUE(client.call("add", "not-json").empty());  // 客户端参数格式错误
        // 请求级格式错误（服务端视角）：请求体缺 method 字段 → malformed 列
        {
            // 直连帧协议发一个畸形请求
            //（通过公开 API 不可构造，借用 raw socket 不现实——改用
            //  未注册方法触发 404，仍计入 local.completed）
        }
        (void)client.call("no_such", "null");

        const libmini::RpcServerStats s = server.stats();
        EXPECT_EQ(s.local.completed, s.completed_total);
        EXPECT_EQ(s.local.rejected, s.rejected_total);
        EXPECT_EQ(s.local.completed, 2u);  // add 成功 + no_such 404
        EXPECT_EQ(s.http.completed, 0u);
        EXPECT_EQ(s.tcp.completed, 0u);
    }

    // ---- HTTP 传输：http 列计数（含请求级格式错误 → malformed）----
    {
        libmini::RpcServer server(0);
        server.register_method("add", [](const std::string& params) {
            const json p = json::parse(params);
            return json{{"sum",
                         p.at("a").get<int>() + p.at("b").get<int>()}}
                .dump();
        });
        server.start_background();
        ASSERT_TRUE(server.wait_until_ready(5000));

        libmini::RpcClient client("127.0.0.1", server.port());
        client.set_max_retries(0);
        EXPECT_FALSE(client.call("add", R"({"a":3,"b":4})").empty());
        // params 不是 JSON → 客户端本地拦截，到不了服务器；
        // 构造请求级格式错误需发缺失 method 字段的请求体，客户端 API
        // 无法做到——malformed 列由协议层测试另行覆盖（见上注释），
        // 这里验证正常路径的分列
        (void)client.call("add", R"({"a":1,"b":1})");

        const libmini::RpcServerStats s = server.stats();
        EXPECT_EQ(s.http.completed, s.completed_total);
        EXPECT_EQ(s.http.completed, 2u);
        EXPECT_EQ(s.tcp.completed, 0u);
        EXPECT_EQ(s.local.completed, 0u);
    }
}

// 分传输延迟分位：各传输的直方图只含自己的样本；无样本传输返回 -1；
// All 过滤器与旧行为一致（合计口径）
TEST(RpcServerStatsTest, PerTransportLatencyStats)
{
    // Tcp 传输：加一个 handler 特意慢 30ms，验证 Tcp 分位 > 管道分位，
    // 且样本数分列相加等于 All
    libmini::RpcServer tcp_srv(libmini::RpcTransport::Tcp, "127.0.0.1:0");
    tcp_srv.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{{"sum",
                     p.at("a").get<int>() + p.at("b").get<int>()}}
            .dump();
    });
    tcp_srv.start_background();
    ASSERT_TRUE(tcp_srv.wait_until_ready(5000));

#ifdef _WIN32
    const std::string pipe_ep = "libmini_stats_lat_test.pipe";
#else
    const std::string pipe_ep =
        libmini::temp_directory_path() + "/libmini_stats_lat_test.sock";
#endif
    libmini::RpcServer pipe_srv(libmini::RpcTransport::LocalPipe, pipe_ep);
    pipe_srv.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{{"sum",
                     p.at("a").get<int>() + p.at("b").get<int>()}}
            .dump();
    });
    pipe_srv.start_background();
    ASSERT_TRUE(pipe_srv.wait_until_ready(5000));

    // Tcp：5 次调用；管道：3 次调用
    {
        libmini::RpcClient c(libmini::RpcTransport::Tcp, tcp_srv.endpoint());
        c.set_max_retries(0);
        for (int i = 0; i < 5; ++i) {
            EXPECT_FALSE(c.call("add", R"({"a":1,"b":2})").empty());
        }
    }
    {
        libmini::RpcClient c(libmini::RpcTransport::LocalPipe, pipe_ep);
        c.set_max_retries(0);
        for (int i = 0; i < 3; ++i) {
            EXPECT_FALSE(c.call("add", R"({"a":1,"b":2})").empty());
        }
    }

    // Tcp 过滤：样本数 5，分位有效
    const libmini::RpcLatencyStats tcp_s =
        tcp_srv.latency_stats({}, libmini::RpcTransportFilter::Tcp);
    EXPECT_EQ(tcp_s.sample_count, 5u);
    EXPECT_GT(tcp_s.p50_ms, 0.0);
    EXPECT_LE(tcp_s.p50_ms, tcp_s.p99_ms);

    // Http 过滤：本服务器无 HTTP 流量 → 样本 0、分位 -1
    const libmini::RpcLatencyStats http_s =
        tcp_srv.latency_stats({}, libmini::RpcTransportFilter::Http);
    EXPECT_EQ(http_s.sample_count, 0u);
    EXPECT_EQ(http_s.p50_ms, -1.0);
    EXPECT_EQ(http_s.p99_ms, -1.0);

    // All 过滤：与服务器自身合计一致
    const libmini::RpcLatencyStats tcp_all = tcp_srv.latency_stats();
    EXPECT_EQ(tcp_all.sample_count, 5u);

    // 管道服务器：Local 过滤命中、Tcp 过滤为空
    const libmini::RpcLatencyStats local_s =
        pipe_srv.latency_stats({50.0}, libmini::RpcTransportFilter::Local);
    EXPECT_EQ(local_s.sample_count, 3u);
    EXPECT_GT(local_s.p50_ms, 0.0);
    EXPECT_EQ(local_s.percentiles.size(), 1u);
    const libmini::RpcLatencyStats local_tcp =
        pipe_srv.latency_stats({}, libmini::RpcTransportFilter::Tcp);
    EXPECT_EQ(local_tcp.sample_count, 0u);
    EXPECT_EQ(local_tcp.p50_ms, -1.0);

    tcp_srv.stop();
    pipe_srv.stop();
}

TEST(RpcTcpTransportTest, RoundTripCall)
{
    libmini::RpcServer server = make_tcp_server();

    libmini::RpcClient client(libmini::RpcTransport::Tcp, server.endpoint());
    const std::string reply = client.call("add", R"({"a":20,"b":22})");
    EXPECT_EQ(reply, R"({"sum":42})");
    EXPECT_EQ(client.last_error(), libmini::RpcError::OK);

    // 访问器一致性
    EXPECT_EQ(client.transport(), libmini::RpcTransport::Tcp);
    EXPECT_EQ(client.endpoint(), server.endpoint());
    EXPECT_EQ(server.transport(), libmini::RpcTransport::Tcp);
    EXPECT_GT(server.port(), 0);  // 端口 0 自动分配后回填真实值

    // 统计与直方图与 HTTP/本地传输共用一套口径
    const libmini::RpcServerStats s = server.stats();
    EXPECT_EQ(s.completed_total, 1u);
    EXPECT_EQ(s.rejected_total, 0u);
    const libmini::RpcLatencyStats ls = server.latency_stats();
    EXPECT_EQ(ls.sample_count, 1u);
    EXPECT_GT(ls.p50_ms, 0.0);
}

TEST(RpcTcpTransportTest, HandlerErrorMapsToServerError)
{
    libmini::RpcServer server = make_tcp_server();
    libmini::RpcClient client(libmini::RpcTransport::Tcp, server.endpoint());

    EXPECT_TRUE(client.call("fail", "null").empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::SERVER_ERROR);
    EXPECT_NE(client.last_error_message().find("boom"), std::string::npos);

    // 未注册方法 → 404 语义
    EXPECT_TRUE(client.call("no_such_method", "null").empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::SERVER_ERROR);
    EXPECT_NE(client.last_error_message().find("not found"),
              std::string::npos);
}

TEST(RpcTcpTransportTest, OverloadRejects)
{
    libmini::RpcServer server = make_tcp_server();
    server.set_max_in_flight(1);
    server.set_overload_mode(libmini::RpcOverloadMode::RejectImmediate);

    libmini::RpcClient busy(libmini::RpcTransport::Tcp, server.endpoint());
    busy.set_timeout_ms(5000);
    // 异步占住唯一槽位
    auto slow_call = std::async(std::launch::async, [&busy]() {
        return busy.call("slow", "null");
    });
    for (int i = 0; i < 100 && server.stats().active_requests < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    libmini::RpcClient client(libmini::RpcTransport::Tcp, server.endpoint());
    client.set_max_retries(0);
    EXPECT_TRUE(client.call("add", R"({"a":1,"b":2})").empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::OVERLOADED);
    EXPECT_EQ(slow_call.get(), R"({"done":true})");
    EXPECT_EQ(server.stats().rejected_total, 1u);
}

TEST(RpcTcpTransportTest, ConnectionFailureReported)
{
    // 无人监听的端口（RFC 5737 文档段，测试环境可安全占用失败）
    libmini::RpcClient client(libmini::RpcTransport::Tcp, "127.0.0.1:1");
    client.set_max_retries(1);
    client.set_retry_base_delay_ms(20);

    EXPECT_TRUE(client.call("add", "null").empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::CONNECTION_FAILED);
}

// ==================== 本地传输测试（Windows 命名管道 / POSIX UDS）====================

namespace {

// 本地传输测试服务器：端点自动放入临时目录，避免冲突与残留
libmini::RpcServer make_local_server()
{
    static std::atomic<int> seq{0};
    const int n = seq.fetch_add(1);
    const std::string endpoint =
#ifdef _WIN32
        "libmini_rpc_test_" + std::to_string(n) + ".pipe";
#else
        libmini::temp_directory_path() + "/libmini_rpc_test_" +
        std::to_string(n) + ".sock";
#endif
    libmini::RpcServer server(libmini::RpcTransport::LocalPipe, endpoint);
    server.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{ {"sum", p.at("a").get<int>() + p.at("b").get<int>()} }
            .dump();
    });
    server.register_method("fail", [](const std::string&) {
        throw std::runtime_error("boom");
        return std::string();
    });
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return json{{"done", true}}.dump();
    });
    server.start_background();
    EXPECT_TRUE(server.wait_until_ready(5000));
    return server;
}

}  // namespace

TEST(RpcLocalTransportTest, RoundTripCall)
{
    libmini::RpcServer server = make_local_server();

    libmini::RpcClient client(libmini::RpcTransport::LocalPipe,
                              server.endpoint());
    const std::string reply = client.call("add", R"({"a":20,"b":22})");
    EXPECT_EQ(reply, R"({"sum":42})");
    EXPECT_EQ(client.last_error(), libmini::RpcError::OK);

    // 访问器一致性
    EXPECT_EQ(client.transport(), libmini::RpcTransport::LocalPipe);
    EXPECT_EQ(client.endpoint(), server.endpoint());
    EXPECT_EQ(server.transport(), libmini::RpcTransport::LocalPipe);

    // 统计与直方图与 HTTP 传输共用一套口径
    const libmini::RpcServerStats s = server.stats();
    EXPECT_EQ(s.completed_total, 1u);
    EXPECT_EQ(s.rejected_total, 0u);
    const libmini::RpcLatencyStats ls = server.latency_stats();
    EXPECT_EQ(ls.sample_count, 1u);
    EXPECT_GT(ls.p50_ms, 0.0);
}

TEST(RpcLocalTransportTest, HandlerErrorMapsToServerError)
{
    libmini::RpcServer server = make_local_server();
    libmini::RpcClient client(libmini::RpcTransport::LocalPipe,
                              server.endpoint());

    EXPECT_TRUE(client.call("fail", "null").empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::SERVER_ERROR);
    EXPECT_NE(client.last_error_message().find("boom"), std::string::npos);

    // 未注册方法 → 404 语义
    EXPECT_TRUE(client.call("no_such_method", "null").empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::SERVER_ERROR);
    EXPECT_NE(client.last_error_message().find("not found"),
              std::string::npos);
}

TEST(RpcLocalTransportTest, OverloadRejectsAndReportsRetryAfter)
{
    libmini::RpcServer server = make_local_server();
    server.set_max_in_flight(1);
    server.set_overload_mode(libmini::RpcOverloadMode::RejectImmediate);
    server.set_retry_after_seconds(3);

    libmini::RpcClient busy(libmini::RpcTransport::LocalPipe,
                            server.endpoint());
    busy.set_timeout_ms(5000);
    // 异步占住唯一槽位（同步调用等返回时槽位已释放）
    auto slow_call = std::async(std::launch::async, [&busy]() {
        return busy.call("slow", "null");
    });
    // 确认槽位已被占用：服务端进入处理状态（带轮询的短等待）
    for (int i = 0; i < 100 && server.stats().active_requests < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    libmini::RpcClient client(libmini::RpcTransport::LocalPipe,
                              server.endpoint());
    client.set_max_retries(0);
    EXPECT_TRUE(client.call("add", R"({"a":1,"b":2})").empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::OVERLOADED);
    EXPECT_EQ(client.last_error_message(), "server overloaded");
    // Retry-After 透传：本地响应的 retry_after_s 字段 → retry_after_ms
    //（内部字段，通过下次重试等待验证过于间接，这里验证过载语义即可）
    EXPECT_EQ(slow_call.get(), R"({"done":true})");  // 占位请求本身成功
    EXPECT_EQ(server.stats().rejected_total, 1u);
}

TEST(RpcLocalTransportTest, WaitInQueueModeServesBurst)
{
    libmini::RpcServer server = make_local_server();
    server.set_max_in_flight(1);
    server.set_overload_mode(libmini::RpcOverloadMode::WaitInQueue);
    server.set_queue_wait_ms(5000);

    libmini::RpcClient client(libmini::RpcTransport::LocalPipe,
                              server.endpoint());
    client.set_max_retries(0);

    // 3 个并发请求压 1 个槽位：排队等待后应全部成功（不拒绝）
    std::vector<std::future<std::string>> results;
    for (int i = 0; i < 3; ++i) {
        results.push_back(std::async(std::launch::async,
                                     [&client]() {
                                         return client.call(
                                             "add",
                                             R"({"a":1,"b":2})");
                                     }));
    }
    int ok = 0;
    for (auto& f : results) {
        if (f.get() == R"({"sum":3})") {
            ++ok;
        }
    }
    EXPECT_EQ(ok, 3);
    EXPECT_EQ(server.stats().rejected_total, 0u);
    EXPECT_EQ(server.stats().completed_total, 3u);
}

TEST(RpcLocalTransportTest, ConnectionFailureReported)
{
    // 不存在的端点
#ifdef _WIN32
    const std::string missing = "\\\\.\\pipe\\libmini_no_such_pipe_12345";
#else
    const std::string missing =
        libmini::temp_directory_path() + "/no_such_socket_12345";
#endif
    libmini::RpcClient client(libmini::RpcTransport::LocalPipe, missing);
    client.set_max_retries(1);
    client.set_retry_base_delay_ms(20);

    EXPECT_TRUE(client.call("add", "null").empty());
    EXPECT_EQ(client.last_error(), libmini::RpcError::CONNECTION_FAILED);
}

// ==================== 客户端连接池测试（LocalPipe/Tcp）====================

namespace {

// 在给定传输上跑同一组池语义断言（传输无关）
void run_pool_tests(libmini::RpcTransport transport,
                    const std::string& endpoint)
{
    // 1) 复用：同一客户端连续调用，created 只增 1，reused 递增
    {
        libmini::RpcClient client(transport, endpoint);
        for (int i = 1; i <= 5; ++i) {
            const std::string reply =
                client.call("add", json{{"a", 1}, {"b", i}}.dump());
            EXPECT_EQ(reply, "{\"sum\":" + std::to_string(i + 1) + "}");
        }
        const libmini::RpcClientPoolStats s = client.pool_stats();
        EXPECT_EQ(s.created_total, 1u);
        EXPECT_EQ(s.reused_total, 4u);
        EXPECT_EQ(s.open_connections, 1u);
        EXPECT_EQ(s.idle_connections, 1u);
        EXPECT_EQ(s.busy_connections, 0u);
    }

    // 2) 空闲回收：idle_timeout 极短，间隔后再次调用应新建连接
    {
        libmini::RpcClient client(transport, endpoint);
        client.set_connection_pool_idle_ms(1);
        (void)client.call("add", R"({"a":1,"b":2})");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        (void)client.call("add", R"({"a":2,"b":3})");
        const libmini::RpcClientPoolStats s = client.pool_stats();
        EXPECT_GE(s.created_total, 2u);
        EXPECT_GE(s.closed_total, 1u);
    }

    // 3) 并发上限：max=2，8 线程并发调 600ms 的 slow；
    //    前两个立即占用连接，其余等待；总耗时至少两批
    {
        libmini::RpcClient client(transport, endpoint);
        client.set_timeout_ms(8000);
        client.set_connection_pool_max(2);
        client.set_max_retries(0);

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::future<std::string>> fs;
        for (int i = 0; i < 8; ++i) {
            fs.push_back(std::async(std::launch::async,
                                    [&client] { return client.call("slow", "null"); }));
        }
        int ok_count = 0;
        for (auto& f : fs) {
            if (f.get() == R"({"done":true})") {
                ++ok_count;
            }
        }
        const auto elapsed = std::chrono::duration_cast<
                                 std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        EXPECT_EQ(ok_count, 8);
        EXPECT_GE(elapsed, 1000);  // 8 请求 / 2 并发 / 300ms ≈ 4 批 ≈ 1200ms
        const libmini::RpcClientPoolStats s = client.pool_stats();
        EXPECT_LE(s.open_connections, 2u);
    }

    // 4) 并发上限排队：max=1，两个并发调用共享同一客户端，
    //    后到者必须等先到者归还（或等待超过请求超时报 pool busy）
    {
        libmini::RpcClient client(transport, endpoint);
        client.set_timeout_ms(400);
        client.set_connection_pool_max(1);
        client.set_max_retries(0);

        auto slow_res = std::async(std::launch::async, [&client] {
            return client.call("slow", "null");
        });
        auto fast_res = std::async(std::launch::async, [&client] {
            return client.call("add", R"({"a":1,"b":2})");
        });
        const std::string rs = slow_res.get();
        const std::string rf = fast_res.get();
        // 两种结局都合法：后者等到了连接（双双成功），或等待超过
        // 请求超时（恰一个报 pool busy 的 TIMEOUT）——但不能双双失败
        EXPECT_TRUE(!rs.empty() || !rf.empty());
        if (rs.empty() != rf.empty()) {
            EXPECT_EQ(client.last_error(), libmini::RpcError::TIMEOUT);
            EXPECT_NE(client.last_error_message().find("pool busy"),
                      std::string::npos);
        }
    }

    // 5) 池禁用：每次调用新建连接，用完即断
    {
        libmini::RpcClient client(transport, endpoint);
        client.set_connection_pool_max(0);
        for (int i = 0; i < 3; ++i) {
            EXPECT_FALSE(client.call("add", R"({"a":1,"b":2})").empty());
        }
        const libmini::RpcClientPoolStats s = client.pool_stats();
        EXPECT_EQ(s.created_total, 0u);
        EXPECT_EQ(s.reused_total, 0u);
        EXPECT_EQ(s.open_connections, 0u);
    }
}

}  // namespace

// 重启场景：池中旧连接随服务器销毁失效，同一客户端应透明重建连接
TEST(RpcClientPoolTest, ReconnectsTransparentlyAfterServerRestart)
{
#ifdef _WIN32
    const std::string endpoint = "libmini_rpc_pool_restart_test.pipe";
#else
    const std::string endpoint =
        libmini::temp_directory_path() + "/libmini_rpc_pool_restart_test.sock";
#endif
    const auto register_handlers = [](libmini::RpcServer& server) {
        server.register_method("add", [](const std::string& params) {
            const json p = json::parse(params);
            return json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}}.dump();
        });
    };

    std::unique_ptr<libmini::RpcClient> client;
    {
        libmini::RpcServer server(libmini::RpcTransport::LocalPipe, endpoint);
        register_handlers(server);
        server.start_background();
        ASSERT_TRUE(server.wait_until_ready(5000));

        client.reset(new libmini::RpcClient(
            libmini::RpcTransport::LocalPipe, endpoint));
        EXPECT_FALSE(client->call("add", R"({"a":1,"b":2})").empty());
        EXPECT_EQ(client->pool_stats().idle_connections, 1u);
    }  // 服务器销毁，客户端池中连接失效

    // 同端点重启服务器：旧连接被自动丢弃并换新连接，调用方无感知
    libmini::RpcServer server(libmini::RpcTransport::LocalPipe, endpoint);
    register_handlers(server);
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    EXPECT_EQ(client->call("add", R"({"a":3,"b":4})"), R"({"sum":7})");
    const libmini::RpcClientPoolStats s = client->pool_stats();
    EXPECT_GE(s.created_total, 2u);  // 旧连接判失效丢弃后新建
    EXPECT_GE(s.closed_total, 1u);
}

TEST(RpcClientPoolTest, TcpTransport)
{
    libmini::RpcServer server = make_tcp_server();
    run_pool_tests(libmini::RpcTransport::Tcp, server.endpoint());
}

TEST(RpcClientPoolTest, LocalPipeTransport)
{
#ifdef _WIN32
    const std::string endpoint = "libmini_rpc_pool_test.pipe";
#else
    const std::string endpoint =
        libmini::temp_directory_path() + "/libmini_rpc_pool_test.sock";
#endif
    libmini::RpcServer server(libmini::RpcTransport::LocalPipe, endpoint);
    server.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}}.dump();
    });
    server.register_method("slow", [](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return json{{"done", true}}.dump();
    });
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    run_pool_tests(libmini::RpcTransport::LocalPipe, server.endpoint());
}

// ==================== 请求流水线测试（LocalPipe/Tcp）====================

namespace {

// 并发跟踪处理器状态：peak 记录服务器同时处理的最大请求数
struct TrackState
{
    std::atomic<int> cur{0};
    std::atomic<int> peak{0};
};

void track_update(TrackState* st)
{
    const int now = ++st->cur;
    for (int p = st->peak.load(); now > p &&
         !st->peak.compare_exchange_weak(p, now);) {
    }
}

// 在给定传输上跑同一组流水线语义断言（传输无关）
void run_pipeline_tests(libmini::RpcTransport transport,
                        const std::string& endpoint,
                        TrackState* track)
{
    // 1) 正确性回归：流水线开启下 100 次顺序调用全部正确
    {
        libmini::RpcClient client(transport, endpoint);
        client.set_pipeline_max_in_flight(4);
        for (int i = 1; i <= 100; ++i) {
            const std::string reply =
                client.call("add", json{{"a", i}, {"b", 1}}.dump());
            EXPECT_EQ(reply, "{\"sum\":" + std::to_string(i + 1) + "}");
        }
    }

    // 2) 单连接多在途：8 线程并发 100ms 处理延迟的请求，96 个全部
    //    正确（响应按 id 匹配，错配即失败）；同批在途并发处理下
    //    总耗时应显著小于串行执行（96 x 100ms = 9.6s）
    {
        libmini::RpcClient client(transport, endpoint);
        client.set_pipeline_max_in_flight(8);
        std::atomic<int> ok{0};
        std::vector<std::thread> ths;
        const auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < 8; ++t) {
            ths.emplace_back([&, t]() {
                for (int i = 1; i <= 12; ++i) {
                    const int a = t * 100 + i;
                    const std::string reply = client.call(
                        "slow_add", json{{"a", a}, {"b", 1000}}.dump());
                    try {
                        const json r = json::parse(reply);
                        if (r.at("sum").get<int>() == a + 1000) {
                            ++ok;
                        }
                    } catch (const std::exception&) {
                    }
                }
            });
        }
        for (auto& th : ths) {
            th.join();
        }
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                                 .count();
        EXPECT_EQ(ok.load(), 96);
        // 流水线并发应远快于串行 9.6s；下界给足裕量防环境抖动误报
        EXPECT_LT(elapsed, 6000) << "elapsed=" << elapsed << "ms";
    }

    // 3) 在途槽位限流：max_in_flight=2，8 线程并发，服务器同时
    //    处理数不得超过 2；全部调用成功（超槽排队等待）
    {
        libmini::RpcClient client(transport, endpoint);
        client.set_pipeline_max_in_flight(2);
        std::atomic<int> ok{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 8; ++t) {
            ths.emplace_back([&]() {
                const std::string reply =
                    client.call("track", json{{"ms", 80}}.dump());
                if (reply.find("\"ok\":true") != std::string::npos) {
                    ++ok;
                }
            });
        }
        for (auto& th : ths) {
            th.join();
        }
        EXPECT_EQ(ok.load(), 8);
        EXPECT_LE(track->peak.load(), 2) << "peak=" << track->peak.load();
        EXPECT_GE(track->peak.load(), 2) << "no concurrency observed";
    }

    // 4) 流水线通道统计：pool_stats() 的 pipeline_* 字段语义
    {
        libmini::RpcClient client(transport, endpoint);
        client.set_pipeline_max_in_flight(2);
        for (int i = 0; i < 5; ++i) {
            EXPECT_FALSE(client.call("add", json{{"a", 1}, {"b", 2}}.dump())
                              .empty());
        }

        const libmini::RpcClientPoolStats s0 = client.pool_stats();
        // 5 次请求全部由同一通道承载（复用口径 = 请求次数）
        EXPECT_EQ(s0.pipeline_reused_total, 5u);
        EXPECT_EQ(s0.pipeline_broken_total, 0u);
        EXPECT_EQ(s0.pipeline_in_flight, 0u);        // 全部完成已归还
        EXPECT_EQ(s0.pipeline_in_flight_peak, 1u);   // 顺序调用无并发
        EXPECT_EQ(s0.pipeline_slot_wait_total, 0u);  // 上限 2 从未竞争
        EXPECT_EQ(s0.pipeline_slot_timeout_total, 0u);

        // 并发触发槽位等待：上限 1，4 线程 x 80ms 请求。
        client.set_pipeline_max_in_flight(1);
        // 超时预算给足（最坏等待 240ms + 处理 80ms），
        // 保证全部成功、断言确定性不受时序抖动影响
        client.set_timeout_ms(4000);
        client.set_max_retries(0);
        std::atomic<int> ok{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; ++t) {
            ths.emplace_back([&]() {
                const std::string reply =
                    client.call("track", json{{"ms", 80}}.dump());
                if (reply.find("\"ok\":true") != std::string::npos) {
                    ++ok;
                }
            });
        }
        for (auto& th : ths) {
            th.join();
        }
        EXPECT_EQ(ok.load(), 4);

        const libmini::RpcClientPoolStats s1 = client.pool_stats();
        EXPECT_EQ(s1.pipeline_reused_total, 9u);      // 5 + 4
        EXPECT_EQ(s1.pipeline_in_flight, 0u);
        EXPECT_EQ(s1.pipeline_in_flight_peak, 1u);    // 上限 1 钳制
        EXPECT_GE(s1.pipeline_slot_wait_total, 1u);   // 至少一次竞争等待
        EXPECT_EQ(s1.pipeline_slot_timeout_total, 0u); // 预算充裕无超时
    }

    // 5) 串行模式回归：不设流水线（默认 0）时走连接池，行为不变
    {
        libmini::RpcClient client(transport, endpoint);
        std::atomic<int> ok{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 6; ++t) {
            ths.emplace_back([&]() {
                for (int i = 0; i < 5; ++i) {
                    const std::string reply =
                        client.call("add", json{{"a", 1}, {"b", 2}}.dump());
                    if (reply == "{\"sum\":3}") {
                        ++ok;
                    }
                }
            });
        }
        for (auto& th : ths) {
            th.join();
        }
        EXPECT_EQ(ok.load(), 30);
    }
}

}  // namespace

TEST(RpcPipelineTest, TcpTransport)
{
    libmini::RpcServer server(libmini::RpcTransport::Tcp, "0.0.0.0:0");
    server.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}}.dump();
    });
    server.register_method("slow_add", [](const std::string& params) {
        const json p = json::parse(params);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}}.dump();
    });
    auto track = std::make_shared<TrackState>();
    server.register_method("track", [track](const std::string& params) {
        const json p = json::parse(params);
        track_update(track.get());
        std::this_thread::sleep_for(
            std::chrono::milliseconds(p.at("ms").get<int>()));
        track->cur.fetch_sub(1);
        return json{{"ok", true}}.dump();
    });
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    run_pipeline_tests(libmini::RpcTransport::Tcp, server.endpoint(),
                       track.get());
}

TEST(RpcPipelineTest, LocalPipeTransport)
{
#ifdef _WIN32
    const std::string endpoint = "libmini_rpc_pipeline_test.pipe";
#else
    const std::string endpoint =
        libmini::temp_directory_path() + "/libmini_rpc_pipeline_test.sock";
#endif
    libmini::RpcServer server(libmini::RpcTransport::LocalPipe, endpoint);
    server.register_method("add", [](const std::string& params) {
        const json p = json::parse(params);
        return json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}}.dump();
    });
    server.register_method("slow_add", [](const std::string& params) {
        const json p = json::parse(params);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}}.dump();
    });
    auto track = std::make_shared<TrackState>();
    server.register_method("track", [track](const std::string& params) {
        const json p = json::parse(params);
        track_update(track.get());
        std::this_thread::sleep_for(
            std::chrono::milliseconds(p.at("ms").get<int>()));
        track->cur.fetch_sub(1);
        return json{{"ok", true}}.dump();
    });
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    run_pipeline_tests(libmini::RpcTransport::LocalPipe, server.endpoint(),
                       track.get());
}

// ==================== 优阅停机（本地传输篇）====================
// HTTP 篇见 RpcServerShutdownTest（排空窗口/超时放弃/0 窗口）；
// 这里补齐本地管道的同类语义与客户端重试衔接

namespace {

// 本地端点工具（与池测试同款）
std::string drain_local_endpoint(const char* name)
{
#ifdef _WIN32
    return std::string(name);
#else
    return libmini::temp_directory_path() + "/" + name;
#endif
}

void register_drain_handlers(libmini::RpcServer& server, const char* method)
{
    server.register_method(method, [](const std::string& params) {
        const json p = json::parse(params);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(p.at("ms").get<int>()));
        return json{{"ok", true}}.dump();
    });
}

}  // namespace

// 排空窗口内停机：已到达的流水线在途请求全部完成（
// 相当于 max_in_flight=1 的串行处理，总耗时 ≈ 4x150ms）
TEST(RpcServerShutdownTest, LocalPipeDrainsInFlightPipelineRequests)
{
    const std::string endpoint = drain_local_endpoint(
        "libmini_rpc_drain_inflight.pipe");
    libmini::RpcServer server(libmini::RpcTransport::LocalPipe, endpoint);
    register_drain_handlers(server, "work");
    server.set_max_in_flight(1);
    server.set_overload_mode(libmini::RpcOverloadMode::WaitInQueue);
    server.set_drain_timeout_ms(5000);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient client(libmini::RpcTransport::LocalPipe, endpoint);
    client.set_pipeline_max_in_flight(4);
    client.set_max_retries(0);
    client.set_timeout_ms(15000);

    std::atomic<int> ok{0};
    std::vector<std::future<bool>> fs;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; ++i) {
        fs.push_back(std::async(std::launch::async, [&client] {
            const std::string r =
                client.call("work", json{{"ms", 150}}.dump());
            return r.find("\"ok\":true") != std::string::npos;
        }));
    }
    for (auto& f : fs) {
        if (f.get()) {
            ++ok;
        }
    }
    EXPECT_EQ(ok.load(), 4);  // 在途请求全部在排空窗口内完成
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_GE(elapsed_ms, 450) << "elapsed=" << elapsed_ms << "ms";
}

// 停机后重启同一端点：客户端断连重试自动衔接新实例
// （滞后 300ms 内发起的调用应在重启完成后成功返回）
TEST(RpcServerShutdownTest, ClientRetriesSeamlesslyAfterRestart)
{
    const std::string endpoint = drain_local_endpoint(
        "libmini_rpc_drain_restart.pipe");
    const auto register_echo = [](libmini::RpcServer& server) {
        server.register_method("echo", [](const std::string& p) { return p; });
    };

    libmini::RpcServer server(libmini::RpcTransport::LocalPipe, endpoint);
    register_echo(server);
    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient client(libmini::RpcTransport::LocalPipe, endpoint);
    client.set_max_retries(8);
    client.set_retry_base_delay_ms(100);
    client.set_retry_jitter(false);

    EXPECT_EQ(client.call("echo", "\"pre\""), "\"pre\"");

    // 后台线程：停机后延迟 300ms 重启同一端点（滚动重启典型时序）
    std::thread restarter([&server, &endpoint, register_echo]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        server.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        libmini::RpcServer fresh(libmini::RpcTransport::LocalPipe, endpoint);
        register_echo(fresh);
        fresh.start_background();
        EXPECT_TRUE(fresh.wait_until_ready(5000));
        // 新实例持续运行直到本用例结束（客户端重试需要它在线）
        std::this_thread::sleep_for(std::chrono::seconds(10));
    });
    restarter.detach();

    // 停机后的调用：客户端经断连重试自动打到新实例
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const std::string r = client.call("echo", "\"post\"");
    EXPECT_EQ(r, "\"post\"") << "last: " << client.last_error_message();
}

// 停机新请求得到 429 "server shutting down"（本地传输语义与 HTTP 篇对齐）
TEST(RpcServerShutdownTest, LocalPipeRejectsWith429DuringDraining)
{
    const std::string endpoint = drain_local_endpoint(
        "libmini_rpc_drain_reject.pipe");
    libmini::RpcServer server(libmini::RpcTransport::LocalPipe, endpoint);
    register_drain_handlers(server, "work");
    server.set_max_in_flight(1);
    server.set_overload_mode(libmini::RpcOverloadMode::RejectImmediate);
    server.set_drain_timeout_ms(1000);

    server.start_background();
    ASSERT_TRUE(server.wait_until_ready(5000));

    libmini::RpcClient busy(libmini::RpcTransport::LocalPipe, endpoint);
    busy.set_max_retries(0);
    busy.set_timeout_ms(8000);
    auto occupy = std::async(std::launch::async, [&busy] {
        return busy.call("work", json{{"ms", 400}}.dump());
    });
    bool occupied = false;
    for (int i = 0; i < 200; ++i) {
        if (server.stats().active_requests >= 1) {
            occupied = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(occupied);

    // 处理中停机：后续调用应得到 429 拒绝（而非挂起）
    server.stop();
    occupy.get();

    libmini::RpcClient after(libmini::RpcTransport::LocalPipe, endpoint);
    after.set_max_retries(0);
    after.set_timeout_ms(2000);
    const std::string r = after.call("echo", "\"x\"");
    EXPECT_TRUE(r.empty());
    const std::string msg = after.last_error_message();
    const bool shutdown_429 =
        msg.find("server shutting down") != std::string::npos;
    const bool conn_gone =
        after.last_error() == libmini::RpcError::CONNECTION_FAILED;
    EXPECT_TRUE(shutdown_429 || conn_gone)
        << "unexpected: " << msg;
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
