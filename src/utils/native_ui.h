#ifndef LIBMINI_NATIVE_UI_H
#define LIBMINI_NATIVE_UI_H

// 原生 UI 层（库模块）：用系统 API（Windows: Win32 + GDI）画真正的桌面窗口，
// 零第三方依赖——不引入 Qt/ImGui 之类，只用到操作系统自带的 GDI。
//
// 设计取向是「立即模式」（immediate mode）：每帧顺序描述界面，框架负责绘制、
// 命中测试与滚动。没有控件树、没有回调注册、没有字符串 ID 查表——状态留在
// 你自己的结构体里，界面只是它的一次投影：
//
//   libmini::ui::Ui ui;
//   if (!ui.open("sysmon", 900, 620, &err)) { ... }      // 无后端则走无头路径
//   auto store = ...;                                    // 你自己的状态
//   ui.run([&](libmini::ui::Ui& u) {
//       u.title("sysmon", "本机观测台");
//       if (u.button("刷新")) { store.refresh(); }
//       if (u.every(1000)) { store.poll(); }             // 定时刷新（毫秒）
//       u.kv("CPU", fmt(store.cpu));
//       u.sparkline(store.cpu_history, "CPU %");
//   });
//
// 平台降级：非 Windows 平台 available() 为 false，open() 返回 false 并给出原因，
// run() 直接返回 0——调用方据此退回控制台/无头路径，三平台 CI 因此不必为 GUI
// 单独开路。头文件刻意不包含 windows.h，只有 .cpp 里有平台代码。
//
// 自检钩子：set_frame_limit() 让窗口跑满 N 帧后自动退出；simulate_click() 注入
// 合成点击；last_rect() 取上一个控件的矩形（用来知道该点哪里）；capture_bmp()
// 把客户区存成 BMP——这样「窗口真的画出来了、按钮真的响应了」可以在 CI 上断言，
// 而不是靠人眼。
//
// 配套模块：libmini::ChildProcess（utils/child_process.h）托管常驻子进程，
// libmini::ui::PanelWindow / run_app_window（utils/ui_panels.h）把「连 HTTP 控制面
// 的面板」与「子进程运行器」两种成品窗口拼好。三者合起来就是一整套零依赖 UI。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "export.h"

namespace libmini {
namespace ui {

// ---------------- 基础类型 ----------------

// 24 位色（与 GDI 的 COLORREF 无关，转换在实现里做）
struct Color
{
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
};

inline Color rgb(unsigned char r, unsigned char g, unsigned char b)
{
    Color c;
    c.r = r;
    c.g = g;
    c.b = b;
    return c;
}

// 深色主题（与各应用的网页控制台保持同一套配色）
namespace theme {

const Color bg = rgb(0x12, 0x14, 0x1a);        // 窗口底色
const Color panel = rgb(0x1a, 0x1e, 0x27);     // 面板底色
const Color header = rgb(0x22, 0x28, 0x36);    // 表头 / 标题条
const Color text = rgb(0xd8, 0xdb, 0xe6);      // 主文本
const Color dim = rgb(0x8b, 0x93, 0xa7);       // 次要文本
const Color faint = rgb(0x5a, 0x62, 0x74);     // 更弱的文本 / 网格
const Color accent = rgb(0x4a, 0xa8, 0xff);    // 强调 / 折线
const Color ok = rgb(0x3d, 0xdc, 0x84);        // 成功
const Color warn = rgb(0xff, 0xb4, 0x54);      // 警告
const Color err = rgb(0xff, 0x6b, 0x6b);       // 失败
const Color cursor = rgb(0x2b, 0x6c, 0xb0);    // 按钮悬停

}  // namespace theme

struct Rect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool contains(int px, int py) const
    {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    int center_x() const { return x + w / 2; }
    int center_y() const { return y + h / 2; }
};

// ---------------- 窗口 + 立即模式界面 ----------------

class LIBMINI_API Ui
{
public:
    Ui();
    ~Ui();

    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // 当前平台是否有原生窗口后端（Windows 有；其它平台为 false）
    static bool available();
    // 后端名："win32" / "headless"
    static const char* backend();

    // 打开窗口。失败（含无后端）返回 false 并填 error；此时应走无头路径。
    bool open(const std::string& title, int width, int height, std::string* error = 0);

    // 跑消息循环，每帧调用一次 frame。返回实际渲染的帧数（无后端返回 0）。
    // 未设帧上限时一直跑到窗口关闭。
    int run(const std::function<void(Ui&)>& frame);

    // 跑固定帧数后自动退出（自检用；0 = 不限制）
    void set_frame_limit(int frames);
    // 每帧毫秒数（默认 33 ≈ 30FPS）
    void set_frame_interval_ms(int ms);
    // 注入一次合成点击（自检用；屏幕客户区坐标）
    void simulate_click(int x, int y);

    // 当前帧号（从 0 起）；未运行时为 0
    int frames() const;
    // 是否收到关闭请求
    bool closing() const;
    // 请求关闭窗口（消息循环退出后 run 返回）
    void close();
    // 立即触发一次重绘（一般不必要，帧循环本来就在跑）
    void request_repaint();

    // ---------------- 界面描述 ----------------

    // 顶部标题 + 副标题（自带分隔线）
    void title(const std::string& text, const std::string& subtitle = std::string());
    // 分组标题
    void section(const std::string& text);
    // 普通文本
    void text(const std::string& value, Color color = theme::text);
    // 键值对（左侧弱色键、右侧主色值），自动对齐
    void kv(const std::string& key, const std::string& value, Color vc = theme::text);
    // 内联小标签（不换行，需在 row 内或自行换行）
    void badge(const std::string& value, Color c);

    // 按钮：返回「本帧是否被点击」。inline_button 在同一行继续排；
    // button 结束后自动换行。
    bool button(const std::string& label, bool enabled = true);
    bool inline_button(const std::string& label, bool enabled = true);
    // 一行里混排若干按钮：第 i 个被点击则返回 i（无则 -1）
    int button_row(const std::vector<std::string>& labels);

    // 表格：列宽比例（空 = 等分）。奇数行自动斑马纹；highlight 行高亮。
    // 返回被点击的行号（没点到返回 -1），行级交互（选中/双击语义由调用方决定）。
    int table(const std::vector<std::string>& headers,
              const std::vector<std::vector<std::string> >& rows,
              const std::vector<int>& weights = std::vector<int>(), int highlight = -1);
    // 折线图（自动缩放到数据范围），caption 画在左上
    void sparkline(const std::vector<double>& series, const std::string& caption,
                   int height = 56);
    // 进度/占比条（fraction 会被夹到 [0,1]）
    void bar(const std::string& caption, double fraction, const std::string& value_text,
             Color fill = theme::accent);
    // 日志视图：只显示能放下的最后几行（自动贴底）
    void log_view(const std::vector<std::string>& lines, int height);

    void gap(int pixels = 8);
    void separator();
    // 让下一个控件独占一行（在 inline_button/badge 之后用）
    void newline();

    // 底栏状态文本（窗口最底部，始终可见）
    void status(const std::string& value, Color c = theme::dim);

    // ---------------- 定时 / 自检辅助 ----------------

    // 每帧调用；距上次为真已过 ms 毫秒则返回 true（用于低频刷新，别每帧都发 HTTP）
    bool every(int ms);

    // 上一个控件（按钮/表格行/图）的屏幕矩形；没有则为 {0,0,0,0}
    Rect last_rect() const;

    // 请求把「完整一帧」存成 BMP（自检用）。
    // 注意时机：本函数通常是从帧回调里调用的，此时当前帧还没画完，因此这里只
    // 登记请求，真正的截图在帧结束后（或消息循环退出前）由框架完成。
    // 返回值仅表示「请求是否被接受」；结果用 capture_ok() / capture_error() 取。
    bool capture_bmp(const std::string& path, std::string* error = 0);
    // 上一次请求的截图是否已成功写出（无请求时 false）
    bool capture_ok() const;
    // 上一次请求的失败原因（成功或未请求时为空）
    std::string capture_error() const;
    // 客户区尺寸
    int width() const;
    int height() const;

private:
    struct Impl;
    Impl* impl_;

    // 把当前双缓冲位图写成 BMP（平台实现；无后端的平台返回 false）
    bool write_current_bitmap(const std::string& path, std::string* error);
};

}  // namespace ui
}  // namespace libmini

#endif  // LIBMINI_NATIVE_UI_H
