#include "ui_controls.h"

#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "ui_theme.h"
#include "ui_icons.h"

namespace ui {
namespace {

using namespace Gdiplus;

constexpr wchar_t kSliderClass[] = L"MicStudioSlider";
constexpr wchar_t kMeterClass[] = L"MicStudioMeter";

// 轨道两端留白 = thumb 半径，圆点拖到端点时正好贴住控件边缘。
// 控件在布局里向两侧各外扩同样的值，轨道两端因此与内容列严格对齐。
// 绘制与命中映射共用同一个值，避免两处各写一份导致端点偏移。
constexpr int kSliderInset = 7;

const Color kBackgroundColor(255, 10, 18, 24);
const Color kTrackColor(255, 27, 42, 53);
const Color kAccent(255, 52, 224, 192);
constexpr COLORREF kText2 = RGB(164, 182, 194);
constexpr COLORREF kAccentRef = RGB(52, 224, 192);
constexpr COLORREF kAmberRef = RGB(240, 180, 90);

void fill_round(Graphics& graphics, const RectF& rect, float radius, const Color& color) {
  icons::fill_round(graphics, rect, radius, color);
}

void stroke_round(Graphics& graphics, const RectF& rect, float radius, const Color& color,
                  float width) {
  icons::stroke_round(graphics, rect, radius, color, width);
}

// 控件统一走内存 DC 双缓冲。
// 直接把 GDI+ 抗锯齿图元画到窗口 DC 时，重绘只覆盖更新区，更新区边界上的
// 半透明像素会和残留内容混合，表现为滑杆四周闪一下白边/白条。先在离屏位图
// 里画完整帧再整块 BitBlt，边界就不会再出现这类瑕疵。
template <typename Paint>
void paint_buffered(HWND hwnd, HDC target, Paint&& paint) {
  RECT client{};
  ::GetClientRect(hwnd, &client);
  const int width = client.right;
  const int height = client.bottom;
  if (width <= 0 || height <= 0) {
    return;
  }

  // 缓冲位图用 32bpp DIB：GDI+ 画到 DDB 上会按预乘方式处理底色，
  // 背景色会被压暗；DIB 与最终窗口表面像素格式一致，颜色不会被改写。
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC memory = ::CreateCompatibleDC(screen);
  HBITMAP bitmap =
      memory != nullptr ? ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0)
                        : nullptr;
  ::ReleaseDC(nullptr, screen);
  if (memory == nullptr || bitmap == nullptr) {
    if (bitmap != nullptr) {
      ::DeleteObject(bitmap);
    }
    if (memory != nullptr) {
      ::DeleteDC(memory);
    }
    paint(target, width, height);
    return;
  }

  HGDIOBJ old_bitmap = ::SelectObject(memory, bitmap);
  paint(memory, width, height);
  ::BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
  ::SelectObject(memory, old_bitmap);
  ::DeleteObject(bitmap);
  ::DeleteDC(memory);
}

struct SliderState {
  int min = 0;
  int max = 1000;
  int pos = 0;
  bool hover = false;
  bool dragging = false;
  bool focused = false;
};

SliderState* slider_state(HWND hwnd) {
  return reinterpret_cast<SliderState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int slider_step(const SliderState& state) {
  return std::max(1, (state.max - state.min) / 100);
}

void slider_clamp(SliderState& state) {
  state.pos = std::clamp(state.pos, state.min, state.max);
}

void slider_notify(HWND hwnd, SliderState& state, bool end_track) {
  ::SendMessageW(::GetParent(hwnd), WM_HSCROLL,
                 MAKEWPARAM(end_track ? TB_ENDTRACK : TB_THUMBTRACK, state.pos),
                 reinterpret_cast<LPARAM>(hwnd));
}

void slider_set_from_x(HWND hwnd, SliderState& state, int x) {
  RECT client{};
  ::GetClientRect(hwnd, &client);
  const int left = kSliderInset;
  const int right = std::max(left + 1, static_cast<int>(client.right) - kSliderInset);
  const double ratio =
      std::clamp(static_cast<double>(x - left) / static_cast<double>(right - left), 0.0, 1.0);
  state.pos = state.min + static_cast<int>(std::lround(ratio * (state.max - state.min)));
  slider_clamp(state);
  ::InvalidateRect(hwnd, nullptr, FALSE);
}

void paint_slider_frame(SliderState& state, bool enabled, HDC dc, int width, int height) {
  Graphics graphics(dc);
  // 背景必须用无抗锯齿的方式铺满：AntiAlias 下 GDI+ 会对抗锯齿边缘做
  // 半透明混合，贴边的一圈会和残留内容混色，实测就是那条一闪过就消失的白边。
  graphics.SetSmoothingMode(SmoothingModeNone);
  SolidBrush background(kBackgroundColor);
  graphics.FillRectangle(&background, 0, 0, width, height);
  graphics.SetSmoothingMode(SmoothingModeAntiAlias);

  const float center_y = static_cast<float>(height) / 2.0f;
  // 轨道铺满整行：控件宽度就是内容列宽度，两端不再额外内缩。
  // thumb 圆心限制在半径以内，拖到端点时圆点仍然完整可见。
  const float left = static_cast<float>(kSliderInset);
  const float right =
      std::max(left + 1.0f, static_cast<float>(width) - static_cast<float>(kSliderInset));
  const double ratio =
      state.max == state.min
          ? 0.0
          : static_cast<double>(state.pos - state.min) / static_cast<double>(state.max - state.min);
  const float thumb_x = left + static_cast<float>(ratio) * (right - left);
  const float track_width = static_cast<float>(width);

  fill_round(graphics, RectF(0.0f, center_y - 2.0f, track_width, 4.0f), 2.0f, kTrackColor);

  if (thumb_x > 0.5f) {
    if (enabled) {
      fill_round(graphics, RectF(0.0f, center_y - 2.0f, thumb_x, 4.0f), 2.0f, kAccent);
    } else {
      fill_round(graphics, RectF(0.0f, center_y - 2.0f, thumb_x, 4.0f), 2.0f,
                 Color(255, 60, 76, 88));
    }
  }

  if (!enabled) {
    SolidBrush thumb(Color(255, 82, 99, 112));
    graphics.FillEllipse(&thumb, thumb_x - 7.0f, center_y - 7.0f, 14.0f, 14.0f);
    graphics.Flush();
    return;
  }

  // 光晕/焦点环的半径必须收在控件高度内，否则圆会被裁成半截、
  // 上下露出未绘制像素（就是那条闪一下的白边）。
  const float glow_radius = std::min(11.0f, center_y);
  const float ring_radius = std::min(10.0f, center_y - 0.5f);

  // 悬停/拖动给一层很淡的光晕，焦点用细环，状态区分但不喧宾夺主。
  if (state.hover || state.focused || state.dragging) {
    SolidBrush glow(Color(30, 52, 224, 192));
    graphics.FillEllipse(&glow, thumb_x - glow_radius, center_y - glow_radius, glow_radius * 2.0f,
                         glow_radius * 2.0f);
  }

  SolidBrush thumb_fill(state.dragging ? kAccent : Color(255, 226, 240, 244));
  graphics.FillEllipse(&thumb_fill, thumb_x - 7.0f, center_y - 7.0f, 14.0f, 14.0f);
  if (state.dragging) {
    Pen inner(Color(255, 8, 32, 28), 2.0f);
    graphics.DrawEllipse(&inner, thumb_x - 7.0f, center_y - 7.0f, 14.0f, 14.0f);
  }

  if (state.focused && ring_radius > 0.0f) {
    Pen ring(Color(130, 52, 224, 192), 1.0f);
    graphics.DrawEllipse(&ring, thumb_x - ring_radius, center_y - ring_radius, ring_radius * 2.0f,
                         ring_radius * 2.0f);
  }
  graphics.Flush();
}

void paint_slider(HWND hwnd, HDC dc) {
  SliderState* state = slider_state(hwnd);
  if (state == nullptr) {
    return;
  }
  const bool enabled = ::IsWindowEnabled(hwnd) != FALSE;
  paint_buffered(hwnd, dc, [state, enabled](HDC target, int width, int height) {
    paint_slider_frame(*state, enabled, target, width, height);
  });
}

LRESULT CALLBACK slider_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(new SliderState()));
      return 0;

    case WM_NCDESTROY:
      delete slider_state(hwnd);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return ::DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = ::BeginPaint(hwnd, &paint);
      paint_slider(hwnd, dc);
      ::EndPaint(hwnd, &paint);
      return 0;
    }

    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      if (slider_state(hwnd) != nullptr) {
        slider_state(hwnd)->focused = ::GetFocus() == hwnd;
      }
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_MOUSEMOVE: {
      SliderState* state = slider_state(hwnd);
      if (state == nullptr) {
        break;
      }
      if (!state->hover) {
        state->hover = true;
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        ::TrackMouseEvent(&track);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      if (state->dragging) {
        slider_set_from_x(hwnd, *state, GET_X_LPARAM(lparam));
        slider_notify(hwnd, *state, false);
      }
      return 0;
    }

    case WM_MOUSELEAVE: {
      SliderState* state = slider_state(hwnd);
      if (state != nullptr && state->hover) {
        state->hover = false;
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_LBUTTONDOWN: {
      SliderState* state = slider_state(hwnd);
      if (state == nullptr || ::IsWindowEnabled(hwnd) == FALSE) {
        break;
      }
      ::SetFocus(hwnd);
      ::SetCapture(hwnd);
      state->dragging = true;
      slider_set_from_x(hwnd, *state, GET_X_LPARAM(lparam));
      slider_notify(hwnd, *state, false);
      return 0;
    }

    case WM_LBUTTONUP: {
      SliderState* state = slider_state(hwnd);
      if (state == nullptr || !state->dragging) {
        break;
      }
      slider_set_from_x(hwnd, *state, GET_X_LPARAM(lparam));
      state->dragging = false;
      ::ReleaseCapture();
      ::InvalidateRect(hwnd, nullptr, FALSE);
      slider_notify(hwnd, *state, true);
      return 0;
    }

    case WM_CAPTURECHANGED: {
      SliderState* state = slider_state(hwnd);
      if (state != nullptr && state->dragging) {
        state->dragging = false;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        slider_notify(hwnd, *state, true);
      }
      return 0;
    }

    case WM_MOUSEWHEEL: {
      SliderState* state = slider_state(hwnd);
      if (state == nullptr || ::IsWindowEnabled(hwnd) == FALSE) {
        break;
      }
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      state->pos += (delta > 0 ? slider_step(*state) : -slider_step(*state));
      slider_clamp(*state);
      ::InvalidateRect(hwnd, nullptr, FALSE);
      slider_notify(hwnd, *state, false);
      slider_notify(hwnd, *state, true);
      return 0;
    }

    case WM_KEYDOWN: {
      SliderState* state = slider_state(hwnd);
      if (state == nullptr || ::IsWindowEnabled(hwnd) == FALSE) {
        break;
      }
      const int step = slider_step(*state);
      bool handled = true;
      switch (wparam) {
        case VK_LEFT:
        case VK_DOWN:
          state->pos -= step;
          break;
        case VK_RIGHT:
        case VK_UP:
          state->pos += step;
          break;
        case VK_PRIOR:
          state->pos += step * 10;
          break;
        case VK_NEXT:
          state->pos -= step * 10;
          break;
        case VK_HOME:
          state->pos = state->min;
          break;
        case VK_END:
          state->pos = state->max;
          break;
        default:
          handled = false;
          break;
      }
      if (!handled) {
        break;
      }
      slider_clamp(*state);
      ::InvalidateRect(hwnd, nullptr, FALSE);
      slider_notify(hwnd, *state, false);
      slider_notify(hwnd, *state, true);
      return 0;
    }

    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;

    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT && ::IsWindowEnabled(hwnd) != FALSE) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;

    case TBM_SETRANGE: {
      SliderState* state = slider_state(hwnd);
      if (state == nullptr) {
        break;
      }
      const int minimum = LOWORD(lparam);
      const int maximum = HIWORD(lparam);
      state->min = std::min(minimum, maximum);
      state->max = std::max(minimum, maximum);
      slider_clamp(*state);
      if (wparam != 0) {
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case TBM_SETPOS: {
      SliderState* state = slider_state(hwnd);
      if (state == nullptr) {
        break;
      }
      if (wparam != 0) {
        state->pos = static_cast<int>(lparam);
        slider_clamp(*state);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case TBM_GETPOS: {
      SliderState* state = slider_state(hwnd);
      return state == nullptr ? 0 : state->pos;
    }

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

struct MeterState {
  float target_raw = -120.0f;
  float target_out = -120.0f;
  float shown_raw = -120.0f;
  float shown_out = -120.0f;
  bool bypass = false;
  bool animating = false;
};

MeterState* meter_state(HWND hwnd) {
  return reinterpret_cast<MeterState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

float meter_norm(float db) {
  return std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
}

std::wstring meter_value_text(float db) {
  wchar_t text[32]{};
  if (db <= -119.5f) {
    swprintf_s(text, L"-120");
  } else {
    swprintf_s(text, L"%.0f", static_cast<double>(db));
  }
  return text;
}

void paint_meter_frame(const MeterState& state, HDC dc, int width, int height) {
  // 两行紧凑对比：左标签、中条形、右数值。
  // 控件本身与内容列同宽，因此内边距取 0：标签左沿、数值右沿与整窗内容列对齐。
  const int row_top[2] = {2, 26};
  const int row_height = 24;
  const float bar_x = 50.0f;
  const float value_width = 52.0f;
  const float value_gap = 6.0f;
  const float bar_right = static_cast<float>(width) - value_width;
  const float bar_width = std::max(24.0f, bar_right - value_gap - bar_x);
  const float values[2] = {state.shown_raw, state.shown_out};

  {
    Graphics graphics(dc);
    // 同滑杆：底色用无抗锯齿铺满，避免贴边像素混色出现白边。
    graphics.SetSmoothingMode(SmoothingModeNone);
    SolidBrush background(kBackgroundColor);
    graphics.FillRectangle(&background, 0, 0, width, height);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);

    for (int i = 0; i < 2; ++i) {
      // 条形在行内垂直居中：24px 行高放 6px 条，上下各留 9px。
      const float y = static_cast<float>(row_top[i]) + 9.0f;
      fill_round(graphics, RectF(bar_x, y, bar_width, 6.0f), 3.0f, kTrackColor);
      const float filled = meter_norm(values[i]) * bar_width;
      if (filled > 0.5f) {
        // 统一使用平涂色，避免渐变造成视觉噪音。
        fill_round(graphics, RectF(bar_x, y, filled, 6.0f), 3.0f,
                   i == 1 ? kAccent : Color(255, 91, 107, 120));
      }
    }
    graphics.Flush();
  }

  draw_text(dc, L"原始", RECT{0, row_top[0], 44, row_top[0] + row_height}, font_small(), kText2,
            DT_LEFT | DT_VCENTER);
  draw_text(dc, L"处理后", RECT{0, row_top[1], 44, row_top[1] + row_height}, font_small(),
            kAccentRef, DT_LEFT | DT_VCENTER);

  const LONG value_left = static_cast<LONG>(bar_right) + static_cast<LONG>(value_gap);
  const RECT raw_value{value_left, row_top[0], width, row_top[0] + row_height};
  const RECT out_value{value_left, row_top[1], width, row_top[1] + row_height};
  draw_text(dc, meter_value_text(state.shown_raw), raw_value, font_small(), kText2,
            DT_RIGHT | DT_VCENTER);
  draw_text(dc, state.bypass ? std::wstring(L"已旁路") : meter_value_text(state.shown_out),
            out_value, font_small(), state.bypass ? kAmberRef : kAccentRef,
            DT_RIGHT | DT_VCENTER);
}

void paint_meter(HWND hwnd, HDC dc) {
  MeterState* state = meter_state(hwnd);
  if (state == nullptr) {
    return;
  }
  paint_buffered(hwnd, dc, [state](HDC target, int width, int height) {
    paint_meter_frame(*state, target, width, height);
  });
}

LRESULT CALLBACK meter_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(new MeterState()));
      return 0;

    case WM_NCDESTROY:
      delete meter_state(hwnd);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return ::DefWindowProcW(hwnd, message, wparam, lparam);

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = ::BeginPaint(hwnd, &paint);
      paint_meter(hwnd, dc);
      ::EndPaint(hwnd, &paint);
      return 0;
    }

    case WM_TIMER: {
      MeterState* state = meter_state(hwnd);
      if (state == nullptr) {
        break;
      }
      state->shown_raw += (state->target_raw - state->shown_raw) * 0.28f;
      state->shown_out += (state->target_out - state->shown_out) * 0.28f;
      const bool done = std::fabs(state->target_raw - state->shown_raw) < 0.4f &&
                        std::fabs(state->target_out - state->shown_out) < 0.4f;
      if (done) {
        state->shown_raw = state->target_raw;
        state->shown_out = state->target_out;
        state->animating = false;
        ::KillTimer(hwnd, 1);
      }
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

const wchar_t* slider_class() { return kSliderClass; }
const wchar_t* meter_class() { return kMeterClass; }

void register_control_classes(HINSTANCE instance) {
  WNDCLASSEXW slider{};
  slider.cbSize = sizeof(slider);
  slider.style = CS_HREDRAW | CS_VREDRAW;
  slider.lpfnWndProc = slider_proc;
  slider.hInstance = instance;
  slider.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  slider.lpszClassName = kSliderClass;
  ::RegisterClassExW(&slider);

  WNDCLASSEXW meter{};
  meter.cbSize = sizeof(meter);
  meter.style = CS_HREDRAW | CS_VREDRAW;
  meter.lpfnWndProc = meter_proc;
  meter.hInstance = instance;
  meter.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  meter.lpszClassName = kMeterClass;
  ::RegisterClassExW(&meter);
}

void set_meter(HWND meter, float raw_db, float processed_db, bool bypass) {
  if (meter == nullptr) {
    return;
  }
  MeterState* state = meter_state(meter);
  if (state == nullptr) {
    return;
  }
  const bool changed = std::fabs(state->target_raw - raw_db) > 0.05f ||
                       std::fabs(state->target_out - processed_db) > 0.05f ||
                       state->bypass != bypass;
  state->target_raw = raw_db;
  state->target_out = processed_db;
  state->bypass = bypass;
  if (!changed) {
    return;
  }
  if (!state->animating) {
    state->animating = true;
    ::SetTimer(meter, 1, 30, nullptr);
  }
  ::InvalidateRect(meter, nullptr, FALSE);
}

#ifdef MICSTUDIO_UI_TESTING
bool meter_snapshot(HWND meter, MeterSnapshot& out) {
  MeterState* state = meter_state(meter);
  if (state == nullptr) {
    return false;
  }
  out.target_raw = state->target_raw;
  out.target_out = state->target_out;
  out.shown_raw = state->shown_raw;
  out.shown_out = state->shown_out;
  out.bypass = state->bypass;
  out.animating = state->animating;
  return true;
}

void meter_paint_to(HWND meter, HDC dc) {
  if (dc == nullptr) {
    return;
  }
  paint_meter(meter, dc);
}

void slider_paint_to(HWND slider, HDC dc) {
  if (dc == nullptr) {
    return;
  }
  paint_slider(slider, dc);
}
#endif

}  // namespace ui
