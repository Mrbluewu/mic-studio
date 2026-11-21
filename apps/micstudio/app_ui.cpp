#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "audio_io/device_enumerator.h"
#include "config/settings.h"
#include "engine/audio_engine.h"
#include "platform/com.h"
#include "platform/default_endpoint.h"
#include "platform/utf8.h"
#include "presets/preset_library.h"
#include "resource.h"
#include "ui_controls.h"
#include "ui_theme.h"

#include "app.h"

using namespace app;

namespace {

const ms::presets::PresetTemplate& current_preset() {
  return g_presets[static_cast<std::size_t>(g_preset_index)];
}

void apply_settings_to_engine() {
  g_engine.set_denoise_amount(g_settings.denoise_amount);
  g_engine.set_input_gain_db(g_settings.gain_db);
  g_engine.set_output_gain_db(g_settings.output_gain_db);
  g_engine.set_effects(g_settings.clarity, g_settings.body, g_settings.leveler, g_settings.deesser);
}

// 高级区整体显示/隐藏：收起时只保留主流程需要的控件。
void apply_advanced_visibility() {
  const int command = g_advanced ? SW_SHOW : SW_HIDE;
  auto toggle = [command](HWND control) {
    if (control != nullptr) {
      ::ShowWindow(control, command);
    }
  };

  for (HWND button : g_preset_buttons) {
    toggle(button);
  }
  toggle(g_compare_button);
  toggle(g_gain_slider);
  toggle(g_gain_value);
  toggle(g_out_gain_slider);
  toggle(g_out_gain_value);
  toggle(g_meter);
  for (HWND check : g_effect_checks) {
    toggle(check);
  }
  for (HWND check : g_option_checks) {
    toggle(check);
  }
  toggle(g_system_sound_button);
  for (HWND button : g_plugin_slot_button) {
    toggle(button);
  }
}

}  // namespace

namespace app {

void set_control_text(HWND control, const std::wstring& text) {
  ::SetWindowTextW(control, text.c_str());
}

void update_status() {
  const auto stats = g_engine.stats();
  const auto& preset = current_preset();

  g_latency_ms = stats.latency_ms;
  ui::set_meter(g_meter, stats.noise_floor_in_db, stats.noise_floor_out_db, g_bypass);
  if (g_window != nullptr) {
    ::InvalidateRect(g_window, &g_layout.header, FALSE);
    ::InvalidateRect(g_window, &g_layout.footer, FALSE);
  }

  if (g_tray_added) {
    wchar_t tip[128]{};
    swprintf_s(tip, L"Mic Studio · %s%s｜底噪 %.0f dB",
               ms::platform::from_utf8(preset.name).c_str(), g_bypass ? L"（已旁路）" : L"",
               stats.noise_floor_out_db);
    wcscpy_s(g_icon.szTip, tip);
    g_icon.uFlags = NIF_TIP;
    ::Shell_NotifyIconW(NIM_MODIFY, &g_icon);
  }
}

void highlight_preset(int index) {
  for (std::size_t i = 0; i < g_preset_buttons.size(); ++i) {
    ::SendMessageW(g_preset_buttons[i], BM_SETCHECK,
                   static_cast<WPARAM>(i == static_cast<std::size_t>(index) ? BST_CHECKED
                                                                          : BST_UNCHECKED),
                   0);
    ::InvalidateRect(g_preset_buttons[i], nullptr, FALSE);
  }
}

// 把设置里的增益/降噪/效果卡状态同步到控件，不触发 WM_HSCROLL。
void sync_treatment_controls() {
  if (g_gain_slider != nullptr) {
    const float gain = std::clamp(g_settings.gain_db, -60.0f, 60.0f);
    ::SendMessageW(g_gain_slider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::lround(gain * 10.0f) + 600));
    wchar_t text[32]{};
    swprintf_s(text, L"%+.1f dB", static_cast<double>(gain));
    set_control_text(g_gain_value, text);
  }
  if (g_out_gain_slider != nullptr) {
    const float gain = std::clamp(g_settings.output_gain_db, -60.0f, 60.0f);
    ::SendMessageW(g_out_gain_slider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::lround(gain * 10.0f) + 600));
    wchar_t text[32]{};
    swprintf_s(text, L"%+.1f dB", static_cast<double>(gain));
    set_control_text(g_out_gain_value, text);
  }
  if (g_denoise_slider != nullptr) {
    const float amount = std::clamp(g_settings.denoise_amount, 0.0f, 1.0f);
    ::SendMessageW(g_denoise_slider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::lround(amount * 100.0f)));
    wchar_t text[32]{};
    swprintf_s(text, L"%.0f%%", static_cast<double>(amount * 100.0f));
    set_control_text(g_denoise_value, text);
  }
  const bool effects[4] = {g_settings.clarity, g_settings.body, g_settings.leveler,
                           g_settings.deesser};
  for (int i = 0; i < 4; ++i) {
    if (g_effect_checks[i] != nullptr) {
      ::SendMessageW(g_effect_checks[i], BM_SETCHECK,
                     effects[i] ? BST_CHECKED : BST_UNCHECKED, 0);
      ::InvalidateRect(g_effect_checks[i], nullptr, FALSE);
    }
  }

  const bool options[3] = {g_settings.builtin_denoise, g_settings.start_with_windows,
                           g_settings.start_minimized};
  for (int i = 0; i < 3; ++i) {
    if (g_option_checks[i] != nullptr) {
      ::SendMessageW(g_option_checks[i], BM_SETCHECK,
                     options[i] ? BST_CHECKED : BST_UNCHECKED, 0);
      ::InvalidateRect(g_option_checks[i], nullptr, FALSE);
    }
  }
  if (g_denoise_slider != nullptr) {
    ::EnableWindow(g_denoise_slider, g_settings.builtin_denoise ? TRUE : FALSE);
    ::InvalidateRect(g_denoise_slider, nullptr, FALSE);
  }
}

void select_preset(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= g_presets.size()) {
    return;
  }
  g_preset_index = index;
  highlight_preset(index);

  const auto& preset = current_preset();
  g_settings.preset_id = preset.id;
  // 场景只改降噪强度，不覆盖用户手动设置的输入增益。
  g_settings.denoise_amount = preset.denoise_amount;
  apply_settings_to_engine();
  sync_treatment_controls();
  g_settings.save();
  update_status();
}

void toggle_advanced() {
  g_advanced = !g_advanced;
  apply_advanced_visibility();
  apply_layout(true);
}

void apply_layout(bool resize_window) {
  if (g_window == nullptr || g_in_layout) {
    return;
  }
  g_in_layout = true;

  // 客户区宽度固定，布局只随“是否展开高级区”纵向变化。
  g_layout = ui::compute_layout(g_advanced);

  auto move = [](HWND control, const RECT& rect, int height_override = 0) {
    if (control == nullptr) {
      return;
    }
    const int height = height_override > 0 ? height_override : rect.bottom - rect.top;
    ::SetWindowPos(control, nullptr, rect.left, rect.top, rect.right - rect.left, height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
  };

  // 数值标签：布局给出的是紧凑的行高，直接套用会把 12px 字体裁掉上下边。
  // 统一给 20px 高度、并围绕原中心对齐，文字位置与行距都保持不变。
  constexpr int kTextRowHeight = 20;
  auto move_text = [](HWND control, const RECT& rect) {
    if (control == nullptr) {
      return;
    }
    const int height = rect.bottom - rect.top;
    const int top = rect.top + height / 2 - kTextRowHeight / 2;
    ::SetWindowPos(control, nullptr, rect.left, top, rect.right - rect.left, kTextRowHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
  };

  move(g_input_button, g_layout.device_input);
  move(g_output_button, g_layout.device_output);
  move(g_power_button, g_layout.power_button);
  move(g_advanced_button, g_layout.advanced_toggle);
  for (int i = 0; i < 4 && i < static_cast<int>(g_preset_buttons.size()); ++i) {
    move(g_preset_buttons[static_cast<std::size_t>(i)], g_layout.preset[i]);
  }
  move(g_compare_button, g_layout.compare);
  move(g_gain_slider, g_layout.gain_slider);
  move_text(g_gain_value, g_layout.gain_value);
  move(g_out_gain_slider, g_layout.out_gain_slider);
  move_text(g_out_gain_value, g_layout.out_gain_value);
  move(g_denoise_slider, g_layout.denoise_slider);
  move_text(g_denoise_value, g_layout.denoise_value);
  for (int i = 0; i < 4; ++i) {
    move(g_effect_checks[i], g_layout.effect[i]);
  }
  for (int i = 0; i < 3; ++i) {
    move(g_option_checks[i], g_layout.option[i]);
  }
  move(g_system_sound_button, g_layout.system_sound);
  move(g_meter, g_layout.meter);
  for (int slot = 0; slot < 3; ++slot) {
    move(g_plugin_slot_button[slot], g_layout.plugin_slot[slot]);
  }

  // 固定宽度窗口：只在展开/收起时调整高度。
  if (resize_window && ::IsZoomed(g_window) == FALSE) {
    RECT frame{0, 0, kWindowWidth, g_layout.client_height};
    const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(g_window, GWL_STYLE));
    const DWORD exstyle = static_cast<DWORD>(::GetWindowLongPtrW(g_window, GWL_EXSTYLE));
    ::AdjustWindowRectEx(&frame, style, FALSE, exstyle);
    ::SetWindowPos(g_window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  g_in_layout = false;
  ::InvalidateRect(g_window, nullptr, FALSE);
}

void create_controls(HWND hwnd) {
  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  const HFONT body_font = ui::font_body();
  const HFONT value_font = ui::font_value();

  auto make_button = [&](const wchar_t* text, int id) {
    HWND button = ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0,
                                    10, 10, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                                    nullptr);
    ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(body_font), TRUE);
    ui::enable_hover(button);
    return button;
  };

  auto label = [&](const wchar_t* text, HFONT font, DWORD style = WS_CHILD | WS_VISIBLE) {
    HWND control = ::CreateWindowExW(0, L"STATIC", text, style, 0, 0, 10, 10, hwnd, nullptr,
                                     instance, nullptr);
    ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
  };

  g_input_button = make_button(L"", kInputButton);
  g_output_button = make_button(L"", kOutputButton);

  for (std::size_t i = 0; i < g_presets.size(); ++i) {
    HWND button = make_button(ms::platform::from_utf8(g_presets[i].name).c_str(),
                              kPresetButtonBase + static_cast<int>(i));
    g_preset_buttons.push_back(button);
  }

  g_compare_button = make_button(L"对比原声 2 秒", kCompareButton);
  g_power_button = make_button(L"", kPowerButton);
  g_advanced_button = make_button(L"高级设置", kAdvancedButton);
  g_system_sound_button = make_button(L"打开系统声音面板", kSystemSoundButton);

  g_gain_slider = ::CreateWindowExW(0, ui::slider_class(), L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    0, 0, 10, 28, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGainSlider)),
                                    instance, nullptr);
  ::SendMessageW(g_gain_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1200));
  g_gain_value = label(L"+0.0 dB", value_font);

  g_out_gain_slider = ::CreateWindowExW(
      0, ui::slider_class(), L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 28, hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutputGainSlider)), instance, nullptr);
  ::SendMessageW(g_out_gain_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1200));
  g_out_gain_value = label(L"+0.0 dB", value_font);

  g_denoise_slider = ::CreateWindowExW(
      0, ui::slider_class(), L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 28, hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDenoiseSlider)), instance, nullptr);
  ::SendMessageW(g_denoise_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
  g_denoise_value = label(L"0%", value_font);

  const wchar_t* kEffectNames[4] = {L"更清晰", L"更厚实", L"音量稳定", L"去齿音"};
  for (int i = 0; i < 4; ++i) {
    g_effect_checks[i] = make_button(kEffectNames[i], kEffectBase + i);
  }

  const wchar_t* kOptionNames[3] = {L"内置降噪", L"随系统启动", L"启动时最小化"};
  for (int i = 0; i < 3; ++i) {
    g_option_checks[i] = make_button(kOptionNames[i], kOptionBase + i);
  }

  for (int slot = 0; slot < 3; ++slot) {
    g_plugin_slot_button[slot] = make_button(L"", kPluginSlotBase + slot);
  }

  g_meter = ::CreateWindowExW(0, ui::meter_class(), L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                              nullptr, instance, nullptr);

  apply_advanced_visibility();
}

void release_back_buffer() {
  if (g_back_dc != nullptr) {
    if (g_back_old != nullptr) {
      ::SelectObject(g_back_dc, g_back_old);
      g_back_old = nullptr;
    }
    ::DeleteDC(g_back_dc);
    g_back_dc = nullptr;
  }
  if (g_back_bitmap != nullptr) {
    ::DeleteObject(g_back_bitmap);
    g_back_bitmap = nullptr;
  }
  g_back_size = SIZE{0, 0};
}

void paint_client(HWND hwnd, HDC dc) {
  ui::HeadState state;
  state.bypass = g_bypass;
  state.advanced = g_advanced;
  state.latency_ms = g_latency_ms;
  state.footer_line = g_footer_line;
  ui::paint_all(dc, hwnd, g_layout, state);
}

}  // namespace app
