// 原生 UI 栈（native_ui / child_process）的库级测试。
//
// 这一层原先住在 apps/ 里，如今是库的正式模块（libmini::ui::Ui、libmini::ChildProcess），
// 因此它的回归也归库自己管：窗口自检不再只挂在示例应用上，库本体（含 shared 构建的
// 导出面）也直接验一遍。
//
// 窗口用例在「本平台没有后端」或「有后端但建不出窗口」（无交互桌面的 CI）时跳过，
// 与示例应用 --ui-selftest 的口径一致；ChildProcess 的用例在任何平台都真跑。

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "libmini.h"

using namespace libmini;

// ------------------------------- 窗口层 ---------------------------------

TEST(NativeUiTest, BackendIsReportedConsistently)
{
    const std::string backend = ui::Ui::backend();
    EXPECT_FALSE(backend.empty());
    // 有后端 ⇔ 后端名不是 headless，两者不能各说各话
    EXPECT_EQ(ui::Ui::available(), backend != "headless");
}

TEST(NativeUiTest, HeadlessPlatformsDegradeCleanly)
{
    if (ui::Ui::available()) {
        GTEST_SKIP() << "本平台有窗口后端，降级路径不适用";
    }
    ui::Ui window;
    std::string error;
    EXPECT_FALSE(window.open("libmini ui", 400, 300, &error));
    EXPECT_FALSE(error.empty());

    // 没有后端时 run() 是空转：返回 0、不渲染任何帧
    EXPECT_EQ(0, window.run([](ui::Ui&) {}));
    EXPECT_EQ(0, window.frames());

    std::string shot_error;
    EXPECT_FALSE(window.capture_bmp("no_backend.bmp", &shot_error));
    EXPECT_FALSE(shot_error.empty());
}

TEST(NativeUiTest, WindowRendersHitTestsAndCaptures)
{
    if (!ui::Ui::available()) {
        GTEST_SKIP() << "本平台没有原生窗口后端";
    }

    ui::Ui window;
    std::string error;
    if (!window.open("libmini ui test", 560, 420, &error)) {
        GTEST_SKIP() << "窗口建不出来（例如无交互桌面）: " << error;
    }

    const std::string shot = libmini::unique_temp_path("libmini_ui_shot_") + ".bmp";
    libmini::remove_file(shot);

    // 跑固定帧数、每帧几乎不等待，让自检在 CI 上秒级结束
    window.set_frame_limit(6);
    window.set_frame_interval_ms(1);

    int clicks = 0;
    bool click_injected = false;
    bool shot_requested = false;
    bool button_rect_seen = false;

    window.run([&](ui::Ui& u) {
        u.title("libmini ui test", "library module");
        // 只有库模块才有的接口：立即模式控件、主题、底栏状态
        if (u.button("点我")) {
            ++clicks;
        }
        const ui::Rect button = u.last_rect();
        button_rect_seen = button_rect_seen || (button.w > 0 && button.h > 0);
        if (!click_injected && button.w > 0 && button.h > 0) {
            // 合成点击在下一帧被按钮的命中测试消费
            u.simulate_click(button.center_x(), button.center_y());
            click_injected = true;
        }

        u.kv("后端", u.backend());
        u.badge("徽章", ui::theme::accent);
        u.bar("进度", 0.5, "50%");
        u.sparkline(std::vector<double>{1.0, 3.0, 2.0, 5.0}, "样本");
        u.log_view(std::vector<std::string>{"第一行", "第二行"}, 60);
        u.table(std::vector<std::string>{"列 A", "列 B"},
                std::vector<std::vector<std::string> >{{"a1", "b1"}, {"a2", "b2"}});
        u.status("就绪", ui::theme::ok);

        // 截图要在帧跑完之前登记（实现里留到帧结束后兑现），否则会拍到半帧
        if (!shot_requested && u.frames() + 2 >= 6) {
            shot_requested = u.capture_bmp(shot, 0);
        }
    });

    EXPECT_EQ(6, window.frames());
    EXPECT_TRUE(button_rect_seen) << "按钮应当拿到一个非空矩形（布局跑起来了）";
    EXPECT_TRUE(click_injected);
    EXPECT_EQ(1, clicks) << "合成点击应当被按钮消费";
    EXPECT_TRUE(shot_requested);
    EXPECT_TRUE(window.capture_ok()) << window.capture_error();

    // 截图确实是 BMP：开头两字节 "BM"，且长度与 32 位位图相当
    const std::string bytes = libmini::read_file(shot);
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ('B', bytes[0]);
    EXPECT_EQ('M', bytes[1]);
    const std::string::size_type header_bytes = 14 + 40;
    EXPECT_GT(bytes.size(), header_bytes);
    EXPECT_TRUE(libmini::remove_file(shot));
}

// ---------------------------- 常驻子进程 --------------------------------

TEST(ChildProcessTest, StreamsOutputAndReportsExitCode)
{
    ChildProcess child;
    std::string error;
    // 6 万字节 stdout + 4 万字节 stderr：远超管道缓冲区，验证父进程是边跑边排空
    ASSERT_TRUE(child.start(LIBMINI_PROCESS_CHILD_EXE, {"60000", "40000", "7"}, "", &error))
        << error;
    EXPECT_TRUE(child.started());
    EXPECT_GT(child.pid(), 0L);
    EXPECT_FALSE(child.command_line().empty());

    ASSERT_TRUE(child.wait(60000)) << "子进程没有在超时前结束";
    int code = ChildProcess::kNotExited;
    ASSERT_TRUE(child.try_finish(&code));
    EXPECT_EQ(7, code) << "退出码要原样透传";
    EXPECT_EQ(7, child.exit_code());
    EXPECT_FALSE(child.running());

    // stdout 与 stderr 合并到同一根管道，两边的内容都该在
    const std::string all = child.all_output();
    EXPECT_NE(std::string::npos, all.find(std::string(1000, 'O')));
    EXPECT_NE(std::string::npos, all.find(std::string(1000, 'E')));

    // take_output() 是增量的：第一次给全部，第二次就没东西了
    EXPECT_EQ(all, child.take_output());
    EXPECT_TRUE(child.take_output().empty());
    EXPECT_FALSE(child.lines(300).empty());

    // 已经结束后再启动应当被拒（同一个句柄只托管一个进程）
    std::string second_error;
    EXPECT_FALSE(child.start(LIBMINI_PROCESS_CHILD_EXE, {"0", "0", "0"}, "", &second_error));
    EXPECT_FALSE(second_error.empty());
}

TEST(ChildProcessTest, MoveTransfersProcessOwnership)
{
    ChildProcess holder;
    {
        ChildProcess original;
        std::string error;
        // 先按住 300ms 再写 2000 字节、以 5 退出
        ASSERT_TRUE(original.start(LIBMINI_PROCESS_CHILD_EXE, {"2000", "0", "5", "300"}, "",
                                   &error))
            << error;
        holder = std::move(original);
        // original 在这里析构：托管权已经交出去了，它不该把进程一起带走
    }
    EXPECT_TRUE(holder.started());

    ASSERT_TRUE(holder.wait(60000)) << "搬走后再析构原句柄，进程仍应活着并正常收尾";
    int code = ChildProcess::kNotExited;
    ASSERT_TRUE(holder.try_finish(&code));
    EXPECT_EQ(5, code);
    EXPECT_NE(std::string::npos, holder.all_output().find(std::string(2000, 'O')));
}

TEST(ChildProcessTest, KillsLongRunningChild)
{
    ChildProcess child;
    std::string error;
    // 不写任何输出，直接按住 30 秒：足够长，能确定接下来那次 kill() 杀的是活着的进程
    ASSERT_TRUE(child.start(LIBMINI_PROCESS_CHILD_EXE, {"0", "0", "0", "30000"}, "", &error))
        << error;
    EXPECT_TRUE(child.running());
    EXPECT_TRUE(child.kill());

    EXPECT_TRUE(child.wait(15000)) << "kill 之后应当很快退出";
    EXPECT_FALSE(child.running());
    int code = ChildProcess::kNotExited;
    EXPECT_TRUE(child.try_finish(&code));
    EXPECT_NE(0, code) << "被强杀的进程退出码不应是 0";
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
