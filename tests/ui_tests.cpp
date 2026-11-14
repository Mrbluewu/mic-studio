// Mic Studio UI 模块单元测试。
// 覆盖：主题初始化/释放、布局计算、自绘绘制、滑杆消息契约、底噪条生命周期。
// 说明：滑杆与底噪条都是真实窗口，测试直接向窗口发送消息，
//       不依赖消息循环，因此可在无交互的桌面会话中运行。

#include <windows.h>

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ui_controls.h"
#include "ui_theme.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check_impl(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL %s:%d: %s\n", __FILE__, line, expr);
  }
}

#define CHECK(expr) check_impl((expr), #expr, __LINE__)

void begin(const char* name) { std::printf("-- %s\n", name); }

bool approx(int value, int expected, int tolerance = 1) {
  return std::abs(value - expected) <= tolerance;
}

bool color_near(COLORREF a, COLORREF b, int tolerance) {
  return std::abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b))) <= tolerance &&
         std::abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b))) <= tolerance &&
         std::abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b))) <= tolerance;
}

// ---------------------------------------------------------------------------
// 测试环境
// ---------------------------------------------------------------------------

HINSTANCE g_instance = nullptr;
HWND g_host = nullptr;

constexpr wchar_t kHostClass[] = L"MicStudioTestHost";

struct ScrollLog {
  std::vector<int> codes;
  std::vector<int> positions;

  void clear() {
    codes.clear();
    positions.clear();
  }
  int count() const { return static_cast<int>(codes.size()); }
  bool has(int code) const { return std::find(codes.begin(), codes.end(), code) != codes.end(); }
};

ScrollLog g_scroll;
ScrollLog* g_scroll_log = nullptr;

LRESULT CALLBACK host_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_HSCROLL && g_scroll_log != nullptr) {
    g_scroll_log->codes.push_back(static_cast<int>(LOWORD(wparam)));
    g_scroll_log->positions.push_back(static_cast<int>(HIWORD(wparam)));
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void register_host_class() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = host_proc;
  wc.hInstance = g_instance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kHostClass;
  ::RegisterClassExW(&wc);
}

HWND create_slider(int width = 280) {
  return ::CreateWindowExW(0, ui::slider_class(), L"", WS_CHILD | WS_VISIBLE, 0, 0, width, 30,
                           g_host, nullptr, g_instance, nullptr);
}

HWND create_button() {
  return ::CreateWindowExW(0, L"BUTTON", L"测试", WS_CHILD | BS_OWNERDRAW, 0, 0, 104, 30, g_host,
                           nullptr, g_instance, nullptr);
}

HWND create_meter(int width = 232, int height = 52) {
  return ::CreateWindowExW(0, ui::meter_class(), L"", WS_CHILD | WS_VISIBLE, 0, 0, width, height,
                           g_host, nullptr, g_instance, nullptr);
}

// 手动推进 40 帧，0.72^40 ≈ 2e-6，足以让平滑收敛。
void converge_meter(HWND meter) {
  for (int i = 0; i < 40; ++i) {
    ::SendMessageW(meter, WM_TIMER, 1, 0);
  }
}

// 离屏画布：验证绘制确实落到目标表面。
struct Canvas {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  int width = 0;
  int height = 0;

  ~Canvas() { release(); }

  bool create(int w, int h) {
    width = w;
    height = h;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;  // 自上而下，便于按坐标取样
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    dc = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (bitmap == nullptr || dc == nullptr) {
      return false;
    }
    previous = ::SelectObject(dc, bitmap);
    return true;
  }

  void fill(COLORREF color) {
    HBRUSH brush = ::CreateSolidBrush(color);
    RECT rect{0, 0, width, height};
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);
  }

  COLORREF pixel(int x, int y) const { return ::GetPixel(dc, x, y); }

  void release() {
    if (dc != nullptr) {
      if (previous != nullptr) {
        ::SelectObject(dc, previous);
        previous = nullptr;
      }
      ::DeleteDC(dc);
      dc = nullptr;
    }
    if (bitmap != nullptr) {
      ::DeleteObject(bitmap);
      bitmap = nullptr;
    }
  }
};

template <typename Pred>
int count_pixels(const Canvas& canvas, const RECT& rect, Pred pred) {
  int count = 0;
  for (int y = rect.top; y < rect.bottom; ++y) {
    for (int x = rect.left; x < rect.right; ++x) {
      if (pred(canvas.pixel(x, y))) {
        ++count;
      }
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// 主题生命周期
// ---------------------------------------------------------------------------

void test_theme_lifecycle() {
  begin("theme: initialize/shutdown");

  CHECK(ui::font_title() == nullptr);  // 初始化前不应有字体句柄

  CHECK(ui::initialize());
  CHECK(ui::font_title() != nullptr);
  CHECK(ui::font_section() != nullptr);
  CHECK(ui::font_body() != nullptr);
  CHECK(ui::font_small() != nullptr);
  CHECK(ui::font_value() != nullptr);
  CHECK(ui::card_brush() != nullptr);
  CHECK(ui::card_color() == RGB(17, 28, 38));
  CHECK(ui::window_brush() != nullptr);
  CHECK(ui::window_color() == RGB(10, 18, 24));

  // 页脚版本号来自构建注入的 MICSTUDIO_VERSION，格式固定为 "v" + 版本。
  const std::wstring version = ui::version_string();
  CHECK(version.size() > 1);
  CHECK(version.front() == L'v');
  CHECK(version == L"v" + std::wstring(MICSTUDIO_VERSION,
                                       MICSTUDIO_VERSION + ::lstrlenA(MICSTUDIO_VERSION)));

  // 空 DC / 空字体必须安全返回，不能崩溃。
  ui::draw_text(nullptr, L"safe", RECT{0, 0, 40, 20}, ui::font_body(), RGB(255, 255, 255),
                DT_LEFT);
  Canvas canvas;
  CHECK(canvas.create(40, 20));
  ui::draw_text(canvas.dc, L"safe", RECT{0, 0, 40, 20}, nullptr, RGB(255, 255, 255), DT_LEFT);

  DRAWITEMSTRUCT empty{};
  ui::draw_button(empty, ui::ButtonKind::Primary, false);  // hDC 为空时安全返回

  ui::shutdown();
  CHECK(ui::font_title() == nullptr);
  CHECK(ui::font_body() == nullptr);
  CHECK(ui::card_brush() == nullptr);
  CHECK(ui::window_brush() == nullptr);

  ui::shutdown();  // 幂等
  CHECK(ui::initialize());  // 后续用例继续使用
}

// ---------------------------------------------------------------------------
// 布局计算
// ---------------------------------------------------------------------------

void test_layout_geometry() {
  begin("layout: geometry");

  // 固定 300px 窄窗：所有元素左右边距 14px，内容宽 272px。
  const std::size_t no_rows[3] = {0, 0, 0};
  const bool no_notes[3] = {false, false, false};
  ui::Layout l = ui::compute_layout(no_rows, no_notes, true);
  constexpr int kLeft = 14;
  constexpr int kRight = 286;

  CHECK(l.header.left == kLeft && l.header.right == kRight);
  CHECK(l.power.left == kLeft && l.power.right == kRight);
  CHECK(l.device_input.left == kLeft && l.device_input.right == kRight);
  CHECK(l.device_output.left == kLeft && l.device_output.right == kRight);
  CHECK(l.denoise_slider.left == kLeft && l.denoise_slider.right == kRight);
  CHECK(l.advanced_toggle.left == kLeft && l.advanced_toggle.right == kRight);
  CHECK(l.footer.left == kLeft && l.footer.right == kRight);

  // 默认态纵向顺序：标题 → 开关 → 两个设备行 → 强度 → 高级入口。
  CHECK(l.header.bottom <= l.power.top);
  CHECK(l.power.bottom <= l.device_input.top);
  CHECK(l.device_input.bottom <= l.device_output.top);
  CHECK(l.device_output.bottom <= l.denoise_label.top);
  CHECK(l.denoise_label.bottom <= l.denoise_slider.top);
  CHECK(l.denoise_slider.bottom <= l.advanced_toggle.top);

  // 主开关水平居中，圆直径 104px，命中区域只有圆形本身。
  CHECK(approx((l.power.left + l.power.right) / 2, kLeft + (kRight - kLeft) / 2));
  CHECK(l.power.bottom - l.power.top == 232);
  CHECK(l.power_button.right - l.power_button.left == 104);
  CHECK(l.power_button.bottom - l.power_button.top == 104);
  CHECK(approx((l.power_button.left + l.power_button.right) / 2,
               (l.power.left + l.power.right) / 2));
  CHECK(approx((l.power_button.top + l.power_button.bottom) / 2,
               (l.power.top + l.power.bottom) / 2));

  // 强度标签在左、数值右对齐。
  CHECK(l.denoise_label.left == kLeft);
  CHECK(l.denoise_value.right == kRight);
  CHECK(l.denoise_label.right < l.denoise_value.left);

  // 折叠态：页脚紧跟高级入口，整窗高 500px。
  ui::Layout collapsed = ui::compute_layout(no_rows, no_notes, false);
  CHECK(collapsed.footer.top == collapsed.advanced_toggle.bottom + 6);
  CHECK(collapsed.client_height == 502);
  CHECK(collapsed.footer.bottom + 14 == collapsed.client_height);
  CHECK(collapsed.client_height < l.client_height);

  // 高级区自上而下：场景 → 插件 → 参数 → 声音处理 → 对比 → 底噪条 → 常规。
  CHECK(l.advanced_toggle.bottom < l.scene_label.top);
  CHECK(l.scene_label.bottom <= l.preset[0].top);
  CHECK(l.preset[0].bottom <= l.plugin_label.top);
  CHECK(l.plugin_label.bottom <= l.plugin_slot[0].top);
  CHECK(l.plugin_slot[0].bottom <= l.plugin_slot[1].top);
  CHECK(l.plugin_slot[1].bottom <= l.plugin_slot[2].top);
  CHECK(l.plugin_slot[2].bottom <= l.param_label.top);
  CHECK(l.param_label.bottom <= l.param_row_origin.y);
  CHECK(l.gain_slider.top >= l.param_row_origin.y);
  CHECK(l.gain_slider.bottom <= l.effect[0].top);
  CHECK(l.effect[0].bottom <= l.compare.top);
  CHECK(l.compare.bottom <= l.meter.top);
  CHECK(l.meter.bottom <= l.option[0].top);
  CHECK(l.option[0].bottom <= l.footer.top);
  CHECK(l.footer.bottom + 14 == l.client_height);

  // 场景与效果 chips：同一行等宽，间距 6px。
  for (int i = 0; i < 4; ++i) {
    CHECK(l.preset[i].right > l.preset[i].left);
    CHECK(l.preset[i].bottom - l.preset[i].top == 26);
    CHECK(l.effect[i].right > l.effect[i].left);
    CHECK(l.effect[i].bottom - l.effect[i].top == 26);
    if (i > 0) {
      CHECK(l.preset[i - 1].right + 6 == l.preset[i].left);
      CHECK(l.effect[i - 1].right + 6 == l.effect[i].left);
    }
  }
  CHECK(l.preset[3].right == kRight);
  CHECK(l.effect[3].right == kRight);

  // 常规开关：三个等宽 chip 一行，覆盖内置降噪/开机启动/最小化启动。
  for (int i = 0; i < 3; ++i) {
    CHECK(l.option[i].right > l.option[i].left);
    CHECK(l.option[i].bottom - l.option[i].top == 26);
    if (i > 0) {
      CHECK(l.option[i - 1].right + 6 == l.option[i].left);
    }
  }
  CHECK(l.option[2].right == kRight);

  // 插件槽等宽整行，参数页签靠右且互不重叠。
  for (int slot = 0; slot < 3; ++slot) {
    CHECK(l.plugin_slot[slot].bottom - l.plugin_slot[slot].top == 28);
  }
  CHECK(l.param_tab[0].right < l.param_tab[1].left);
  CHECK(l.param_tab[1].right < l.param_tab[2].left);
  CHECK(l.param_tab[2].right == kRight);
  CHECK(l.param_row_origin.x == kLeft);
  CHECK(l.param_row_origin.y >= l.param_tab[0].bottom);

  // 增益滑杆单独一行，数值右对齐；输入/输出增益上下排列。
  CHECK(l.gain_slider.left == kLeft && l.gain_slider.right == kRight);
  CHECK(l.gain_value.right == kRight);
  CHECK(l.gain_label.right < l.gain_value.left);
  CHECK(l.gain_slider.bottom <= l.out_gain_label.top);
  CHECK(l.out_gain_slider.left == kLeft && l.out_gain_slider.right == kRight);
  CHECK(l.out_gain_value.right == kRight);
  CHECK(l.out_gain_label.right < l.out_gain_value.left);
  CHECK(l.out_gain_slider.bottom <= l.effect[0].top);

  // 对比按钮与底噪条都是整行。
  CHECK(l.compare.left == kLeft && l.compare.right == kRight);
  CHECK(l.compare.bottom - l.compare.top == 26);
  CHECK(l.meter.left == kLeft && l.meter.right == kRight);
  CHECK(l.meter.bottom - l.meter.top == 52);
}

void test_layout_fixed_width() {
  begin("layout: fixed width");

  // 布局不再依赖客户区宽度：固定 300px 窄窗，传入任何宽度结果一致。
  const std::size_t one_row[3] = {2, 0, 0};
  const bool no_notes[3] = {false, false, false};
  const ui::Layout fixed = ui::compute_layout(one_row, no_notes, true);
  CHECK(fixed.header.right == 286);
  CHECK(fixed.power.right == 286);
  CHECK(fixed.meter.right == 286);
  CHECK(fixed.client_height == 1042 + 26);

  const int rows[] = {0, 1, 3, 8};
  for (int row : rows) {
    const std::size_t per_slot[3] = {static_cast<std::size_t>(row), 0, 0};
    ui::Layout layout = ui::compute_layout(per_slot, no_notes, true);
    CHECK(layout.header.right == 286);
    CHECK(layout.compare.right == 286);
  }
}

void test_layout_param_rows() {
  begin("layout: parameter rows");

  // 没有插件时仍保留一行空白，避免参数区塌陷。
  const std::size_t none_rows[3] = {0, 0, 0};
  const bool no_notes[3] = {false, false, false};
  ui::Layout none = ui::compute_layout(none_rows, no_notes, true);
  CHECK(none.client_height == 1042);
  CHECK(none.param_row_origin.y == 684);

  // 三行参数比一行高 2*26px。
  const std::size_t three_rows[3] = {3, 0, 0};
  ui::Layout three = ui::compute_layout(three_rows, no_notes, true);
  CHECK(three.client_height == 1042 + 2 * 26);
  CHECK(three.sound_label.top == none.sound_label.top + 2 * 26);

  // 三个槽取参数行数最多的一个。
  const std::size_t mixed_rows[3] = {2, 5, 0};
  ui::Layout mixed = ui::compute_layout(mixed_rows, no_notes, true);
  CHECK(mixed.client_height == 1042 + 4 * 26);
  CHECK(mixed.footer.top == mixed.option[2].bottom + 10);

  // 加载失败提示行额外占用 18px。
  const bool one_note[3] = {false, true, false};
  ui::Layout note = ui::compute_layout(none_rows, one_note, true);
  CHECK(note.client_height == 1042 + 18);

  const std::size_t four_rows[3] = {4, 4, 4};
  const bool all_notes[3] = {true, true, true};
  ui::Layout both = ui::compute_layout(four_rows, all_notes, true);
  CHECK(both.client_height == 1042 + 3 * 26 + 18);

  const int rows = ui::max_param_rows();
  CHECK(rows >= 2 && rows <= 6);
  CHECK(ui::max_param_rows() == rows);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void test_drawing() {
  begin("theme: paint and owner-draw");

  Canvas canvas;
  CHECK(canvas.create(340, 1060));

  const std::size_t two_rows[3] = {2, 2, 0};
  const bool no_notes[3] = {false, false, false};
  ui::Layout layout = ui::compute_layout(two_rows, no_notes, true);
  ui::HeadState state;
  state.advanced = true;
  state.footer_line = L"48 kHz · 10 ms · 处理中";

  const COLORREF bg = ui::window_color();
  const COLORREF card = ui::card_color();

  canvas.fill(RGB(255, 255, 255));
  ui::paint_all(canvas.dc, g_host, layout, state);
  CHECK(color_near(canvas.pixel(4, 4), bg, 2));          // 整窗平涂背景
  CHECK(color_near(canvas.pixel(300, 890), bg, 2));      // 高级区没有卡片外框
  CHECK(!color_near(canvas.pixel(4, 4), RGB(255, 255, 255), 8));

  // 顶部状态胶囊：开启为深绿底，旁路为深琥珀底。
  CHECK(color_near(canvas.pixel(226, 20), RGB(15, 43, 44), 4));
  state.bypass = true;
  canvas.fill(RGB(255, 255, 255));
  ui::paint_all(canvas.dc, g_host, layout, state);
  CHECK(color_near(canvas.pixel(226, 20), RGB(38, 35, 26), 4));
  state.bypass = false;

  // 折叠态：页脚以下不再绘制任何内容。
  canvas.fill(RGB(255, 255, 255));
  const std::size_t empty_rows[3] = {0, 0, 0};
  ui::paint_all(canvas.dc, g_host, ui::compute_layout(empty_rows, no_notes, false), state);
  CHECK(color_near(canvas.pixel(4, 4), bg, 2));
  CHECK(color_near(canvas.pixel(4, 519), bg, 2));

  HWND button = create_button();
  CHECK(button != nullptr);

  const ui::ButtonKind kinds[] = {ui::ButtonKind::Primary,      ui::ButtonKind::Secondary,
                                  ui::ButtonKind::Ghost,        ui::ButtonKind::Preset,
                                  ui::ButtonKind::Toggle,       ui::ButtonKind::DeviceInput,
                                  ui::ButtonKind::DeviceOutput, ui::ButtonKind::SlotVst2,
                                  ui::ButtonKind::SlotVst3};
  for (ui::ButtonKind kind : kinds) {
    for (int checked = 0; checked < 2; ++checked) {
      canvas.fill(RGB(255, 255, 255));
      DRAWITEMSTRUCT item{};
      item.hDC = canvas.dc;
      item.hwndItem = button;
      item.rcItem = RECT{20, 10, 124, 40};
      ui::draw_button(item, kind, checked != 0);
      // 圆角之外也必须铺窗口背景，不能露出按钮默认白底。
      CHECK(color_near(canvas.pixel(21, 11), bg, 2));
    }
  }

  // 设备行使用面板底色。
  canvas.fill(RGB(255, 255, 255));
  DRAWITEMSTRUCT device{};
  device.hDC = canvas.dc;
  device.hwndItem = button;
  device.rcItem = RECT{14, 314, 286, 352};
  ui::draw_button(device, ui::ButtonKind::DeviceInput, false);
  CHECK(color_near(canvas.pixel(150, 333), card, 3));
  CHECK(color_near(canvas.pixel(15, 315), bg, 2));  // 圆角外

  // 圆形电源开关：命中区域只有 104px 圆形本身。
  const int power_cx = (layout.power_button.left + layout.power_button.right) / 2;
  // 圆心处是电源图标，取圆内偏上一点，避开图标笔画。
  const int power_cy = (layout.power_button.top + layout.power_button.bottom) / 2 - 34;
  DRAWITEMSTRUCT power{};
  power.hDC = canvas.dc;
  power.hwndItem = button;
  power.rcItem = layout.power_button;
  canvas.fill(RGB(255, 255, 255));
  ui::draw_button(power, ui::ButtonKind::Power, true);
  CHECK(color_near(canvas.pixel(power_cx, power_cy), RGB(52, 224, 192), 6));
  CHECK(color_near(canvas.pixel(layout.power_button.left + 1, layout.power_button.top + 1), bg,
                   2));  // 圆外仍是窗口背景

  canvas.fill(RGB(255, 255, 255));
  ui::draw_button(power, ui::ButtonKind::Power, false);
  CHECK(color_near(canvas.pixel(power_cx, power_cy), card, 3));

  canvas.fill(RGB(255, 255, 255));
  DRAWITEMSTRUCT pressed{};
  pressed.hDC = canvas.dc;
  pressed.hwndItem = button;
  pressed.rcItem = RECT{20, 10, 124, 40};
  pressed.itemState = ODS_SELECTED | ODS_DISABLED;
  ui::draw_button(pressed, ui::ButtonKind::Primary, false);
  CHECK(!color_near(canvas.pixel(60, 25), RGB(255, 255, 255), 8));

  ::DestroyWindow(button);
}

void test_hover_subclass() {
  begin("button: hover tracking");

  HWND button = create_button();
  CHECK(button != nullptr);
  ui::enable_hover(button);

  CHECK(::GetPropW(button, L"ui_hover") == nullptr);
  ::SendMessageW(button, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
  CHECK(::GetPropW(button, L"ui_hover") != nullptr);
  ::SendMessageW(button, WM_MOUSEMOVE, 0, MAKELPARAM(6, 5));  // 重复移动不重复设置
  CHECK(::GetPropW(button, L"ui_hover") != nullptr);
  ::SendMessageW(button, WM_MOUSELEAVE, 0, 0);
  CHECK(::GetPropW(button, L"ui_hover") == nullptr);

  ::DestroyWindow(button);
}

// ---------------------------------------------------------------------------
// 自绘滑杆
// ---------------------------------------------------------------------------

void slider_range(HWND hwnd, int minimum, int maximum) {
  ::SendMessageW(hwnd, TBM_SETRANGE, TRUE, MAKELPARAM(minimum, maximum));
}

void slider_set(HWND hwnd, int value) { ::SendMessageW(hwnd, TBM_SETPOS, TRUE, value); }

int slider_get(HWND hwnd) { return static_cast<int>(::SendMessageW(hwnd, TBM_GETPOS, 0, 0)); }

void slider_key(HWND hwnd, int key) {
  ::SendMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>(key), 0);
}

void slider_wheel(HWND hwnd, int delta) {
  ::SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), MAKELPARAM(0, 0));
}

void slider_mouse(HWND hwnd, UINT message, int x) {
  const WPARAM keys = message == WM_MOUSEMOVE ? MK_LBUTTON : 0;
  ::SendMessageW(hwnd, message, keys, MAKELPARAM(x, 15));
}

void test_slider_range_contract() {
  begin("slider: range/position contract");

  HWND slider = create_slider();
  CHECK(slider != nullptr);
  CHECK(slider_get(slider) == 0);

  slider_range(slider, 0, 100);
  CHECK(slider_get(slider) == 0);

  // 与原生 trackbar 一致：只有 wParam=TRUE 才写入位置。
  ::SendMessageW(slider, TBM_SETPOS, FALSE, 40);
  CHECK(slider_get(slider) == 0);
  ::SendMessageW(slider, TBM_SETPOS, TRUE, 40);
  CHECK(slider_get(slider) == 40);

  // 越界自动夹紧。
  slider_set(slider, 400);
  CHECK(slider_get(slider) == 100);
  slider_set(slider, -30);
  CHECK(slider_get(slider) == 0);

  // 反向范围按升序归一。
  slider_range(slider, 500, 0);
  slider_set(slider, 700);
  CHECK(slider_get(slider) == 500);
  slider_set(slider, -5);
  CHECK(slider_get(slider) == 0);

  // 设置范围不应回发 WM_HSCROLL。
  g_scroll.clear();
  slider_range(slider, 0, 10);
  slider_set(slider, 5);
  CHECK(g_scroll.count() == 0);

  ::DestroyWindow(slider);
}

void test_slider_keyboard() {
  begin("slider: keyboard");

  HWND slider = create_slider();
  slider_range(slider, 0, 100);  // 步进 = 1

  g_scroll.clear();
  slider_key(slider, VK_RIGHT);
  CHECK(slider_get(slider) == 1);
  CHECK(g_scroll.count() == 2);  // 拖动 + 结束各一次
  CHECK(g_scroll.codes[0] == TB_THUMBTRACK);
  CHECK(g_scroll.codes[1] == TB_ENDTRACK);
  CHECK(g_scroll.positions[0] == 1);

  slider_key(slider, VK_LEFT);
  CHECK(slider_get(slider) == 0);
  slider_key(slider, VK_LEFT);
  CHECK(slider_get(slider) == 0);  // 下限不越界

  slider_key(slider, VK_END);
  CHECK(slider_get(slider) == 100);
  slider_key(slider, VK_HOME);
  CHECK(slider_get(slider) == 0);

  slider_set(slider, 50);
  slider_key(slider, VK_PRIOR);  // +10
  CHECK(slider_get(slider) == 60);
  slider_key(slider, VK_NEXT);  // -10
  CHECK(slider_get(slider) == 50);

  slider_key(slider, VK_SPACE);  // 未处理按键不影响数值
  CHECK(slider_get(slider) == 50);

  // 0..1000 时步进按 1% 计算。
  slider_range(slider, 0, 1000);
  slider_set(slider, 500);
  slider_key(slider, VK_RIGHT);
  CHECK(slider_get(slider) == 510);
  slider_key(slider, VK_LEFT);
  CHECK(slider_get(slider) == 500);

  ::DestroyWindow(slider);
}

void test_slider_wheel() {
  begin("slider: wheel");

  HWND slider = create_slider();
  slider_range(slider, 0, 1000);
  slider_set(slider, 500);

  g_scroll.clear();
  slider_wheel(slider, WHEEL_DELTA);
  CHECK(slider_get(slider) == 510);
  CHECK(g_scroll.has(TB_THUMBTRACK));
  CHECK(g_scroll.has(TB_ENDTRACK));

  slider_wheel(slider, -WHEEL_DELTA);
  CHECK(slider_get(slider) == 500);

  slider_set(slider, 0);
  slider_wheel(slider, -WHEEL_DELTA);
  CHECK(slider_get(slider) == 0);  // 下限不越界

  slider_set(slider, 1000);
  slider_wheel(slider, WHEEL_DELTA);
  CHECK(slider_get(slider) == 1000);  // 上限不越界

  ::DestroyWindow(slider);
}

void test_slider_mouse() {
  begin("slider: mouse mapping");

  // 客户端宽 280px，thumb 半径 7 → 轨道范围 7..273。
  HWND slider = create_slider(280);
  slider_range(slider, 0, 100);

  g_scroll.clear();
  slider_mouse(slider, WM_LBUTTONDOWN, 7);  // 轨道左端
  CHECK(slider_get(slider) == 0);
  CHECK(g_scroll.codes[0] == TB_THUMBTRACK);

  slider_mouse(slider, WM_MOUSEMOVE, 140);  // 中点映射 50%
  CHECK(approx(slider_get(slider), 50));

  slider_mouse(slider, WM_MOUSEMOVE, 300);  // 超出右端夹到最大值
  CHECK(slider_get(slider) == 100);

  slider_mouse(slider, WM_MOUSEMOVE, -40);  // 超出左端夹到最小值
  CHECK(slider_get(slider) == 0);

  g_scroll.clear();
  slider_mouse(slider, WM_MOUSEMOVE, 140);
  slider_mouse(slider, WM_LBUTTONUP, 140);
  CHECK(approx(slider_get(slider), 50));
  CHECK(g_scroll.has(TB_THUMBTRACK));
  CHECK(g_scroll.has(TB_ENDTRACK));
  CHECK(g_scroll.codes.back() == TB_ENDTRACK);

  // 未按下时移动不应改变数值。
  slider_set(slider, 30);
  slider_mouse(slider, WM_MOUSEMOVE, 260);
  CHECK(slider_get(slider) == 30);

  ::DestroyWindow(slider);
}

void test_slider_disabled() {
  begin("slider: disabled ignores input");

  HWND slider = create_slider();
  slider_range(slider, 0, 100);
  slider_set(slider, 30);

  ::EnableWindow(slider, FALSE);
  slider_mouse(slider, WM_LBUTTONDOWN, 270);
  CHECK(slider_get(slider) == 30);
  slider_key(slider, VK_RIGHT);
  CHECK(slider_get(slider) == 30);
  slider_wheel(slider, WHEEL_DELTA);
  CHECK(slider_get(slider) == 30);

  ::EnableWindow(slider, TRUE);
  slider_key(slider, VK_RIGHT);
  CHECK(slider_get(slider) == 31);

  ::DestroyWindow(slider);
}

// ---------------------------------------------------------------------------
// 底噪对比条
// ---------------------------------------------------------------------------

void test_meter_lifecycle() {
  begin("meter: lifecycle and animation");

  HWND meter = ::CreateWindowExW(0, ui::meter_class(), L"", WS_CHILD | WS_VISIBLE, 0, 0, 232, 52,
                                 g_host, nullptr, g_instance, nullptr);
  CHECK(meter != nullptr);

  // 空句柄与非 meter 窗口都应安全返回。
  ui::set_meter(nullptr, -50.0f, -60.0f, false);
  ui::set_meter(g_host, -50.0f, -60.0f, false);

  ui::set_meter(meter, -48.0f, -66.0f, false);
  // 手动驱动 40 帧，覆盖平滑逼近与收敛后停表两条路径。
  for (int i = 0; i < 40; ++i) {
    ::SendMessageW(meter, WM_TIMER, 1, 0);
  }

  ui::set_meter(meter, -80.0f, -95.0f, true);
  for (int i = 0; i < 40; ++i) {
    ::SendMessageW(meter, WM_TIMER, 1, 0);
  }

  // 相同目标值不应重启动画，但也不能出错。
  ui::set_meter(meter, -80.0f, -95.0f, true);
  ::SendMessageW(meter, WM_TIMER, 1, 0);

  // 极值输入只影响映射，不应崩溃。
  ui::set_meter(meter, -200.0f, 5.0f, false);
  for (int i = 0; i < 10; ++i) {
    ::SendMessageW(meter, WM_TIMER, 1, 0);
  }

  ::DestroyWindow(meter);
}

void test_meter_smoothing() {
  begin("meter: smoothing coefficient");

  HWND meter = create_meter();
  ui::MeterSnapshot snap{};
  CHECK(ui::meter_snapshot(meter, snap));
  CHECK(snap.shown_raw == -120.0f);
  CHECK(snap.shown_out == -120.0f);
  CHECK(!snap.animating);

  // 非 meter 窗口不持有状态。
  CHECK(!ui::meter_snapshot(g_host, snap));
  CHECK(!ui::meter_snapshot(nullptr, snap));

  ui::set_meter(meter, -48.0f, -66.0f, false);
  CHECK(ui::meter_snapshot(meter, snap));
  CHECK(snap.target_raw == -48.0f);
  CHECK(snap.target_out == -66.0f);
  CHECK(snap.shown_raw == -120.0f);  // 尚未推进帧
  CHECK(snap.animating);
  CHECK(!snap.bypass);

  // 每帧按 0.28 的系数向目标逼近。
  float raw = -120.0f;
  float out = -120.0f;
  for (int i = 0; i < 5; ++i) {
    ::SendMessageW(meter, WM_TIMER, 1, 0);
    raw += (-48.0f - raw) * 0.28f;
    out += (-66.0f - out) * 0.28f;
    CHECK(ui::meter_snapshot(meter, snap));
    CHECK(std::fabs(snap.shown_raw - raw) < 0.002f);
    CHECK(std::fabs(snap.shown_out - out) < 0.002f);
  }

  // 差值小于 0.4 dB 时吸附到目标并停表。
  int frames = 5;
  while (snap.animating && frames < 60) {
    ::SendMessageW(meter, WM_TIMER, 1, 0);
    ++frames;
    CHECK(ui::meter_snapshot(meter, snap));
  }
  CHECK(!snap.animating);
  CHECK(snap.shown_raw == -48.0f);
  CHECK(snap.shown_out == -66.0f);
  CHECK(frames <= 25);

  // 变化不超过 0.05 dB 时只更新目标，不重启动画。
  ui::set_meter(meter, -48.04f, -66.0f, false);
  CHECK(ui::meter_snapshot(meter, snap));
  CHECK(!snap.animating);
  CHECK(snap.target_raw == -48.04f);
  CHECK(snap.shown_raw == -48.0f);  // 画面数值保持不动

  // 超过阈值则重新开始逼近。
  ui::set_meter(meter, -48.5f, -66.0f, false);
  CHECK(ui::meter_snapshot(meter, snap));
  CHECK(snap.animating);
  CHECK(snap.target_raw == -48.5f);
  converge_meter(meter);

  // 仅旁路状态变化也要重启动画。
  CHECK(ui::meter_snapshot(meter, snap));
  CHECK(!snap.animating);
  ui::set_meter(meter, -48.5f, -66.0f, true);
  CHECK(ui::meter_snapshot(meter, snap));
  CHECK(snap.animating);
  CHECK(snap.bypass);
  converge_meter(meter);
  CHECK(ui::meter_snapshot(meter, snap));
  CHECK(!snap.animating);

  ::DestroyWindow(meter);
}

void test_slider_pixels() {
  begin("slider: pixel appearance");

  // 布局里滑杆只有 20px 高，thumb 14px、光晕 22px 都会贴到边界，
  // 这里就按真实高度取样，确认边缘不会漏出未绘制的白线。
  constexpr int kWidth = 272;
  constexpr int kHeight = 20;
  HWND slider = ::CreateWindowExW(0, ui::slider_class(), L"", WS_CHILD | WS_VISIBLE, 0, 0, kWidth,
                                  kHeight, g_host, nullptr, g_instance, nullptr);
  CHECK(slider != nullptr);
  slider_range(slider, 0, 1000);
  slider_set(slider, 500);

  Canvas canvas;
  CHECK(canvas.create(kWidth, kHeight));
  const COLORREF card = ui::window_color();
  const COLORREF accent = RGB(52, 224, 192);

  canvas.fill(RGB(255, 255, 255));
  ui::slider_paint_to(slider, canvas.dc);

  // 四角必须是卡片底色：绘制必须铺满整个客户区，不能漏边。
  CHECK(color_near(canvas.pixel(0, 0), card, 2));
  CHECK(color_near(canvas.pixel(kWidth - 1, 0), card, 2));
  CHECK(color_near(canvas.pixel(0, kHeight - 1), card, 2));
  CHECK(color_near(canvas.pixel(kWidth - 1, kHeight - 1), card, 2));

  // 中点为 50%：左半段是强调色，thumb 右侧仍是轨道色。
  CHECK(color_near(canvas.pixel(20, kHeight / 2), accent, 24));
  CHECK(color_near(canvas.pixel(kWidth - 20, kHeight / 2), RGB(27, 42, 53), 2));

  // 除 thumb 圆点自身外，整帧不应残留任何白色像素——这正是那条闪一下的
  // 白边：背景铺满后边缘不应再有未绘制/混色像素。
  const RECT frame{0, 0, kWidth, kHeight};
  const int white = count_pixels(canvas, frame, [](COLORREF c) {
    return GetRValue(c) > 200 && GetGValue(c) > 200 && GetBValue(c) > 200;
  });
  // thumb 圆点面积约 150px，其余位置出现白色即判定为白边复现。
  CHECK(white > 0 && white < 260);
  CHECK(color_near(canvas.pixel(20, 0), card, 2));
  CHECK(color_near(canvas.pixel(kWidth - 20, kHeight - 1), card, 2));

  // 获得焦点后画焦点环，圆环同样不能越界，边缘仍是底色。
  ::SetFocus(slider);
  canvas.fill(RGB(255, 255, 255));
  ui::slider_paint_to(slider, canvas.dc);
  CHECK(color_near(canvas.pixel(0, 0), card, 2));
  CHECK(color_near(canvas.pixel(kWidth - 1, kHeight - 1), card, 2));
  CHECK(color_near(canvas.pixel(20, 0), card, 2));

  ::DestroyWindow(slider);
}

void test_meter_pixels() {
  begin("meter: pixel appearance");

  HWND meter = create_meter(232, 52);
  CHECK(meter != nullptr);
  Canvas canvas;
  CHECK(canvas.create(232, 52));

  const COLORREF card = ui::window_color();
  const COLORREF track = RGB(27, 42, 53);

  // 原始 -30 dB（量程 2/3），处理后 -60 dB（量程 1/3）。
  ui::set_meter(meter, -30.0f, -60.0f, false);
  converge_meter(meter);
  ui::MeterSnapshot snap{};
  CHECK(ui::meter_snapshot(meter, snap));
  CHECK(snap.shown_raw == -30.0f && snap.shown_out == -60.0f);

  canvas.fill(RGB(255, 255, 255));
  ui::meter_paint_to(meter, canvas.dc);

  CHECK(color_near(canvas.pixel(4, 4), card, 2));                 // 卡片底色
  CHECK(color_near(canvas.pixel(64, 16), RGB(91, 107, 120), 2));  // 原始条填充
  CHECK(color_near(canvas.pixel(150, 16), track, 2));             // 原始条剩余轨道
  CHECK(color_near(canvas.pixel(60, 38), RGB(52, 224, 192), 2));  // 处理后条为强调色
  CHECK(color_near(canvas.pixel(120, 38), track, 2));             // 处理后条剩余轨道

  // 处理后数值使用强调色。
  const int accent = count_pixels(canvas, RECT{172, 28, 230, 48}, [](COLORREF c) {
    return GetGValue(c) > 120 && GetBValue(c) > 90 && GetRValue(c) < 120;
  });
  CHECK(accent >= 5);

  // 0 dB 满格，低于量程下限为空条。
  ui::set_meter(meter, 0.0f, -200.0f, false);
  converge_meter(meter);
  canvas.fill(RGB(255, 255, 255));
  ui::meter_paint_to(meter, canvas.dc);
  CHECK(color_near(canvas.pixel(160, 16), RGB(91, 107, 120), 2));  // 填满整条
  CHECK(color_near(canvas.pixel(60, 38), track, 2));               // 低于下限不填充

  // 旁路时显示琥珀色“已旁路”。
  ui::set_meter(meter, -30.0f, -60.0f, true);
  converge_meter(meter);
  canvas.fill(RGB(255, 255, 255));
  ui::meter_paint_to(meter, canvas.dc);
  const int amber = count_pixels(canvas, RECT{172, 28, 230, 48}, [](COLORREF c) {
    return GetRValue(c) > 150 && GetGValue(c) > 110 && GetBValue(c) < 140;
  });
  CHECK(amber >= 5);

  ::DestroyWindow(meter);
}

}  // namespace

int main() {
  std::printf("Mic Studio UI tests\n");

  g_instance = ::GetModuleHandleW(nullptr);
  g_scroll_log = &g_scroll;

  ui::prepare_process();
  register_host_class();
  test_theme_lifecycle();  // 结束时保持已初始化状态

  ui::register_control_classes(g_instance);
  WNDCLASSEXW slider_info{};
  slider_info.cbSize = sizeof(slider_info);
  CHECK(::GetClassInfoExW(g_instance, ui::slider_class(), &slider_info) != FALSE);
  WNDCLASSEXW meter_info{};
  meter_info.cbSize = sizeof(meter_info);
  CHECK(::GetClassInfoExW(g_instance, ui::meter_class(), &meter_info) != FALSE);

  g_host = ::CreateWindowExW(0, kHostClass, L"host", WS_POPUP, 0, 0, 340, 1100, nullptr, nullptr,
                             g_instance, nullptr);
  CHECK(g_host != nullptr);

  test_layout_geometry();
  test_layout_fixed_width();
  test_layout_param_rows();
  test_drawing();
  test_hover_subclass();
  test_slider_range_contract();
  test_slider_keyboard();
  test_slider_wheel();
  test_slider_mouse();
  test_slider_disabled();
  test_meter_lifecycle();
  test_meter_smoothing();
  test_slider_pixels();
  test_meter_pixels();

  if (g_host != nullptr) {
    ::DestroyWindow(g_host);
  }
  ui::shutdown();

  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}




