// 原生 UI 层实现（见 native_ui.h 的用法与设计说明）。库模块：只依赖 STL 与
// Win32/GDI（外加内部头 win_utf.h 做 UTF-8 → UTF-16 转换），不引入任何
// 第三方 GUI 依赖。
//
// 结构：Win32/GDI 相关代码全部收敛为 Impl 的几个成员函数（fill / text / line /
// polyline / ensure_offscreen / wnd_proc），控件与布局代码平台中立——非 Windows
// 编译时窗口部分整体消失，只剩布局计算与空实现，三平台 CI 不必为 GUI 单独开路。
//
// 为什么这些辅助都是 Impl 的成员：Ui::Impl 是私有嵌套类型，类外的自由函数无权
// 命名它，所以「平台原语 + 布局游标操作」都挂在 Impl 上（Ui 的方法可以访问）。

#include "native_ui.h"

#include <cstddef>
#include <cstring>

#include "libmini.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "win_utf.h"  // libmini::internal::utf8_to_wide

#endif  // _WIN32

namespace libmini {
namespace ui {

namespace {

// ---------------- 版式常量 ----------------

const int kMargin = 16;        // 内容区外边距
const int kColGap = 10;        // 行内控件间距
const int kBlockGap = 8;       // 纵向控件间距
const int kButtonH = 26;       // 按钮高
const int kButtonPadX = 14;    // 按钮文字左右留白
const int kLineH = 20;         // 单行文本高
const int kTitleH = 28;        // 大标题高
const int kSectionH = 24;      // 分组标题高
const int kTableHeaderH = 26;  // 表头高
const int kTableRowH = 22;     // 表格行高
const int kStatusH = 24;       // 底栏高
const int kBarH = 20;          // 进度条高
const int kKeyColW = 180;      // 键值对的键列宽

// GDI 没有透明度，斑马纹等直接用混好的实色
const Color kZebraA = rgb(0x17, 0x1b, 0x23);
const Color kZebraB = rgb(0x14, 0x17, 0x1e);
const Color kLogBg = rgb(0x0c, 0x0e, 0x13);

Rect make_rect(int x, int y, int w, int h)
{
    Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

bool point_in(const Rect& r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

}  // namespace

// ---------------- 实现体 ----------------

struct Ui::Impl
{
    // ---- 跨平台状态 ----
    std::function<void(Ui&)> frame_cb;
    std::vector<std::pair<int, int> > pending_clicks;  // 待消费的点击（客户区坐标）
    std::vector<int> every_marks;                      // every(ms) 按调用点的下次触发时刻
    std::size_t every_index = 0;

    int frame_count = 0;
    int frame_limit = 0;
    int frame_interval_ms = 33;
    bool closing_flag = false;
    bool opened = false;

    int client_w = 0;
    int client_h = 0;
    int mouse_x = -1;
    int mouse_y = -1;

    // ---- 布局游标 ----
    int cursor_x = kMargin;
    int cursor_y = kMargin;
    int row_start_y = kMargin;
    int row_h = 0;
    bool in_row = false;
    int scroll = 0;
    int content_h = 0;

    Rect last_rect;
    std::string status_text;
    Color status_color = theme::dim;

    // 截图请求：从帧回调里只能登记，真正取图在「帧结束后」，否则会拍到半帧
    bool has_pending_capture = false;
    std::string pending_capture_path;
    bool capture_ok_flag = false;
    std::string capture_error_text;

    bool write_bmp(const std::string& path, std::string* error);  // 平台实现见文件后部

#if defined(_WIN32)
    HWND hwnd = 0;
    HDC mem_dc = 0;
    HBITMAP mem_bmp = 0;
    HBITMAP mem_old = 0;
    HFONT font = 0;
    HFONT mono_font = 0;
    bool buffer_dirty = true;
#endif

    // ---------------- 布局 ----------------

    int content_width() const
    {
        const int w = client_w - 2 * kMargin;
        return w > 40 ? w : 40;
    }

    void start_row(int h)
    {
        in_row = true;
        row_start_y = cursor_y;
        row_h = h;
        cursor_x = kMargin;
    }

    // 结束当前行：光标推进到行底
    void line_break()
    {
        if (in_row) {
            in_row = false;
            cursor_y = row_start_y + row_h + kBlockGap;
        }
        cursor_x = kMargin;
    }

    // 占一块 w×h：inline_ok=false 表示必须独占一行
    Rect place(int w, int h, bool inline_ok)
    {
        if (in_row) {
            const bool too_wide = cursor_x + w > kMargin + content_width();
            if (!inline_ok || too_wide) {
                line_break();
            }
        }
        if (!in_row) {
            start_row(h);
        }
        Rect r = make_rect(cursor_x, row_start_y, w, h);
        cursor_x += w + kColGap;
        if (h > row_h) {
            row_h = h;
        }
        return r;
    }

    // 消费一次落在 r 内的待处理点击
    bool consume_click(const Rect& r)
    {
        for (std::size_t i = 0; i < pending_clicks.size(); ++i) {
            if (point_in(r, pending_clicks[i].first, pending_clicks[i].second)) {
                pending_clicks.erase(pending_clicks.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    bool hovered(const Rect& r) const
    {
        return mouse_x >= 0 && point_in(r, mouse_x, mouse_y);
    }

    // ---------------- 平台原语 ----------------

    void fill(const Rect& r, Color c)
    {
        if (r.w <= 0 || r.h <= 0) {
            return;
        }
#if defined(_WIN32)
        if (mem_dc == 0) {
            return;
        }
        RECT rc;
        rc.left = r.x;
        rc.top = r.y;
        rc.right = r.x + r.w;
        rc.bottom = r.y + r.h;
        HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
        FillRect(mem_dc, &rc, brush);
        DeleteObject(brush);
#else
        (void)c;
#endif
    }

    // 圆角填充（按钮、徽章、面板、进度条）
    void fill_round(const Rect& r, Color c, int radius)
    {
        if (r.w <= 0 || r.h <= 0) {
            return;
        }
#if defined(_WIN32)
        if (mem_dc == 0) {
            return;
        }
        HBRUSH brush = CreateSolidBrush(RGB(c.r, c.g, c.b));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(c.r, c.g, c.b));
        HGDIOBJ old_brush = SelectObject(mem_dc, brush);
        HGDIOBJ old_pen = SelectObject(mem_dc, pen);
        RoundRect(mem_dc, r.x, r.y, r.x + r.w, r.y + r.h, radius, radius);
        SelectObject(mem_dc, old_brush);
        SelectObject(mem_dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
#else
        fill(r, c);
        (void)radius;
#endif
    }

    // 画文本（左上角对齐，垂直居中于 kLineH 的行内）；返回绘制宽度
    int text(int x, int y, int clip_w, const std::string& value, Color c, bool mono = false)
    {
        if (value.empty()) {
            return 0;
        }
#if defined(_WIN32)
        if (mem_dc == 0) {
            return static_cast<int>(value.size()) * 7;
        }
        HGDIOBJ old_font = SelectObject(mem_dc, mono && mono_font ? mono_font : font);
        SetBkMode(mem_dc, TRANSPARENT);
        SetTextColor(mem_dc, RGB(c.r, c.g, c.b));

        const std::wstring wide = libmini::internal::utf8_to_wide(value);
        RECT rc;
        rc.left = x;
        rc.top = y;
        rc.right = clip_w > 0 ? x + clip_w : x + 8192;
        rc.bottom = y + kLineH;
        UINT flags = DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_LEFT;
        if (clip_w > 0) {
            flags |= DT_END_ELLIPSIS;
        }
        const int drawn = wide.empty()
                              ? 0
                              : DrawTextW(mem_dc, wide.c_str(), static_cast<int>(wide.size()), &rc,
                                          flags);
        SelectObject(mem_dc, old_font);
        return drawn;
#else
        (void)x;
        (void)y;
        (void)clip_w;
        (void)c;
        (void)mono;
        return static_cast<int>(value.size()) * 7;
#endif
    }

    int text_width(const std::string& value, bool mono = false) const
    {
        if (value.empty()) {
            return 0;
        }
#if defined(_WIN32)
        if (mem_dc == 0) {
            return static_cast<int>(value.size()) * 7;
        }
        HGDIOBJ old_font = SelectObject(mem_dc, mono && mono_font ? mono_font : font);
        SIZE size;
        size.cx = 0;
        size.cy = 0;
        const std::wstring wide = libmini::internal::utf8_to_wide(value);
        if (!wide.empty()) {
            GetTextExtentPoint32W(mem_dc, wide.c_str(), static_cast<int>(wide.size()), &size);
        }
        SelectObject(mem_dc, old_font);
        return size.cx;
#else
        (void)mono;
        return static_cast<int>(value.size()) * 7;
#endif
    }

    void line(int x1, int y1, int x2, int y2, Color c)
    {
#if defined(_WIN32)
        if (mem_dc == 0) {
            return;
        }
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(c.r, c.g, c.b));
        HGDIOBJ old_pen = SelectObject(mem_dc, pen);
        MoveToEx(mem_dc, x1, y1, 0);
        LineTo(mem_dc, x2, y2);
        SelectObject(mem_dc, old_pen);
        DeleteObject(pen);
#else
        (void)x1;
        (void)y1;
        (void)x2;
        (void)y2;
        (void)c;
#endif
    }

    void polyline(const std::vector<std::pair<int, int> >& points, Color c)
    {
        if (points.size() < 2) {
            return;
        }
#if defined(_WIN32)
        if (mem_dc == 0) {
            return;
        }
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(c.r, c.g, c.b));
        HGDIOBJ old_pen = SelectObject(mem_dc, pen);
        MoveToEx(mem_dc, points[0].first, points[0].second, 0);
        for (std::size_t i = 1; i < points.size(); ++i) {
            LineTo(mem_dc, points[i].first, points[i].second);
        }
        SelectObject(mem_dc, old_pen);
        DeleteObject(pen);
#else
        (void)c;
#endif
    }

    // ---------------- 窗口 ----------------

#if defined(_WIN32)
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool ensure_offscreen()
    {
        if (hwnd == 0) {
            return false;
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) {
            return false;
        }
        if (mem_dc != 0 && !buffer_dirty && client_w == w && client_h == h) {
            return true;
        }
        client_w = w;
        client_h = h;
        buffer_dirty = false;

        HDC screen = GetDC(hwnd);
        if (mem_dc != 0) {
            if (mem_old != 0) {
                SelectObject(mem_dc, mem_old);
                mem_old = 0;
            }
            DeleteDC(mem_dc);
            mem_dc = 0;
        }
        if (mem_bmp != 0) {
            DeleteObject(mem_bmp);
            mem_bmp = 0;
        }
        mem_dc = CreateCompatibleDC(screen);
        mem_bmp = CreateCompatibleBitmap(screen, w, h);
        mem_old = static_cast<HBITMAP>(SelectObject(mem_dc, mem_bmp));
        ReleaseDC(hwnd, screen);
        return mem_dc != 0 && mem_bmp != 0;
    }

    static bool register_class()
    {
        static bool done = false;
        if (done) {
            return true;
        }
        WNDCLASSEXW wc;
        std::memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &Impl::wnd_proc;
        wc.hInstance = GetModuleHandleW(0);
        wc.hCursor = LoadCursorA(0, IDC_ARROW);  // 本文件不定义 UNICODE，IDC_ARROW 走 A 版
        wc.lpszClassName = L"LibminiNativeUiWindow";
        if (RegisterClassExW(&wc) == 0) {
            return false;
        }
        done = true;
        return true;
    }

    void release_gdi()
    {
        if (mem_dc != 0) {
            if (mem_old != 0) {
                SelectObject(mem_dc, mem_old);
                mem_old = 0;
            }
            DeleteDC(mem_dc);
            mem_dc = 0;
        }
        if (mem_bmp != 0) {
            DeleteObject(mem_bmp);
            mem_bmp = 0;
        }
        if (font != 0) {
            DeleteObject(font);
            font = 0;
        }
        if (mono_font != 0) {
            DeleteObject(mono_font);
            mono_font = 0;
        }
    }
#endif
};

#if defined(_WIN32)

namespace {

// LPARAM 里的鼠标坐标（windowsx.h 的 GET_X_LPARAM 宏走 short 符号扩展）
void mouse_xy(LPARAM lp, int* x, int* y)
{
    *x = static_cast<int>(static_cast<short>(LOWORD(lp)));
    *y = static_cast<int>(static_cast<short>(HIWORD(lp)));
}

#pragma pack(push, 1)
struct BmpFileHeader
{
    unsigned short type;
    unsigned int size;
    unsigned short reserved1;
    unsigned short reserved2;
    unsigned int offset;
};
struct BmpInfoHeader
{
    unsigned int size;
    int width;
    int height;
    unsigned short planes;
    unsigned short bit_count;
    unsigned int compression;
    unsigned int image_size;
    int x_ppm;
    int y_ppm;
    unsigned int colors_used;
    unsigned int colors_important;
};
#pragma pack(pop)

}  // namespace

LRESULT CALLBACK Ui::Impl::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Impl* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        impl = static_cast<Impl*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
        impl->hwnd = hwnd;
        return 0;
    }
    if (impl == 0) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_SIZE:
        impl->buffer_dirty = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;  // 全自绘，避免闪烁
    case WM_MOUSEMOVE:
        mouse_xy(lp, &impl->mouse_x, &impl->mouse_y);
        return 0;
    case WM_LBUTTONDOWN:
        mouse_xy(lp, &impl->mouse_x, &impl->mouse_y);
        return 0;
    case WM_LBUTTONUP: {
        int x = 0;
        int y = 0;
        mouse_xy(lp, &x, &y);
        impl->pending_clicks.push_back(std::make_pair(x, y));
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        impl->scroll -= (delta / WHEEL_DELTA) * 48;
        if (impl->scroll < 0) {
            impl->scroll = 0;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CLOSE:
        impl->closing_flag = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        impl->hwnd = 0;
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (impl->mem_dc != 0 && impl->client_w > 0 && impl->client_h > 0) {
            BitBlt(dc, 0, 0, impl->client_w, impl->client_h, impl->mem_dc, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#endif  // _WIN32

Ui::Ui() : impl_(new Impl())
{
}

Ui::~Ui()
{
    delete impl_;
}

bool Ui::available()
{
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

const char* Ui::backend()
{
#if defined(_WIN32)
    return "win32";
#else
    return "headless";
#endif
}

void Ui::set_frame_limit(int frames)
{
    impl_->frame_limit = frames;
}

void Ui::set_frame_interval_ms(int ms)
{
    impl_->frame_interval_ms = ms < 1 ? 1 : ms;
}

void Ui::simulate_click(int x, int y)
{
    impl_->pending_clicks.push_back(std::make_pair(x, y));
}

int Ui::frames() const
{
    return impl_->frame_count;
}

bool Ui::closing() const
{
    return impl_->closing_flag;
}

void Ui::close()
{
    impl_->closing_flag = true;
}

void Ui::request_repaint()
{
#if defined(_WIN32)
    if (impl_->hwnd != 0) {
        InvalidateRect(impl_->hwnd, 0, FALSE);
    }
#endif
}

int Ui::width() const
{
    return impl_->client_w;
}

int Ui::height() const
{
    return impl_->client_h;
}

Rect Ui::last_rect() const
{
    return impl_->last_rect;
}

bool Ui::every(int ms)
{
    const std::size_t index = impl_->every_index++;
    const int now = static_cast<int>(libmini::current_timestamp_ms());
    if (index >= impl_->every_marks.size()) {
        impl_->every_marks.resize(index + 1, 0);
    }
    if (now >= impl_->every_marks[index]) {
        impl_->every_marks[index] = now + (ms < 1 ? 1 : ms);
        return true;
    }
    return false;
}

// ---------------- 控件 ----------------

void Ui::gap(int pixels)
{
    impl_->line_break();
    impl_->cursor_y += pixels > 0 ? pixels : 0;
}

void Ui::newline()
{
    impl_->line_break();
}

void Ui::separator()
{
    const Rect r = impl_->place(impl_->content_width(), 1, false);
    impl_->fill(r, theme::header);
    impl_->last_rect = r;
    impl_->line_break();
}

void Ui::title(const std::string& text, const std::string& subtitle)
{
    const Rect r = impl_->place(impl_->content_width(), kTitleH, false);
    const int sub_w = subtitle.empty() ? 0 : impl_->text_width(subtitle) + 8;
    impl_->text(r.x, r.y + 2, r.w - sub_w, text, theme::text);
    if (!subtitle.empty()) {
        impl_->text(r.x + r.w - sub_w + 8, r.y + 5, sub_w, subtitle, theme::dim);
    }
    impl_->line(0, r.y + r.h - 1, impl_->client_w, r.y + r.h - 1, theme::header);
    impl_->last_rect = r;
    impl_->line_break();
}

void Ui::section(const std::string& text)
{
    const Rect r = impl_->place(impl_->content_width(), kSectionH, false);
    impl_->text(r.x, r.y + 4, r.w, text, theme::dim);
    impl_->line(r.x, r.y + r.h - 2, r.x + r.w, r.y + r.h - 2, theme::header);
    impl_->last_rect = r;
    impl_->line_break();
}

void Ui::text(const std::string& value, Color color)
{
    const Rect r = impl_->place(impl_->content_width(), kLineH, false);
    impl_->text(r.x, r.y, r.w, value, color);
    impl_->last_rect = r;
    impl_->line_break();
}

void Ui::kv(const std::string& key, const std::string& value, Color vc)
{
    const Rect r = impl_->place(impl_->content_width(), kLineH, false);
    impl_->text(r.x, r.y, kKeyColW - 8, key, theme::dim);
    impl_->text(r.x + kKeyColW, r.y, r.w - kKeyColW, value, vc);
    impl_->last_rect = r;
    impl_->line_break();
}

void Ui::badge(const std::string& value, Color c)
{
    const int w = impl_->text_width(value) + 18;
    const Rect r = impl_->place(w, kLineH, true);
    impl_->fill_round(r, theme::panel, 10);
    impl_->text(r.x + 9, r.y, r.w - 18, value, c);
    impl_->last_rect = r;
}

bool Ui::inline_button(const std::string& label, bool enabled)
{
    const int w = impl_->text_width(label) + 2 * kButtonPadX;
    const Rect r = impl_->place(w, kButtonH, true);
    const bool hot = enabled && impl_->hovered(r);
    impl_->fill_round(r, hot ? theme::cursor : theme::panel, 6);
    impl_->text(r.x + kButtonPadX, r.y + (kButtonH - kLineH) / 2, r.w - 2 * kButtonPadX, label,
                enabled ? theme::text : theme::faint);
    impl_->last_rect = r;
    if (!enabled) {
        return false;
    }
    return impl_->consume_click(r);
}

bool Ui::button(const std::string& label, bool enabled)
{
    impl_->line_break();
    const bool clicked = inline_button(label, enabled);
    impl_->line_break();
    return clicked;
}

int Ui::button_row(const std::vector<std::string>& labels)
{
    impl_->line_break();
    int hit = -1;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (inline_button(labels[i], true) && hit < 0) {
            hit = static_cast<int>(i);
        }
    }
    impl_->line_break();
    return hit;
}

int Ui::table(const std::vector<std::string>& headers,
              const std::vector<std::vector<std::string> >& rows,
              const std::vector<int>& weights, int highlight)
{
    const std::size_t cols = headers.size();
    if (cols == 0) {
        return -1;
    }
    const int total_w = impl_->content_width();

    // 列宽：给了权重按权重分，否则等分（余量都给最后一列，避免缝隙）
    std::vector<int> widths(cols, total_w / static_cast<int>(cols));
    int weight_sum = 0;
    for (std::size_t i = 0; i < cols && i < weights.size(); ++i) {
        weight_sum += weights[i] > 0 ? weights[i] : 1;
    }
    if (weight_sum > 0 && weights.size() >= cols) {
        int used = 0;
        for (std::size_t i = 0; i < cols; ++i) {
            const int w = total_w * (weights[i] > 0 ? weights[i] : 1) / weight_sum;
            widths[i] = w;
            used += w;
        }
        widths[cols - 1] += total_w - used;
    } else {
        int used = 0;
        for (std::size_t i = 0; i + 1 < cols; ++i) {
            used += widths[i];
        }
        widths[cols - 1] += total_w - used;
    }

    const int header_h = kTableHeaderH;
    const int body_h = static_cast<int>(rows.size()) * kTableRowH;
    const Rect area = impl_->place(total_w, header_h + body_h + 2, false);

    int y = area.y;
    impl_->fill(make_rect(area.x, y, total_w, header_h), theme::header);
    int x = area.x;
    for (std::size_t i = 0; i < cols; ++i) {
        impl_->text(x + 8, y + (header_h - kLineH) / 2, widths[i] - 12, headers[i], theme::dim);
        x += widths[i];
    }
    y += header_h;

    int clicked_row = -1;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const Rect row_rect = make_rect(area.x, y, total_w, kTableRowH);
        const bool is_highlight = static_cast<int>(r) == highlight;
        impl_->fill(row_rect, is_highlight ? theme::panel
                                          : ((r % 2) == 0 ? kZebraA : kZebraB));
        if (!is_highlight && impl_->hovered(row_rect)) {
            impl_->fill(row_rect, theme::panel);
        }
        int cx = area.x;
        for (std::size_t c = 0; c < cols; ++c) {
            const std::string cell = c < rows[r].size() ? rows[r][c] : std::string();
            impl_->text(cx + 8, y + (kTableRowH - kLineH) / 2, widths[c] - 12, cell,
                        c == 0 ? theme::text : theme::dim);
            cx += widths[c];
        }
        if (clicked_row < 0 && impl_->consume_click(row_rect)) {
            clicked_row = static_cast<int>(r);
        }
        y += kTableRowH;
    }
    impl_->line(area.x, y, area.x + total_w, y, theme::header);

    impl_->last_rect = area;
    impl_->line_break();
    return clicked_row;
}

void Ui::sparkline(const std::vector<double>& series, const std::string& caption, int height)
{
    if (height < 24) {
        height = 24;
    }
    const Rect area = impl_->place(impl_->content_width(), height, false);
    impl_->fill_round(area, theme::panel, 6);

    for (int i = 1; i < 4; ++i) {
        const int gy = area.y + area.h * i / 4;
        impl_->line(area.x + 1, gy, area.x + area.w - 2, gy, kZebraB);
    }

    if (!series.empty()) {
        double lo = series[0];
        double hi = series[0];
        for (std::size_t i = 1; i < series.size(); ++i) {
            if (series[i] < lo) {
                lo = series[i];
            }
            if (series[i] > hi) {
                hi = series[i];
            }
        }
        if (hi - lo < 1e-9) {
            hi = lo + 1.0;  // 常量序列也要看得见一条线
        }
        const int inner_w = area.w - 12;
        const int inner_h = area.h - 16;
        const std::size_t n = series.size();
        std::vector<std::pair<int, int> > points;
        points.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.0;
            const double norm = (series[i] - lo) / (hi - lo);
            points.push_back(std::make_pair(area.x + 6 + static_cast<int>(t * inner_w),
                                            area.y + area.h - 8 -
                                                static_cast<int>(norm * inner_h)));
        }
        impl_->polyline(points, theme::accent);
    }
    if (!caption.empty()) {
        impl_->text(area.x + 8, area.y + 2, area.w - 16, caption, theme::dim);
    }
    impl_->last_rect = area;
    impl_->line_break();
}

void Ui::bar(const std::string& caption, double fraction, const std::string& value_text,
             Color fill)
{
    if (fraction < 0.0) {
        fraction = 0.0;
    }
    if (fraction > 1.0) {
        fraction = 1.0;
    }
    const Rect area = impl_->place(impl_->content_width(), kBarH, false);
    const int label_w = impl_->text_width(caption) + 12;
    const int value_w = impl_->text_width(value_text) + 12;
    impl_->text(area.x, area.y + 2, label_w, caption, theme::dim);

    const int track_x = area.x + label_w;
    const int track_w = area.w - label_w - value_w;
    if (track_w > 8) {
        impl_->fill_round(make_rect(track_x, area.y + 4, track_w, 12), theme::panel, 6);
        const int fill_w = static_cast<int>(track_w * fraction);
        if (fill_w > 2) {
            impl_->fill_round(make_rect(track_x, area.y + 4, fill_w, 12), fill, 6);
        }
    }
    if (!value_text.empty()) {
        impl_->text(area.x + area.w - value_w + 4, area.y + 2, value_w - 4, value_text,
                    theme::text);
    }
    impl_->last_rect = area;
    impl_->line_break();
}

void Ui::log_view(const std::vector<std::string>& lines, int height)
{
    if (height < kLineH) {
        height = kLineH;
    }
    const Rect area = impl_->place(impl_->content_width(), height, false);
    impl_->fill_round(area, kLogBg, 6);

    const int fits = (height - 8) / kLineH;
    const std::size_t total = lines.size();
    const std::size_t start =
        total > static_cast<std::size_t>(fits) ? total - static_cast<std::size_t>(fits) : 0;
    int y = area.y + 4;
    for (std::size_t i = start; i < total; ++i) {
        impl_->text(area.x + 8, y, area.w - 16, lines[i], theme::dim, true);
        y += kLineH;
    }
    if (lines.empty()) {
        impl_->text(area.x + 8, area.y + 4, area.w - 16, "（暂无输出）", theme::faint);
    }
    impl_->last_rect = area;
    impl_->line_break();
}

void Ui::status(const std::string& value, Color c)
{
    impl_->status_text = value;
    impl_->status_color = c;
}

// ---------------- 截图（平台中立：只登记请求，真正取图在帧结束后） ----------------

bool Ui::capture_bmp(const std::string& path, std::string* error)
{
    if (!available() || !impl_->opened) {
        if (error != 0) {
            *error = available() ? "窗口未打开，无法截图"
                                 : "当前平台没有原生窗口后端，无法截图";
        }
        return false;
    }
    if (path.empty()) {
        if (error != 0) {
            *error = "截图路径为空";
        }
        return false;
    }
    impl_->has_pending_capture = true;
    impl_->pending_capture_path = path;
    impl_->capture_ok_flag = false;
    impl_->capture_error_text.clear();
    if (error != 0) {
        error->clear();
    }
    return true;
}

bool Ui::capture_ok() const
{
    return impl_->capture_ok_flag;
}

std::string Ui::capture_error() const
{
    return impl_->capture_error_text;
}

// ---------------- 后端 ----------------

#if defined(_WIN32)

bool Ui::open(const std::string& title, int width, int height, std::string* error)
{
    if (impl_->hwnd != 0) {
        if (error != 0) {
            *error = "窗口已经打开";
        }
        return false;
    }
    if (!Impl::register_class()) {
        if (error != 0) {
            *error = "注册窗口类失败";
        }
        return false;
    }
    if (width < 420) {
        width = 420;
    }
    if (height < 260) {
        height = 260;
    }

    RECT rc;
    rc.left = 0;
    rc.top = 0;
    rc.right = width;
    rc.bottom = height;
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    const std::wstring wide_title = libmini::internal::utf8_to_wide(title);
    HWND hwnd = CreateWindowExW(0, L"LibminiNativeUiWindow", wide_title.c_str(),
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top, 0, 0, GetModuleHandleW(0),
                                impl_);
    if (hwnd == 0) {
        if (error != 0) {
            *error = "CreateWindowExW 失败（GetLastError=" +
                     libmini::lexical_cast<std::string>(
                         static_cast<int>(GetLastError())) +
                     "）";
        }
        return false;
    }

    impl_->font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    impl_->mono_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FIXED_PITCH | FF_MODERN, L"Consolas");
    impl_->opened = true;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

int Ui::run(const std::function<void(Ui&)>& frame)
{
    if (!impl_->opened) {
        return 0;
    }
    impl_->frame_cb = frame;

    bool quit = false;
    while (!quit && !impl_->closing_flag) {
        MSG msg;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) {
            break;
        }
        // 截图请求在这里兑现：此刻位图里是「上一帧的完整画面」（含底栏），
        // 若放在帧回调里取，拿到的会是半帧
        if (impl_->has_pending_capture) {
            impl_->has_pending_capture = false;
            impl_->capture_ok_flag =
                write_current_bitmap(impl_->pending_capture_path, &impl_->capture_error_text);
            if (!impl_->capture_ok_flag && impl_->capture_error_text.empty()) {
                impl_->capture_error_text = "截图失败";
            }
        }
        if (impl_->hwnd != 0 && impl_->ensure_offscreen()) {
            // 每帧从头开始描述界面
            impl_->cursor_x = kMargin;
            impl_->cursor_y = kMargin - impl_->scroll;
            impl_->in_row = false;
            impl_->row_start_y = impl_->cursor_y;
            impl_->row_h = 0;
            impl_->every_index = 0;
            impl_->last_rect = Rect();
            impl_->status_text.clear();
            impl_->status_color = theme::dim;

            impl_->fill(make_rect(0, 0, impl_->client_w, impl_->client_h), theme::bg);
            if (impl_->frame_cb) {
                impl_->frame_cb(*this);
            }
            impl_->line_break();

            // 内容高度 → 夹住滚动
            impl_->content_h = impl_->cursor_y + impl_->scroll - kMargin;
            const int view_h = impl_->client_h - kStatusH - kMargin;
            const int max_scroll =
                impl_->content_h > view_h ? impl_->content_h - view_h : 0;
            if (impl_->scroll > max_scroll) {
                impl_->scroll = max_scroll;
            }

            // 底栏（不随滚动移动）
            const Rect bar = make_rect(0, impl_->client_h - kStatusH, impl_->client_w, kStatusH);
            impl_->fill(bar, theme::panel);
            impl_->line(bar.x, bar.y, bar.x + bar.w, bar.y, theme::header);
            impl_->text(bar.x + kMargin, bar.y + 3, bar.w / 2,
                        std::string("后端 ") + backend() + " · 第 " +
                            libmini::lexical_cast<std::string>(impl_->frame_count) +
                            " 帧 · 滚轮滚动 · Esc 关闭",
                        theme::faint);
            if (!impl_->status_text.empty()) {
                const std::string& st = impl_->status_text;
                const int w = impl_->text_width(st);
                impl_->text(bar.x + bar.w - kMargin - w, bar.y + 3, w, st, impl_->status_color);
            }

            impl_->frame_count++;
            HDC dc = GetDC(impl_->hwnd);
            BitBlt(dc, 0, 0, impl_->client_w, impl_->client_h, impl_->mem_dc, 0, 0, SRCCOPY);
            ReleaseDC(impl_->hwnd, dc);
        }
        if (impl_->frame_limit > 0 && impl_->frame_count >= impl_->frame_limit) {
            break;
        }
        Sleep(static_cast<DWORD>(impl_->frame_interval_ms));
    }

    // 最后一帧才登记的请求也要兑现（此时窗口还没销毁、GDI 资源还在）
    if (impl_->has_pending_capture) {
        impl_->has_pending_capture = false;
        impl_->capture_ok_flag =
            write_current_bitmap(impl_->pending_capture_path, &impl_->capture_error_text);
        if (!impl_->capture_ok_flag && impl_->capture_error_text.empty()) {
            impl_->capture_error_text = "截图失败";
        }
    }
    impl_->closing_flag = true;
    impl_->release_gdi();
    if (impl_->hwnd != 0 && IsWindow(impl_->hwnd)) {
        DestroyWindow(impl_->hwnd);
    }
    impl_->hwnd = 0;
    impl_->opened = false;
    return impl_->frame_count;
}

bool Ui::write_current_bitmap(const std::string& path, std::string* error)
{
    if (impl_->mem_dc == 0 || impl_->mem_bmp == 0) {
        if (error != 0) {
            *error = "还没有可截图的画面（窗口未渲染）";
        }
        return false;
    }
    const int w = impl_->client_w;
    const int h = impl_->client_h;
    if (w <= 0 || h <= 0) {
        if (error != 0) {
            *error = "客户区尺寸非法";
        }
        return false;
    }

    BITMAPINFO info;
    std::memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = h;  // 正数 = 自下而上，正是 BMP 的存放顺序
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * 4 *
                                          static_cast<std::size_t>(h),
                                      0);
    if (GetDIBits(impl_->mem_dc, impl_->mem_bmp, 0, static_cast<UINT>(h), &pixels[0], &info,
                  DIB_RGB_COLORS) == 0) {
        if (error != 0) {
            *error = "GetDIBits 失败";
        }
        return false;
    }

    BmpFileHeader file_header;
    file_header.type = 0x4D42;  // "BM"
    file_header.size = static_cast<unsigned int>(sizeof(BmpFileHeader) +
                                                 sizeof(BmpInfoHeader) + pixels.size());
    file_header.reserved1 = 0;
    file_header.reserved2 = 0;
    file_header.offset = static_cast<unsigned int>(sizeof(BmpFileHeader) +
                                                   sizeof(BmpInfoHeader));

    BmpInfoHeader info_header;
    std::memset(&info_header, 0, sizeof(info_header));
    info_header.size = sizeof(BmpInfoHeader);
    info_header.width = w;
    info_header.height = h;
    info_header.planes = 1;
    info_header.bit_count = 32;
    info_header.compression = 0;  // BI_RGB
    info_header.image_size = static_cast<unsigned int>(pixels.size());

    std::string bytes;
    bytes.reserve(sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + pixels.size());
    bytes.append(reinterpret_cast<const char*>(&file_header), sizeof(BmpFileHeader));
    bytes.append(reinterpret_cast<const char*>(&info_header), sizeof(BmpInfoHeader));
    bytes.append(reinterpret_cast<const char*>(&pixels[0]), pixels.size());
    if (!libmini::write_file(path, bytes)) {
        if (error != 0) {
            *error = "写文件失败: " + path;
        }
        return false;
    }
    return true;
}

#else  // 非 Windows：无原生窗口后端

bool Ui::open(const std::string& title, int width, int height, std::string* error)
{
    (void)title;
    (void)width;
    (void)height;
    if (error != 0) {
        *error = "当前平台没有原生窗口后端（本层基于 Win32/GDI，其它平台请走无头模式）";
    }
    return false;
}

int Ui::run(const std::function<void(Ui&)>& frame)
{
    (void)frame;
    return 0;
}

bool Ui::write_current_bitmap(const std::string& path, std::string* error)
{
    (void)path;
    if (error != 0) {
        *error = "当前平台没有原生窗口后端，无法截图";
    }
    return false;
}

#endif  // _WIN32

}  // namespace ui
}  // namespace libmini
