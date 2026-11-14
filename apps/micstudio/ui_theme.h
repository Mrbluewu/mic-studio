#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

namespace ui {

enum class ButtonKind {
  Primary,
  Secondary,
  Ghost,
  Preset,
  Toggle,
  Power,
  DeviceInput,
  DeviceOutput,
  SlotVst2,
  SlotVst3,
};

// 主窗口全部控件的位置；由 compute_layout 统一计算，避免坐标散落。
// 客户区固定 300px 宽：默认只显示“开关 + 设备 + 强度 + 高级入口”，
// advanced=true 时在下方追加场景、插件、参数与声音处理。
struct Layout {
  RECT header;
  RECT power;
  // 主开关只响应圆形按钮本身，点击周围空白不切换降噪。
  RECT power_button;

  RECT device_input;
  RECT device_output;

  RECT denoise_label;
  RECT denoise_value;
  RECT denoise_slider;
  RECT advanced_toggle;

  RECT scene_label;
  RECT preset[4];

  RECT plugin_label;
  RECT plugin_slot[3];

  RECT param_label;
  RECT param_tab[3];
  POINT param_row_origin;

  RECT sound_label;
  RECT gain_label;
  RECT gain_slider;
  RECT gain_value;
  RECT out_gain_label;
  RECT out_gain_slider;
  RECT out_gain_value;
  RECT effect[4];
  RECT compare;
  RECT meter;

  // 常规开关：内置降噪 / 随系统启动 / 启动时最小化
  RECT option[3];

  RECT footer;
  int client_height = 0;
};

struct HeadState {
  bool bypass = false;
  bool advanced = false;
  double latency_ms = 0.0;
  std::wstring footer_line;
};

void prepare_process();
bool initialize();
void shutdown();

void apply_dwm(HWND hwnd);

HFONT font_title();
HFONT font_section();
HFONT font_body();
HFONT font_small();
HFONT font_value();

HBRUSH card_brush();
COLORREF card_color();
HBRUSH window_brush();
COLORREF window_color();

Layout compute_layout(const std::size_t (&rows)[3], const bool (&note)[3],
                      bool advanced = false);
int max_param_rows();

// 页脚展示的版本号（形如 L"v0.1.9"），来源是构建时注入的 MICSTUDIO_VERSION。
std::wstring version_string();

void paint_all(HDC dc, HWND hwnd, const Layout& layout, const HeadState& state);
void draw_button(const DRAWITEMSTRUCT& item, ButtonKind kind, bool checked);
void enable_hover(HWND hwnd);

void draw_text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
               UINT flags);

}  // namespace ui
