#include "ui_theme.h"

#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cwchar>

#include "ui_icons.h"

// 版本号由构建系统注入（CMake target_compile_definitions），
// 未注入时（例如单独编译本文件）退化成占位版本，保证能编译。
#ifndef MICSTUDIO_VERSION
#define MICSTUDIO_VERSION "0.0.0"
#endif

namespace ui {
namespace {

using namespace Gdiplus;

// Krisp 式极简配色：整窗一个背景色，行/卡片只用一层浅色面板区分。
constexpr COLORREF kAppBg = RGB(10, 18, 24);
constexpr COLORREF kCard = RGB(17, 28, 38);
constexpr COLORREF kCardAlt = RGB(22, 34, 45);
constexpr COLORREF kCardHover = RGB(22, 34, 45);
constexpr COLORREF kCardPressed = RGB(15, 26, 35);
constexpr COLORREF kBorder = RGB(26, 39, 52);
constexpr COLORREF kBorderHover = RGB(36, 52, 66);
constexpr COLORREF kText = RGB(232, 242, 246);
constexpr COLORREF kText2 = RGB(164, 182, 194);
constexpr COLORREF kMuted = RGB(120, 140, 154);
constexpr COLORREF kLabel = RGB(95, 115, 133);
// 页脚要能读清，不能再压到比分组标签更暗。
constexpr COLORREF kFooter = RGB(102, 122, 138);
constexpr COLORREF kAccent = RGB(52, 224, 192);
constexpr COLORREF kAccentFill = RGB(20, 46, 44);
constexpr COLORREF kAmber = RGB(240, 180, 90);

inline Color to_color(COLORREF color, BYTE alpha = 255) {
  return Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

constexpr int kPad = 14;
constexpr int kFixedWidth = 300;
constexpr int kBlockGap = 10;
constexpr int kChipHeight = 26;
constexpr int kSlotHeight = 28;
constexpr int kSlotGap = 4;
constexpr int kDeviceRow = 38;
constexpr int kControlRadius = 8;
constexpr int kPowerDiameter = 104;
// 主开关区的三处留白：标题下沿 → 圆顶、圆底 → 首行状态文字、状态文字 → 设备行。
constexpr int kPowerTopGap = 25;
constexpr int kPowerStatusGap = 4;
// 两行状态文字：首行 22px + 行间 2px + 次行 16px。
constexpr int kPowerStatusHeight = 40;
constexpr int kPowerBottomGap = 23;

ULONG_PTR g_token = 0;
bool g_ready = false;
HFONT g_font_title = nullptr;
HFONT g_font_section = nullptr;
HFONT g_font_body = nullptr;
HFONT g_font_small = nullptr;
HFONT g_font_value = nullptr;
HBRUSH g_card_brush = nullptr;
HBRUSH g_window_brush = nullptr;

using SetPreferredAppModeFn = int(WINAPI*)(int);
SetPreferredAppModeFn g_set_app_mode = nullptr;

HFONT make_font(int height, int weight) {
  return ::CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void init_dark_support() {
  HMODULE uxtheme = ::LoadLibraryW(L"uxtheme.dll");
  if (uxtheme == nullptr) {
    return;
  }
  g_set_app_mode =
      reinterpret_cast<SetPreferredAppModeFn>(::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
  if (g_set_app_mode != nullptr) {
    g_set_app_mode(2);  // ForceDark
  }
}

void fill_round(Graphics& graphics, const RECT& rect, float radius, const Color& color) {
  icons::fill_round(graphics, rect, radius, color);
}

void stroke_round(Graphics& graphics, const RECT& rect, float radius, const Color& color,
                  float width) {
  icons::stroke_round(graphics, rect, radius, color, width);
}

void paint_soft_button(Graphics& graphics, const RECT& rect, bool ghost, bool hover,
                       bool pressed, bool disabled) {
  if (disabled) {
    if (!ghost) {
      fill_round(graphics, rect, static_cast<float>(kControlRadius), to_color(kCard));
      stroke_round(graphics, rect, static_cast<float>(kControlRadius), Color(255, 23, 35, 46),
                   1.0f);
    }
    return;
  }
  if (ghost && !hover && !pressed) {
    return;
  }
  const Color fill = pressed ? to_color(kCardPressed)
                             : hover ? to_color(kCardHover) : to_color(kCardAlt);
  const Color border = pressed ? to_color(kBorder)
                               : hover ? to_color(kBorderHover) : to_color(kBorder);
  fill_round(graphics, rect, static_cast<float>(kControlRadius), fill);
  stroke_round(graphics, rect, static_cast<float>(kControlRadius), border, 1.0f);
}

}  // namespace

bool initialize() {
  Gdiplus::GdiplusStartupInput input;
  if (Gdiplus::GdiplusStartup(&g_token, &input, nullptr) != Gdiplus::Ok) {
    return false;
  }
  g_ready = true;

  g_font_title = make_font(16, FW_SEMIBOLD);
  // 标题只在这一处使用，放大到 15px 让它成为标题行唯一的强层级。
  g_font_section = make_font(15, FW_SEMIBOLD);
  g_font_body = make_font(12, FW_NORMAL);
  g_font_small = make_font(11, FW_NORMAL);
  g_font_value = make_font(12, FW_SEMIBOLD);
  g_card_brush = ::CreateSolidBrush(kCard);
  g_window_brush = ::CreateSolidBrush(kAppBg);
  return true;
}

void prepare_process() { init_dark_support(); }

void shutdown() {
  if (g_font_title != nullptr) {
    ::DeleteObject(g_font_title);
    g_font_title = nullptr;
  }
  if (g_font_section != nullptr) {
    ::DeleteObject(g_font_section);
    g_font_section = nullptr;
  }
  if (g_font_body != nullptr) {
    ::DeleteObject(g_font_body);
    g_font_body = nullptr;
  }
  if (g_font_small != nullptr) {
    ::DeleteObject(g_font_small);
    g_font_small = nullptr;
  }
  if (g_font_value != nullptr) {
    ::DeleteObject(g_font_value);
    g_font_value = nullptr;
  }
  if (g_card_brush != nullptr) {
    ::DeleteObject(g_card_brush);
    g_card_brush = nullptr;
  }
  if (g_window_brush != nullptr) {
    ::DeleteObject(g_window_brush);
    g_window_brush = nullptr;
  }
  if (g_ready) {
    Gdiplus::GdiplusShutdown(g_token);
    g_ready = false;
  }
}

void apply_dwm(HWND hwnd) {
  const BOOL dark = TRUE;
  ::DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
  const int corner = 2;  // DWMWCP_ROUND
  ::DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
  const COLORREF caption = RGB(8, 14, 20);
  const COLORREF border = RGB(27, 42, 54);
  const COLORREF text = RGB(232, 242, 246);
  ::DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
  ::DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
  ::DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
}

HFONT font_title() { return g_font_title; }
HFONT font_section() { return g_font_section; }
HFONT font_body() { return g_font_body; }
HFONT font_small() { return g_font_small; }
HFONT font_value() { return g_font_value; }

HBRUSH card_brush() { return g_card_brush; }
COLORREF card_color() { return kCard; }
HBRUSH window_brush() { return g_window_brush; }
COLORREF window_color() { return kAppBg; }

Layout compute_layout(bool advanced) {
  Layout layout{};
  // 窗口固定 300px 宽，布局不随客户区伸缩。
  const int w = kFixedWidth;
  const int left = kPad;
  const int right = w - kPad;
  const int content = right - left;

  layout.header = {left, 14, right, 34};

  // 主开关独占上半屏，是整窗唯一的视觉重心。
  // 圆钮上下留白都压到原来的 1/3：标题下沿到圆顶 25px、圆底到首行文字 4px、
  // 第二行文字下沿到设备行 23px；其余控件整体上移，整窗高度随之收缩。
  // 只有圆形按钮本身可点击；两行状态文字画在父窗口上，不参与命中测试。
  const int power_cx = (left + right) / 2;
  const int power_cy = layout.header.bottom + kPowerTopGap + kPowerDiameter / 2;
  layout.power_button = {power_cx - kPowerDiameter / 2, power_cy - kPowerDiameter / 2,
                         power_cx + kPowerDiameter / 2, power_cy + kPowerDiameter / 2};
  // power 只作为圆钮 + 状态文字的失效重绘区，不必铺到设备行。
  layout.power = {left, layout.header.bottom + 10, right,
                  layout.power_button.bottom + kPowerStatusGap + kPowerStatusHeight};

  int top = layout.power.bottom + kPowerBottomGap;
  layout.device_input = {left, top, right, top + kDeviceRow};
  top += kDeviceRow + 6;
  layout.device_output = {left, top, right, top + kDeviceRow};
  top += kDeviceRow + 14;

  // 设备区与降噪强度之间留 14px 组间距，和高级区内部的组间距一致。
  layout.denoise_label = {left, top, left + 120, top + 16};
  // 与本区块两个增益数值共用同一列宽，右沿对齐时左沿也齐。
  layout.denoise_value = {right - 60, top, right, top + 16};
  top += 18;
  layout.denoise_slider = {left, top, right, top + 20};
  // 滑杆与高级入口之间留出分隔线的呼吸位：上方 8px、下方 6px。
  top += 20 + 16;
  layout.advanced_toggle = {left, top, right, top + 32};
  top += 32 + 10;

  if (!advanced) {
    // 与展开态页脚一致：最后一行到页脚统一 6px。
    layout.footer = {left, layout.advanced_toggle.bottom + 6, right,
                     layout.advanced_toggle.bottom + 22};
    layout.client_height = layout.footer.bottom + kPad;
    return layout;
  }

  // 高级区：场景 → 插件 → 声音处理 → 常规，全部单列堆叠。
  // 组内「标题 → 内容」固定 20px，组间统一 kGroupGap，避免各段间距忽大忽小。
  constexpr int kGroupGap = 12;
  layout.scene_label = {left, top, right, top + 14};
  top += 20;
  // 四个 chip 平分整列：余数依次补给前几格，保证最右一格正好贴到内容右沿。
  const int chip_gap = 6;
  const int chip_base = (content - 3 * chip_gap) / 4;
  const int chip_extra = content - 3 * chip_gap - chip_base * 4;
  auto lay_chips4 = [&](RECT (&target)[4], int y) {
    int x = left;
    for (int i = 0; i < 4; ++i) {
      const int width_i = chip_base + (i < chip_extra ? 1 : 0);
      target[i] = {x, y, x + width_i, y + kChipHeight};
      x += width_i + chip_gap;
    }
  };
  lay_chips4(layout.preset, top);
  top += kChipHeight + kGroupGap;

  layout.plugin_label = {left, top, right, top + 14};
  top += 20;
  for (int slot = 0; slot < 3; ++slot) {
    layout.plugin_slot[slot] = {left, top, right, top + kSlotHeight};
    top += kSlotHeight + kSlotGap;
  }
  // 循环末尾多累加了一次槽间距：退回后按组间距接下一组。
  top += kGroupGap - kSlotGap;

  layout.sound_label = {left, top, right, top + 14};
  top += 20;
  // 两条增益滑杆各自「标签 → 滑杆」20px、两条之间 16px，节奏一致。
  constexpr int kGainGap = 16;
  layout.gain_label = {left, top, left + 80, top + 14};
  layout.gain_value = {right - 60, top, right, top + 14};
  top += 20;
  layout.gain_slider = {left, top, right, top + 20};
  top += 20 + kGainGap;

  layout.out_gain_label = {left, top, left + 80, top + 14};
  layout.out_gain_value = {right - 60, top, right, top + 14};
  top += 20;
  layout.out_gain_slider = {left, top, right, top + 20};
  top += 20 + kGroupGap;

  lay_chips4(layout.effect, top);
  top += kChipHeight + 6;

  layout.compare = {left, top, right, top + kChipHeight};
  top += kChipHeight + 6;

  layout.meter = {left, top, right, top + 52};
  top += 62;

  // 三格同理：余数补给前一格，最右一格贴内容右沿。
  const int option_base = (content - 2 * chip_gap) / 3;
  const int option_extra = content - 2 * chip_gap - option_base * 3;
  {
    int x = left;
    for (int i = 0; i < 3; ++i) {
      const int width_i = option_base + (i < option_extra ? 1 : 0);
      layout.option[i] = {x, top, x + width_i, top + kChipHeight};
      x += width_i + chip_gap;
    }
  }
  top += kChipHeight + 10;

  layout.system_sound = {left, top, right, top + kChipHeight};
  top += kChipHeight + 10;

  layout.footer = {left, top, right, top + 16};
  layout.client_height = layout.footer.bottom + kPad;
  return layout;
}

std::wstring version_string() {
  // 从窄字符串转宽字符串，只做一次 ASCII 转换，无需依赖区域设置。
  std::wstring result = L"v";
  for (const char* p = MICSTUDIO_VERSION; *p != '\0'; ++p) {
    result.push_back(static_cast<wchar_t>(*p));
  }
  return result;
}

void paint_all(HDC dc, HWND hwnd, const Layout& layout, const HeadState& state) {
  RECT client{};
  ::GetClientRect(hwnd, &client);
  const int width = client.right;
  const int height = client.bottom;
  if (width <= 0 || height <= 0) {
    return;
  }

  // 整窗一个平涂背景，不再有卡片外框：层次靠行、间距和字色层级表达。
  const bool on = !state.bypass;
  const RECT pill{layout.header.right - 64, layout.header.top, layout.header.right,
                  layout.header.top + 20};
  {
    Graphics graphics(dc);
    // 底色统一不加抗锯齿：抗锯齿会把最外一圈像素与残留内容混合，
    // 表现为窗口/控件边缘那 1px 一闪而过的白线。
    graphics.SetSmoothingMode(SmoothingModeNone);
    SolidBrush background(to_color(kAppBg));
    graphics.FillRectangle(&background, 0, 0, width, height);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);

    fill_round(graphics, pill, 10.0f, on ? Color(255, 15, 43, 44) : Color(255, 38, 35, 26));
    stroke_round(graphics, pill, 10.0f, on ? Color(255, 24, 78, 72) : Color(255, 78, 63, 38),
                 1.0f);
    icons::fill_dot(graphics, static_cast<float>(pill.left) + 10.5f,
                    static_cast<float>(pill.top) + 10.0f, 5.0f,
                    on ? to_color(kAccent) : to_color(kAmber));

    // 分组分隔线：折叠态只画上边一条，把「高级设置」与上方参数区隔开；
    // 展开态上下各一条，形成完整的分组区间。
    {
      Pen line(to_color(kBorder), 1.0f);
      const float l = static_cast<float>(layout.advanced_toggle.left);
      const float r = static_cast<float>(layout.advanced_toggle.right);
      const float top_line = static_cast<float>(layout.advanced_toggle.top) - 0.5f;
      graphics.DrawLine(&line, l, top_line, r, top_line);
      if (state.advanced) {
        const float bottom_line = static_cast<float>(layout.advanced_toggle.bottom) + 0.5f;
        graphics.DrawLine(&line, l, bottom_line, r, bottom_line);
      }
    }
    graphics.Flush();
  }

  // 文字统一用 GDI 绘制，保证 ClearType 清晰度。
  // 标题行三段从右往左依次排：状态胶囊 → 延迟 → 标题。
  // 延迟宽度按实际文本测量，文案变化（个位/十位/三位数）时不会挤压标题。
  const int pill_gap = 8;
  int latency_right = pill.left - pill_gap;
  if (state.latency_ms > 0.0) {
    wchar_t latency[32]{};
    std::swprintf(latency, 32, L"延迟 %.0f ms", state.latency_ms);
    SIZE extent{};
    HFONT old_font = static_cast<HFONT>(::SelectObject(dc, font_small()));
    ::GetTextExtentPoint32W(dc, latency, ::lstrlenW(latency), &extent);
    ::SelectObject(dc, old_font);
    // 文本宽度再加 1px 余量，避免右对齐时最后一像素被裁掉。
    const int latency_width = extent.cx + 1;
    draw_text(dc, latency, RECT{latency_right - latency_width, layout.header.top, latency_right,
                               layout.header.top + 20},
              font_small(), kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    latency_right -= latency_width + pill_gap;
  }
  // 标题占满剩余空间；即使延迟文案很长也不会与标题重叠，最多被省略号截断。
  const int title_right = latency_right;
  // 标题直接与内容左沿对齐：去掉左侧色块后，标题本身就是最左的视觉起点。
  draw_text(dc, L"Mic Studio",
            RECT{layout.header.left, layout.header.top - 4, title_right,
                 layout.header.top + 22},
            font_section(), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  draw_text(dc, on ? L"已开启" : L"已暂停",
            RECT{pill.left + 17, pill.top - 1, pill.right - 6, pill.bottom + 1}, font_small(),
            on ? kAccent : kAmber, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // 主开关下方的两行说明文字画在父窗口上，让圆形按钮成为唯一命中区域。
  {
    // 与圆形按钮的下沿对齐，避免文字压到圆上。
    const LONG text_top = layout.power_button.bottom + kPowerStatusGap;
    draw_text(dc, on ? L"降噪已开启" : L"原声直通",
              RECT{layout.power.left, text_top, layout.power.right, text_top + 22}, font_title(),
              on ? kText2 : kAmber, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    draw_text(dc, on ? L"点击暂停降噪" : L"点击恢复降噪",
              RECT{layout.power.left, text_top + 24, layout.power.right, text_top + 40},
              font_small(), kMuted,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  draw_text(dc, L"降噪强度", layout.denoise_label, font_body(), kText2,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  if (state.advanced) {
    draw_text(dc, L"场景", layout.scene_label, font_small(), kLabel,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, L"降噪插件", layout.plugin_label, font_small(), kLabel,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, L"声音处理", layout.sound_label, font_small(), kLabel,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, L"输入增益", layout.gain_label, font_small(), kText2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, L"输出增益", layout.out_gain_label, font_small(), kText2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  // 页脚：左侧状态文案（默认“本地处理 · 不上传”），右侧版本号。
  // 版本号单独占一条右对齐文字，状态文案右沿停在它前面，长文案最多省略。
  const std::wstring version = version_string();
  const int version_width = 46;
  draw_text(dc, state.footer_line.empty() ? L"本地处理 · 不上传" : state.footer_line,
            RECT{layout.footer.left, layout.footer.top, layout.footer.right - version_width,
                 layout.footer.bottom},
            font_small(), state.footer_line.empty() ? kFooter : kMuted,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  draw_text(dc, version,
            RECT{layout.footer.right - version_width, layout.footer.top, layout.footer.right,
                 layout.footer.bottom},
            font_small(), kFooter,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void draw_text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
               UINT flags) {
  if (dc == nullptr || font == nullptr) {
    return;
  }
  const int old_mode = ::SetBkMode(dc, TRANSPARENT);
  const COLORREF old_bk = ::SetBkColor(dc, kAppBg);
  const COLORREF old_color = ::SetTextColor(dc, color);
  HFONT old_font = static_cast<HFONT>(::SelectObject(dc, font));
  ::DrawTextW(dc, text.c_str(), -1, &rect, flags);
  ::SelectObject(dc, old_font);
  ::SetTextColor(dc, old_color);
  ::SetBkColor(dc, old_bk);
  ::SetBkMode(dc, old_mode);
}

void draw_button(const DRAWITEMSTRUCT& item, ButtonKind kind, bool checked) {
  if (item.hDC == nullptr) {
    return;
  }
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const bool hover = ::GetPropW(item.hwndItem, L"ui_hover") != nullptr;
  const bool focused = (item.itemState & ODS_FOCUS) != 0 || ::GetFocus() == item.hwndItem;

  const RECT rect = item.rcItem;
  const float radius = static_cast<float>(kControlRadius);

  {
    Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);

    // 整块先用不抗锯齿的 GDI 打底：圆角以外也必须铺满窗口背景，
    // 否则按钮窗口自身的白底会在边缘留下 1px 亮线。
    RECT base_rect = rect;
    HBRUSH base_brush = ::CreateSolidBrush(kAppBg);
    ::FillRect(item.hDC, &base_rect, base_brush);
    ::DeleteObject(base_brush);

    switch (kind) {
      case ButtonKind::Primary: {
        const Color fill = disabled ? Color(255, 27, 53, 49)
                                    : pressed ? Color(255, 44, 196, 169)
                                    : hover ? Color(255, 74, 232, 203)
                                            : Color(255, 52, 224, 192);
        fill_round(graphics, rect, radius, fill);
        break;
      }
      case ButtonKind::Secondary:
      case ButtonKind::Ghost:
        paint_soft_button(graphics, rect, kind == ButtonKind::Ghost, hover, pressed, disabled);
        break;
      case ButtonKind::Preset:
      case ButtonKind::Toggle: {
        if (checked) {
          fill_round(graphics, rect, radius, Color(255, 18, 44, 42));
          stroke_round(graphics, rect, radius, Color(255, 44, 120, 108), 1.0f);
        } else {
          fill_round(graphics, rect, radius, to_color(kCard));
          stroke_round(graphics, rect, radius,
                       hover ? to_color(kBorderHover) : to_color(kBorder), 1.0f);
        }
        break;
      }
      case ButtonKind::DeviceInput:
      case ButtonKind::DeviceOutput: {
        fill_round(graphics, rect, radius,
                   hover || pressed ? to_color(kCardAlt) : to_color(kCard));
        stroke_round(graphics, rect, radius,
                     hover ? to_color(kBorderHover) : to_color(kBorder), 1.0f);

        const float icon_x = static_cast<float>(rect.left) + 14.0f;
        const float cy = (static_cast<float>(rect.top) + static_cast<float>(rect.bottom)) / 2.0f;
        const Color icon_color = icons::to_color(icons::kIconGray);
        if (kind == ButtonKind::DeviceInput) {
          icons::draw_mic(graphics, icon_x, cy, 16.0f, icon_color);
        } else {
          icons::draw_speaker(graphics, icon_x, cy, 16.0f, icon_color);
        }
        break;
      }
      case ButtonKind::SlotVst2:
      case ButtonKind::SlotVst3: {
        fill_round(graphics, rect, radius,
                   hover || pressed ? to_color(kCardAlt) : to_color(kCard));
        stroke_round(graphics, rect, radius,
                     hover ? to_color(kBorderHover) : to_color(kBorder), 1.0f);
        break;
      }
      case ButtonKind::Power: {
        // 唯一的圆形主开关：开=强调色实心，关=暗色描边。不再画外发光。
        const float cx = (static_cast<float>(rect.left) + static_cast<float>(rect.right)) / 2.0f;
        const float cy = (static_cast<float>(rect.top) + static_cast<float>(rect.bottom)) / 2.0f;
        const RectF circle(static_cast<float>(rect.left), static_cast<float>(rect.top),
                           static_cast<float>(rect.right - rect.left),
                           static_cast<float>(rect.bottom - rect.top));

        const Color fill = disabled ? Color(255, 20, 30, 38)
                                    : checked ? (pressed ? Color(255, 44, 196, 169)
                                                  : hover ? Color(255, 74, 232, 203)
                                                          : to_color(kAccent))
                                              : (pressed ? to_color(kCardPressed)
                                                  : hover ? to_color(kCardHover)
                                                          : Color(255, 17, 28, 38));
        SolidBrush circle_brush(fill);
        graphics.FillEllipse(&circle_brush, circle);
        if (!checked) {
          Pen ring(to_color(kBorderHover), 1.0f);
          graphics.DrawEllipse(&ring, circle);
        }

        const Color icon = disabled ? Color(255, 96, 112, 124)
                                    : checked ? Color(255, 6, 20, 17)
                                              : icons::to_color(icons::kIconGray);
        icons::draw_power(graphics, cx, cy, 26.0f, icon);
        break;
      }
    }

    // Ghost 是纯文字入口，不画焦点边框：否则点击后会多出一圈与分隔线重叠的亮框。
    // 它的焦点态改用文字提亮表示，见下方文字颜色分支。
    if (focused && !disabled &&
        (kind == ButtonKind::Primary || kind == ButtonKind::Secondary)) {
      stroke_round(graphics, rect, radius, Color(150, 52, 224, 192), 1.0f);
    }
    graphics.Flush();
  }

  wchar_t label[256]{};
  ::GetWindowTextW(item.hwndItem, label, static_cast<int>(sizeof(label) / sizeof(label[0])));

  if (kind == ButtonKind::Power) {
    // 状态文字由父窗口绘制，按钮窗口只覆盖圆形本身。
    return;
  }

  if (kind == ButtonKind::DeviceInput || kind == ButtonKind::DeviceOutput) {
    draw_text(item.hDC, kind == ButtonKind::DeviceInput ? L"麦克风" : L"扬声器",
              RECT{rect.left + 32, rect.top, rect.left + 88, rect.bottom}, font_small(), kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    draw_text(item.hDC, label[0] != 0 ? label : L"未选择",
              RECT{rect.left + 92, rect.top, rect.right - 26, rect.bottom}, font_body(),
              disabled ? RGB(96, 112, 124) : kText,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    // 箭头右沿落在 rect.right - 10，与插件槽行的文字右沿齐平。
    const float x = static_cast<float>(rect.right) - 15.0f;
    const float y = (static_cast<float>(rect.top) + static_cast<float>(rect.bottom)) / 2.0f;
    icons::draw_chevron(graphics, x, y, 16.0f, icons::to_color(RGB(84, 104, 122)), true);
    return;
  }

  if (kind == ButtonKind::SlotVst2 || kind == ButtonKind::SlotVst3) {
    // 空槽还没选类型，显示“插件”；选定后才标出 VST2/VST3。
    const bool empty = label[0] == 0;
    draw_text(item.hDC, empty ? L"插件" : (kind == ButtonKind::SlotVst2 ? L"VST2" : L"VST3"),
              RECT{rect.left + 10, rect.top, rect.left + 52, rect.bottom}, font_small(),
              empty ? kMuted : kText2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (empty) {
      draw_text(item.hDC, L"选择", RECT{rect.left + 56, rect.top, rect.right - 10, rect.bottom},
                font_small(), kAccent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
      draw_text(item.hDC, label, RECT{rect.left + 56, rect.top, rect.right - 10, rect.bottom},
                font_small(), checked ? kAmber : kMuted,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    return;
  }

  COLORREF color = kText2;
  HFONT font = font_body();
  if (kind == ButtonKind::Primary) {
    color = disabled ? RGB(86, 116, 110) : RGB(4, 20, 16);
  } else if (kind == ButtonKind::Preset || kind == ButtonKind::Toggle) {
    font = font_small();
    color = disabled ? RGB(96, 112, 124) : (checked ? kAccent : kText2);
  } else {
    // Ghost 没有边框，焦点态靠文字提亮来表示。
    color = disabled ? RGB(96, 112, 124) : (hover || focused ? kText : kText2);
  }

  RECT text_rect = rect;
  if (pressed) {
    text_rect.top += 1;
    text_rect.bottom += 1;
  }
  draw_text(item.hDC, label, text_rect, font, color,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Ghost 作为折叠入口时补一个小箭头：展开态朝上。
  if (kind == ButtonKind::Ghost) {
    SIZE size{};
    HFONT old_font = static_cast<HFONT>(::SelectObject(item.hDC, font));
    ::GetTextExtentPoint32W(item.hDC, label, ::lstrlenW(label), &size);
    ::SelectObject(item.hDC, old_font);

    const float cx = (static_cast<float>(rect.left) + static_cast<float>(rect.right)) / 2.0f +
                     static_cast<float>(size.cx) / 2.0f + 10.0f;
    const float cy = (static_cast<float>(rect.top) + static_cast<float>(rect.bottom)) / 2.0f;
    Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const Color arrow = disabled ? Color(255, 96, 112, 124)
                                 : to_color(hover ? kText : kMuted);
    // checked = 已展开，箭头朝上；折叠态朝下。
    icons::draw_chevron(graphics, cx, cy, 16.0f, arrow, !checked);
  }
}

namespace {

LRESULT CALLBACK hover_subclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                UINT_PTR subclass_id, DWORD_PTR) {
  switch (message) {
    case WM_MOUSEMOVE:
      if (::GetPropW(hwnd, L"ui_hover") == nullptr) {
        ::SetPropW(hwnd, L"ui_hover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        ::TrackMouseEvent(&track);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      ::RemovePropW(hwnd, L"ui_hover");
      ::InvalidateRect(hwnd, nullptr, FALSE);
      break;
    case WM_NCDESTROY:
      ::RemoveWindowSubclass(hwnd, hover_subclass, subclass_id);
      break;
    default:
      break;
  }
  return ::DefSubclassProc(hwnd, message, wparam, lparam);
}

}  // namespace

void enable_hover(HWND hwnd) {
  ::SetWindowSubclass(hwnd, hover_subclass, 1, 0);
}

}  // namespace ui
