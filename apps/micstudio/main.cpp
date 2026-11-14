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

// 启用 Common Controls v6，深色下拉框与自绘按钮依赖它。
#pragma comment(linker, \
                "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                "language='*'\"")

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
// 第二个实例启动时发给已有实例，让它把自己显示出来。
constexpr UINT kWakeMessage = WM_APP + 2;
constexpr UINT kMenuPresetBase = 1000;
constexpr UINT kMenuBypass = 1100;
constexpr UINT kMenuShow = 1101;
constexpr UINT kMenuExit = 1102;
constexpr UINT_PTR kTimerStatus = 1;
constexpr UINT_PTR kTimerExit = 2;
constexpr UINT_PTR kTimerCompare = 3;
constexpr UINT_PTR kTimerEditor = 4;
constexpr UINT_PTR kTimerSync = 5;
constexpr UINT kIconId = 1;
// 单实例互斥体名（Local 作用域：同一用户会话内唯一即可）。
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MicStudio.SingleInstance";

constexpr int kInputButton = 100;
constexpr int kOutputButton = 101;
constexpr int kGainSlider = 110;
constexpr int kDenoiseSlider = 111;
constexpr int kParamTabBase = 112;
constexpr int kOutputGainSlider = 113;
constexpr int kCompareButton = 200;
constexpr int kPowerButton = 201;
constexpr int kAdvancedButton = 202;
constexpr int kPluginSlotBase = 210;
constexpr int kEffectBase = 220;
constexpr int kOptionBase = 230;
constexpr int kPresetButtonBase = 300;
constexpr int kPluginSliderBase = 400;

// 参数面板每槽最多显示的行数；超出部分插件仍按默认值工作。
constexpr int kPluginRowHeight = 26;
// 客户区固定 300px：与设计稿一致，窗口不可拉伸。
constexpr int kWindowWidth = 300;

HWND g_window = nullptr;
HWND g_input_button = nullptr;
HWND g_output_button = nullptr;
HWND g_compare_button = nullptr;
HWND g_power_button = nullptr;
HWND g_advanced_button = nullptr;
HWND g_gain_slider = nullptr;
HWND g_gain_value = nullptr;
HWND g_out_gain_slider = nullptr;
HWND g_out_gain_value = nullptr;
HWND g_denoise_slider = nullptr;
HWND g_denoise_value = nullptr;
HWND g_effect_checks[4]{};
HWND g_option_checks[3]{};
HWND g_plugin_slot_button[3]{};
HWND g_param_tabs[3]{};
HWND g_meter = nullptr;
int g_input_endpoint = -1;
int g_output_endpoint = -1;
int g_active_param_group = 0;
HWND g_editor_window = nullptr;
int g_editor_width = 0;
int g_editor_height = 0;
int g_editor_slot = -1;
std::vector<HWND> g_preset_buttons;

ui::Layout g_layout{};
std::size_t g_shown_rows[3] = {0, 0, 0};
bool g_note_rows[3] = {false, false, false};
std::wstring g_footer_line;
double g_latency_ms = 0.0;
bool g_in_layout = false;
HDC g_back_dc = nullptr;
HBITMAP g_back_bitmap = nullptr;
HBITMAP g_back_old = nullptr;
SIZE g_back_size{0, 0};

// 参数面板：名称 / 滑杆 / 百分比三列，按插件参数动态创建。
// g_param_groups 按槽分组，用于页签切换时整体显示/隐藏。
std::vector<HWND> g_param_groups[3];
std::vector<HWND> g_param_sliders;
std::vector<HWND> g_param_value_labels;
std::vector<std::size_t> g_param_slider_index;
std::vector<ms::engine::PluginParameterInfo> g_plugin_params;
std::wstring g_plugin_error[3];
bool g_plugin_failed[3] = {false, false, false};
bool g_plugin_params_dirty = false;

NOTIFYICONDATAW g_icon{};
ms::engine::AudioEngine g_engine;
ms::config::Settings g_settings;
std::vector<ms::presets::PresetTemplate> g_presets;
std::vector<ms::audio_io::EndpointInfo> g_inputs;
std::vector<ms::audio_io::EndpointInfo> g_outputs;
int g_preset_index = 0;
bool g_bypass = false;
// 高级区默认收起：主界面只留下开关、设备、降噪强度与底噪对比。
bool g_advanced = false;
bool g_tray_added = false;
bool g_default_switched = false;
std::wstring g_previous_default;

const ms::presets::PresetTemplate& current_preset() {
  return g_presets[static_cast<std::size_t>(g_preset_index)];
}

int resolve_endpoint(const std::vector<ms::audio_io::EndpointInfo>& items, const std::wstring& id,
                     const std::wstring& name) {
  if (!id.empty()) {
    for (const auto& item : items) {
      if (item.id == id) {
        return static_cast<int>(item.index);
      }
    }
  }
  if (!name.empty()) {
    for (const auto& item : items) {
      if (item.friendlyName == name) {
        return static_cast<int>(item.index);
      }
    }
  }
  return -1;
}

int endpoint_position(const std::vector<ms::audio_io::EndpointInfo>& items, int endpoint_index) {
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (static_cast<int>(items[i].index) == endpoint_index) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

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

void apply_settings_to_engine() {
  g_engine.set_denoise_amount(g_settings.denoise_amount);
  g_engine.set_input_gain_db(g_settings.gain_db);
  g_engine.set_output_gain_db(g_settings.output_gain_db);
  g_engine.set_effects(g_settings.clarity, g_settings.body, g_settings.leveler, g_settings.deesser);
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

std::wstring file_name_of(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::vector<ms::engine::PluginParameter> parse_plugin_params(const std::wstring& text) {
  std::vector<ms::engine::PluginParameter> result;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find(L';', start);
    const std::wstring item =
        text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    const std::size_t equals = item.find(L'=');
    if (equals != std::wstring::npos) {
      ms::engine::PluginParameter parameter;
      parameter.index = ::_wtoi(item.substr(0, equals).c_str());
      parameter.value = std::clamp(
          static_cast<float>(::_wtof(item.substr(equals + 1).c_str())), 0.0f, 1.0f);
      result.push_back(parameter);
    }
    if (end == std::wstring::npos) {
      break;
    }
    start = end + 1;
  }
  return result;
}

// 内置 DeepFilterNet 随程序分发：内置降噪固定使用 exe 同目录的 DLL。
std::wstring deepfilter_path() {
  wchar_t exe[MAX_PATH]{};
  ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring directory(exe);
  const std::size_t slash = directory.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return L"DeepFilterNet.dll";
  }
  directory.resize(slash);
  return directory + L"\\DeepFilterNet.dll";
}

void apply_plugin_options(ms::engine::EngineOptions& options) {
  options.builtin_denoise = g_settings.builtin_denoise;
  options.deepfilter_path = deepfilter_path();
  // 固定 3 个槽，保持与设置/UI 的槽号一一对应；空路径由引擎跳过。
  options.plugins.clear();
  for (int i = 0; i < 3; ++i) {
    const auto& slot = g_settings.plugin_slots[i];
    options.plugins.push_back(ms::engine::PluginSlot{
        slot.vst3, slot.path, parse_plugin_params(slot.params)});
  }
}

void log_line(const std::string& text);

// 带插件启动：单个槽加载失败由引擎跳过并记录，不影响其它槽。
ms::platform::Error start_engine_with_plugin(const ms::engine::EngineOptions& base_options) {
  for (int i = 0; i < 3; ++i) {
    g_plugin_error[i].clear();
  }

  ms::engine::EngineOptions options = base_options;
  apply_plugin_options(options);
  auto error = g_engine.start(options);
  if (!error.ok()) {
    return error;
  }

  const auto slot_errors = g_engine.plugin_slot_errors();
  for (int i = 0; i < 3; ++i) {
    if (i < static_cast<int>(slot_errors.size())) {
      g_plugin_error[i] = slot_errors[static_cast<std::size_t>(i)];
    }
    if (!g_plugin_error[i].empty()) {
      log_line("插件槽 " + std::to_string(i + 1) +
               " 加载失败：" + ms::platform::to_utf8(g_plugin_error[i]));
    }
  }
  return error;
}

void destroy_plugin_panel() {
  for (auto& group : g_param_groups) {
    for (HWND control : group) {
      ::DestroyWindow(control);
    }
    group.clear();
  }
  g_param_sliders.clear();
  g_param_value_labels.clear();
  g_param_slider_index.clear();
}

std::wstring encode_plugin_params(int slot) {
  std::wstring text;
  for (const auto& parameter : g_plugin_params) {
    if (parameter.slot != slot) {
      continue;
    }
    wchar_t item[64]{};
    swprintf_s(item, L"%d=%.6f", parameter.id, parameter.value);
    if (!text.empty()) {
      text.push_back(L';');
    }
    text += item;
  }
  return text;
}

void save_plugin_params() {
  for (int slot = 0; slot < 3; ++slot) {
    g_settings.plugin_slots[slot].params = encode_plugin_params(slot);
  }
  g_settings.save();
}

void apply_layout(std::size_t (&shown)[3], bool (&note)[3], bool resize_window);
void rebuild_plugin_panel();
void restore_compare_button();

void show_param_group(int group) {
  for (int slot = 0; slot < 3; ++slot) {
    for (HWND control : g_param_groups[slot]) {
      ::ShowWindow(control, g_advanced && slot == group ? SW_SHOW : SW_HIDE);
    }
  }
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
  for (int slot = 0; slot < 3; ++slot) {
    toggle(g_plugin_slot_button[slot]);
    toggle(g_param_tabs[slot]);
  }

  if (g_advanced) {
    show_param_group(g_active_param_group);
  } else {
    for (auto& group : g_param_groups) {
      for (HWND control : group) {
        ::ShowWindow(control, SW_HIDE);
      }
    }
  }
}

void toggle_advanced() {
  g_advanced = !g_advanced;
  apply_advanced_visibility();
  apply_layout(g_shown_rows, g_note_rows, true);
  // 参数行的位置依赖展开后的布局，收起/展开时重建一次以保证坐标正确。
  rebuild_plugin_panel();
}

void select_param_group(int group) {
  if (group < 0 || group > 2) {
    return;
  }
  g_active_param_group = group;
  for (int i = 0; i < 3; ++i) {
    if (g_param_tabs[i] != nullptr) {
      ::SendMessageW(g_param_tabs[i], BM_SETCHECK, i == group ? BST_CHECKED : BST_UNCHECKED, 0);
      ::InvalidateRect(g_param_tabs[i], nullptr, FALSE);
    }
  }
  show_param_group(group);
}

void apply_layout(std::size_t (&shown)[3], bool (&note)[3], bool resize_window) {
  if (g_window == nullptr || g_in_layout) {
    return;
  }
  g_in_layout = true;

  // 客户区宽度固定，布局只随“是否展开高级区”纵向变化。
  g_layout = ui::compute_layout(shown, note, g_advanced);
  for (int i = 0; i < 3; ++i) {
    g_shown_rows[i] = shown[i];
    g_note_rows[i] = note[i];
  }

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
  move(g_meter, g_layout.meter);
  for (int slot = 0; slot < 3; ++slot) {
    move(g_plugin_slot_button[slot], g_layout.plugin_slot[slot]);
    move(g_param_tabs[slot], g_layout.param_tab[slot]);
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

void rebuild_plugin_panel() {
  if (g_plugin_slot_button[0] == nullptr || g_param_tabs[0] == nullptr) {
    return;
  }

  destroy_plugin_panel();
  g_plugin_params = g_engine.plugin_parameters();

  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  const HFONT name_font = ui::font_small();
  const HFONT value_font = ui::font_value();

  for (int slot = 0; slot < 3; ++slot) {
    const auto& config = g_settings.plugin_slots[slot];
    std::wstring title;
    if (config.path.empty()) {
      title.clear();
    } else if (!g_plugin_error[slot].empty()) {
      title = L"加载失败";
    } else {
      title = file_name_of(config.path);
    }
    set_control_text(g_plugin_slot_button[slot], title);
    // 加载失败时由自绘按钮显示琥珀色文字。
    g_plugin_failed[slot] = !g_plugin_error[slot].empty();
    ::InvalidateRect(g_plugin_slot_button[slot], nullptr, FALSE);
  }

  std::size_t counts[3] = {0, 0, 0};
  for (const auto& parameter : g_plugin_params) {
    if (parameter.slot >= 0 && parameter.slot < 3) {
      ++counts[parameter.slot];
    }
  }

  const int max_rows = ui::max_param_rows();
  std::size_t shown[3] = {0, 0, 0};
  bool note[3] = {false, false, false};
  for (int slot = 0; slot < 3; ++slot) {
    shown[slot] = std::min(counts[slot], static_cast<std::size_t>(max_rows));
    note[slot] = counts[slot] > static_cast<std::size_t>(max_rows);
  }
  apply_layout(shown, note, true);

  // 参数行：名称固定宽度，滑杆占满剩余空间，数值右对齐。
  constexpr int kParamNameWidth = 88;
  constexpr int kParamValueWidth = 40;
  const int param_right = g_layout.param_label.right;
  const int slider_left = g_layout.param_row_origin.x + kParamNameWidth + 4;
  const int value_left = param_right - kParamValueWidth;
  const int slider_width = value_left - 6 - slider_left;

  for (int slot = 0; slot < 3; ++slot) {
    auto& group = g_param_groups[slot];
    int y = g_layout.param_row_origin.y;
    std::size_t created = 0;
    for (std::size_t i = 0; i < g_plugin_params.size() && created < shown[slot]; ++i) {
      const auto& parameter = g_plugin_params[i];
      if (parameter.slot != slot) {
        continue;
      }
      const float value = std::clamp(parameter.value, 0.0f, 1.0f);

      HWND name = ::CreateWindowExW(0, L"STATIC", parameter.name.c_str(),
                                    WS_CHILD | SS_ENDELLIPSIS | SS_LEFT | SS_CENTERIMAGE,
                                    g_layout.param_row_origin.x, y, kParamNameWidth, 20, g_window,
                                    nullptr, instance, nullptr);
      ::SendMessageW(name, WM_SETFONT, reinterpret_cast<WPARAM>(name_font), TRUE);
      group.push_back(name);

      HWND slider = ::CreateWindowExW(
          0, ui::slider_class(), L"", WS_CHILD | WS_TABSTOP, slider_left, y - 3, slider_width,
          26, g_window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPluginSliderBase) +
                                  static_cast<INT_PTR>(slot) * 100 + static_cast<INT_PTR>(created)),
          instance, nullptr);
      ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
      ::SendMessageW(slider, TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(std::lround(value * 1000.0f)));
      group.push_back(slider);
      g_param_sliders.push_back(slider);
      g_param_slider_index.push_back(i);

      wchar_t text[16]{};
      swprintf_s(text, L"%.0f%%", static_cast<double>(value * 100.0f));
      HWND value_label =
          ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | SS_RIGHT | SS_CENTERIMAGE, value_left,
                            y, kParamValueWidth, 20, g_window, nullptr, instance, nullptr);
      ::SendMessageW(value_label, WM_SETFONT, reinterpret_cast<WPARAM>(value_font), TRUE);
      group.push_back(value_label);
      g_param_value_labels.push_back(value_label);

      y += kPluginRowHeight;
      ++created;
    }

    if (shown[slot] == 0) {
      const wchar_t* hint = g_settings.plugin_slots[slot].path.empty() ? L"未加载插件"
                                                                      : L"该插件没有可调参数";
      HWND empty = ::CreateWindowExW(0, L"STATIC", hint, WS_CHILD,
                                     g_layout.param_row_origin.x, y,
                                     param_right - g_layout.param_row_origin.x, 20, g_window,
                                     nullptr, instance, nullptr);
      ::SendMessageW(empty, WM_SETFONT, reinterpret_cast<WPARAM>(name_font), TRUE);
      group.push_back(empty);
      continue;
    }

    if (note[slot]) {
      wchar_t more_text[64]{};
      swprintf_s(more_text, L"还有 %llu 个参数未显示",
                 static_cast<unsigned long long>(counts[slot] - shown[slot]));
      HWND more = ::CreateWindowExW(0, L"STATIC", more_text, WS_CHILD,
                                    g_layout.param_row_origin.x, y,
                                    param_right - g_layout.param_row_origin.x, 20, g_window,
                                    nullptr, instance, nullptr);
      ::SendMessageW(more, WM_SETFONT, reinterpret_cast<WPARAM>(name_font), TRUE);
      group.push_back(more);
    }
  }

  if (counts[g_active_param_group] == 0) {
    for (int slot = 0; slot < 3; ++slot) {
      if (counts[slot] > 0) {
        g_active_param_group = slot;
        break;
      }
    }
  }
  select_param_group(g_active_param_group);
}

void close_plugin_editor_window();

// 重启链路（切换设备时用）。失败时把原因写到底部提示，不退出程序。
bool restart_engine() {
  close_plugin_editor_window();
  if (g_input_endpoint < 0 || g_output_endpoint < 0 ||
      static_cast<std::size_t>(g_input_endpoint) >= g_inputs.size() ||
      static_cast<std::size_t>(g_output_endpoint) >= g_outputs.size()) {
    return false;
  }

  const auto& input = g_inputs[static_cast<std::size_t>(g_input_endpoint)];
  const auto& sink = g_outputs[static_cast<std::size_t>(g_output_endpoint)];

  g_engine.stop();

  ms::engine::EngineOptions options;
  options.input_endpoint_id = input.id;
  options.output_endpoint_id = sink.id;
  options.input_gain_db = g_settings.gain_db;
  options.output_gain_db = g_settings.output_gain_db;
  options.denoise_amount = g_settings.denoise_amount;
  options.clarity = g_settings.clarity;
  options.body = g_settings.body;
  options.leveler = g_settings.leveler;
  options.deesser = g_settings.deesser;

  const auto error = start_engine_with_plugin(options);
  if (!error.ok()) {
    g_footer_line = L"启动失败：" + error.message;
    ::InvalidateRect(g_window, &g_layout.footer, FALSE);
    rebuild_plugin_panel();
    return false;
  }

  g_settings.input_id = input.id;
  g_settings.input_name = input.friendlyName;
  g_settings.output_id = sink.id;
  g_settings.output_name = sink.friendlyName;
  g_settings.save();
  rebuild_plugin_panel();
  update_status();
  return true;
}

void choose_plugin(int slot, bool vst3) {
  constexpr wchar_t kVst2Filter[] =
      L"VST2 插件 (*.dll)\0*.dll\0"
      L"所有文件 (*.*)\0*.*\0\0";
  constexpr wchar_t kVst3Filter[] =
      L"VST3 插件 (*.vst3)\0*.vst3\0"
      L"所有文件 (*.*)\0*.*\0\0";

  wchar_t file[1024]{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = g_window;
  dialog.lpstrFilter = vst3 ? kVst3Filter : kVst2Filter;
  dialog.lpstrFile = file;
  dialog.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
  dialog.lpstrTitle = vst3 ? L"选择 VST3 降噪插件" : L"选择 VST2 降噪插件";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (::GetOpenFileNameW(&dialog) == FALSE) {
    return;
  }

  auto& config = g_settings.plugin_slots[slot];
  // 一个槽只保留一种类型；换类型时清空旧参数。
  config.vst3 = vst3;
  config.path = file;
  config.params.clear();
  g_plugin_error[slot].clear();
  g_active_param_group = slot;
  if (restart_engine()) {
    g_settings.save();
  } else {
    rebuild_plugin_panel();
  }
  log_line(std::string(vst3 ? "已选择 VST3 插件：" : "已选择 VST2 插件：") +
           ms::platform::to_utf8(file));
}

void clear_plugin(int slot) {
  auto& config = g_settings.plugin_slots[slot];
  if (config.path.empty()) {
    return;
  }

  config.path.clear();
  config.params.clear();
  g_plugin_error[slot].clear();
  if (restart_engine()) {
    g_settings.save();
  } else {
    rebuild_plugin_panel();
  }
  log_line("已移除插件槽 " + std::to_string(slot + 1));
}

void toggle_bypass() {
  g_bypass = !g_bypass;
  g_engine.set_bypass(g_bypass);
  // 手动切换开关时终止进行中的 A/B 对比，避免定时器把状态又改回去。
  if (::KillTimer(g_window, kTimerCompare) != 0) {
    g_footer_line.clear();
    restore_compare_button();
  }
  if (g_power_button != nullptr) {
    ::InvalidateRect(g_power_button, nullptr, FALSE);
  }
  // 开关下方的状态文字由父窗口绘制，一起重绘。
  ::InvalidateRect(g_window, &g_layout.power, FALSE);
  update_status();
}

constexpr DWORD kEditorWindowStyle = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;

void resize_editor_window(int width, int height) {
  if (g_editor_window == nullptr || width <= 0 || height <= 0) {
    return;
  }
  RECT rect{0, 0, width, height};
  ::AdjustWindowRectEx(&rect, kEditorWindowStyle, FALSE, 0);
  ::SetWindowPos(g_editor_window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  g_editor_width = width;
  g_editor_height = height;
}

void close_plugin_editor_window() {
  if (g_editor_window == nullptr) {
    return;
  }
  // 先让插件从宿主窗口摘下自己的子窗口，再销毁宿主，避免部分 VST3 崩溃。
  g_engine.close_plugin_editor(g_editor_slot);
  g_editor_slot = -1;
  ::DestroyWindow(g_editor_window);
  g_editor_window = nullptr;
}

LRESULT CALLBACK editor_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  const int editor_slot =
      static_cast<int>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA)) - 1;
  switch (message) {
    case WM_TIMER:
      if (wparam == kTimerEditor && editor_slot >= 0) {
        g_engine.plugin_editor_idle(editor_slot);
        int width = 0;
        int height = 0;
        if (g_engine.plugin_editor_size(editor_slot, width, height) &&
            (width != g_editor_width || height != g_editor_height)) {
          resize_editor_window(width, height);
        }
        return 0;
      }
      break;

    case WM_CLOSE:
      close_plugin_editor_window();
      return 0;

    case WM_DESTROY:
      ::KillTimer(hwnd, kTimerEditor);
      if (g_editor_window == hwnd) {
        g_editor_window = nullptr;
        g_editor_slot = -1;
      }
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

HICON load_app_icon(int size) {
  HICON icon = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr),
                                               MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size,
                                               LR_DEFAULTCOLOR));
  return icon != nullptr ? icon : ::LoadIconW(nullptr, IDI_APPLICATION);
}

void register_editor_window_class() {
  static bool registered = false;
  if (registered) {
    return;
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = editor_window_proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"MicStudioPluginWindow";
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = ui::card_brush();
  wc.hIcon = load_app_icon(::GetSystemMetrics(SM_CXICON));
  wc.hIconSm = load_app_icon(::GetSystemMetrics(SM_CXSMICON));
  registered = ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void open_plugin_editor_window(int slot) {
  if (slot < 0 || slot >= 3) {
    return;
  }
  if (g_editor_window != nullptr) {
    if (g_editor_slot == slot) {
      ::ShowWindow(g_editor_window, SW_SHOWNORMAL);
      ::SetForegroundWindow(g_editor_window);
      return;
    }
    // 同一时间只保留一个插件编辑器。
    close_plugin_editor_window();
  }
  if (!g_engine.plugin_has_editor(slot)) {
    ::MessageBoxW(g_window, L"当前插件没有可用的编辑器窗口。", L"插件界面", MB_ICONINFORMATION);
    return;
  }

  register_editor_window_class();

  const auto& config = g_settings.plugin_slots[slot];
  std::wstring title = config.vst3 ? L"VST3 插件界面" : L"VST2 插件界面";
  if (!config.path.empty()) {
    title += L" - " + file_name_of(config.path);
  }

  g_editor_window = ::CreateWindowExW(
      0, L"MicStudioPluginWindow", title.c_str(), kEditorWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
      720, 480, g_window, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (g_editor_window == nullptr) {
    return;
  }
  ::SetWindowLongPtrW(g_editor_window, GWLP_USERDATA, static_cast<LONG_PTR>(slot) + 1);
  g_editor_slot = slot;
  ui::apply_dwm(g_editor_window);

  int width = 0;
  int height = 0;
  if (!g_engine.open_plugin_editor(slot, g_editor_window, width, height)) {
    ::DestroyWindow(g_editor_window);
    g_editor_window = nullptr;
    g_editor_slot = -1;
    ::MessageBoxW(g_window, L"插件编辑器打开失败。", L"插件界面", MB_ICONWARNING);
    return;
  }

  resize_editor_window(width, height);
  ::SetTimer(g_editor_window, kTimerEditor, 30, nullptr);
  ::ShowWindow(g_editor_window, SW_SHOWNORMAL);
  ::SetForegroundWindow(g_editor_window);
  log_line("已打开插件界面：槽 " + std::to_string(slot + 1));
}

// 把插件界面里的改动回同步到主窗口滑杆。
void sync_plugin_parameter_from_editor() {
  const auto changes = g_engine.take_plugin_parameter_changes();
  if (changes.empty()) {
    return;
  }

  for (const auto& change : changes) {
    // 先更新完整参数快照（即使该参数超出面板显示行数，也要保证保存值正确）。
    for (auto& parameter : g_plugin_params) {
      if (parameter.slot == change.slot && parameter.id == change.id) {
        parameter.value = change.value;
        break;
      }
    }

    for (std::size_t i = 0; i < g_param_sliders.size(); ++i) {
      const std::size_t index = g_param_slider_index[i];
      if (index >= g_plugin_params.size()) {
        continue;
      }
      const auto& parameter = g_plugin_params[index];
      if (parameter.slot != change.slot || parameter.id != change.id) {
        continue;
      }
      if (::GetCapture() == g_param_sliders[i]) {
        continue;  // 用户正在拖动时不要抢拇指
      }
      ::SendMessageW(g_param_sliders[i], TBM_SETPOS, TRUE,
                     static_cast<LPARAM>(std::lround(change.value * 1000.0f)));

      wchar_t text[16]{};
      swprintf_s(text, L"%.0f%%", static_cast<double>(change.value * 100.0f));
      set_control_text(g_param_value_labels[i], text);
      break;
    }
  }
  g_plugin_params_dirty = true;
}

void restore_compare_button() {
  if (g_compare_button == nullptr) {
    return;
  }
  ::EnableWindow(g_compare_button, TRUE);
  ::SetWindowTextW(g_compare_button, L"对比原声 2 秒");
  ::InvalidateRect(g_compare_button, nullptr, FALSE);
}

void start_compare() {
  if (g_bypass) {
    return;
  }
  g_engine.set_bypass(true);
  g_bypass = true;
  ::SetTimer(g_window, kTimerCompare, 2000, nullptr);
  g_footer_line = L"对比中：输出原声，2 秒后恢复";
  // 对比期间禁用按钮，避免重复点击反复重启计时。
  if (g_compare_button != nullptr) {
    ::EnableWindow(g_compare_button, FALSE);
    ::SetWindowTextW(g_compare_button, L"对比中…");
    ::InvalidateRect(g_compare_button, nullptr, FALSE);
  }
  ::InvalidateRect(g_window, &g_layout.header, FALSE);
  ::InvalidateRect(g_window, &g_layout.footer, FALSE);
  ui::set_meter(g_meter, g_engine.stats().noise_floor_in_db, g_engine.stats().noise_floor_out_db,
                true);
}

void show_window() {
  ::ShowWindow(g_window, SW_SHOWNORMAL);
  ::SetForegroundWindow(g_window);
}

// 托盘单击：主界面可见就收起，隐藏（或最小化）就还原。
void toggle_window_visible() {
  if (::IsWindowVisible(g_window) && !::IsIconic(g_window)) {
    ::ShowWindow(g_window, SW_HIDE);
    return;
  }
  show_window();
}

// 随系统启动：写 HKCU\...\Run，值始终指向当前 exe，移动安装目录也不会失效。
bool apply_autostart(bool enabled) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }

  LONG result = ERROR_SUCCESS;
  if (enabled) {
    wchar_t exe[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring value = L"\"" + std::wstring(exe) + L"\"";
    result = ::RegSetValueExW(key, L"MicStudio", 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(value.c_str()),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  } else {
    result = ::RegDeleteValueW(key, L"MicStudio");
    if (result == ERROR_FILE_NOT_FOUND) {
      result = ERROR_SUCCESS;
    }
  }
  ::RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

void cleanup_and_quit() {
  ::KillTimer(g_window, kTimerStatus);
  close_plugin_editor_window();
  g_engine.stop();

  if (g_default_switched && !g_previous_default.empty()) {
    ms::platform::restore_default_capture(g_previous_default);
  }
  if (g_tray_added) {
    ::Shell_NotifyIconW(NIM_DELETE, &g_icon);
    g_tray_added = false;
  }
  ::DestroyWindow(g_window);
  ::PostQuitMessage(0);
}

void show_context_menu() {
  HMENU menu = ::CreatePopupMenu();
  for (std::size_t i = 0; i < g_presets.size(); ++i) {
    UINT flags = MF_STRING;
    if (static_cast<int>(i) == g_preset_index) {
      flags |= MF_CHECKED;
    }
    ::AppendMenuW(menu, flags, kMenuPresetBase + static_cast<UINT>(i),
                  ms::platform::from_utf8(g_presets[i].name).c_str());
  }
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING | (g_bypass ? MF_CHECKED : 0), kMenuBypass, L"旁路（原声）");
  ::AppendMenuW(menu, MF_STRING, kMenuShow, L"显示主窗口");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

  POINT cursor{};
  ::GetCursorPos(&cursor);
  ::SetForegroundWindow(g_window);
  ::TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_window, nullptr);
  ::DestroyMenu(menu);
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

  const wchar_t* kTabNames[3] = {L"槽1", L"槽2", L"槽3"};
  for (int i = 0; i < 3; ++i) {
    g_param_tabs[i] = make_button(kTabNames[i], kParamTabBase + i);
  }

  g_meter = ::CreateWindowExW(0, ui::meter_class(), L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                              nullptr, instance, nullptr);

  apply_advanced_visibility();
}

std::wstring device_name_of(const std::vector<ms::audio_io::EndpointInfo>& items, int position) {
  if (position < 0 || static_cast<std::size_t>(position) >= items.size()) {
    return L"未选择";
  }
  return items[static_cast<std::size_t>(position)].friendlyName;
}

void update_device_buttons() {
  set_control_text(g_input_button, device_name_of(g_inputs, g_input_endpoint));
  set_control_text(g_output_button, device_name_of(g_outputs, g_output_endpoint));
}

// 设备行点击后弹出原生菜单：比组合框更贴近设计稿的窄条行。
void show_device_menu(bool input) {
  const auto& items = input ? g_inputs : g_outputs;
  const int current = input ? g_input_endpoint : g_output_endpoint;
  HMENU menu = ::CreatePopupMenu();
  for (std::size_t i = 0; i < items.size(); ++i) {
    UINT flags = MF_STRING;
    if (static_cast<int>(i) == current) {
      flags |= MF_CHECKED;
    }
    ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(i) + 1, items[i].friendlyName.c_str());
  }

  HWND owner = input ? g_input_button : g_output_button;
  RECT rect{};
  ::GetWindowRect(owner, &rect);
  ::SetForegroundWindow(g_window);
  const int command =
      ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                       rect.left, rect.bottom, 0, g_window, nullptr);
  ::DestroyMenu(menu);

  if (command <= 0 || static_cast<std::size_t>(command) > items.size()) {
    return;
  }
  const int chosen = command - 1;
  if (chosen == current) {
    return;
  }
  if (input) {
    g_input_endpoint = chosen;
  } else {
    g_output_endpoint = chosen;
  }
  update_device_buttons();
  restart_engine();
}

void show_plugin_menu(int slot) {
  if (slot < 0 || slot >= 3) {
    return;
  }
  const auto& config = g_settings.plugin_slots[slot];
  const bool loaded = !config.path.empty();
  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING, 1, loaded ? L"更换为 VST2 插件…" : L"添加 VST2 插件…");
  ::AppendMenuW(menu, MF_STRING, 2, loaded ? L"更换为 VST3 插件…" : L"添加 VST3 插件…");
  ::AppendMenuW(menu, MF_STRING | (loaded ? 0 : MF_GRAYED), 3, L"打开插件界面");
  ::AppendMenuW(menu, MF_STRING | (loaded ? 0 : MF_GRAYED), 4, L"移除插件");

  HWND owner = g_plugin_slot_button[slot];
  RECT rect{};
  ::GetWindowRect(owner, &rect);
  ::SetForegroundWindow(g_window);
  const int command =
      ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                       rect.left, rect.bottom, 0, g_window, nullptr);
  ::DestroyMenu(menu);

  if (command == 1) {
    choose_plugin(slot, false);
  } else if (command == 2) {
    choose_plugin(slot, true);
  } else if (command == 3) {
    open_plugin_editor_window(slot);
  } else if (command == 4) {
    clear_plugin(slot);
  }
}

// 无控制台后，日志写文件 + 调试器输出，现场排障靠它。
// 注意：文本本身已是 UTF-8，直接按字节写入，不使用 ccs 转换模式。
void log_line(const std::string& text) {
  const std::wstring directory = ms::config::Settings::directory();
  ::CreateDirectoryW(directory.c_str(), nullptr);
  const std::wstring path = directory + L"\\micstudio.log";

  FILE* file = nullptr;
  if (::_wfopen_s(&file, path.c_str(), L"a") == 0 && file != nullptr) {
    std::fwrite(text.data(), 1, text.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
  }
  ::OutputDebugStringA((text + "\n").c_str());
}

constexpr COLORREF kText2Color = RGB(164, 182, 194);
constexpr COLORREF kAccentColor = RGB(52, 224, 192);
constexpr COLORREF kDangerColor = RGB(240, 113, 103);

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

bool is_value_label(HWND control) {
  if (control == g_gain_value || control == g_out_gain_value ||
      control == g_denoise_value) {
    return true;
  }
  for (HWND label : g_param_value_labels) {
    if (label == control) {
      return true;
    }
  }
  return false;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      g_window = hwnd;  // CreateWindowExW 返回前就需要它来创建子控件/调整尺寸
      {
        const std::size_t rows[3] = {0, 0, 0};
        const bool note[3] = {false, false, false};
        g_layout = ui::compute_layout(rows, note, g_advanced);
      }
      create_controls(hwnd);
      update_device_buttons();
      highlight_preset(g_preset_index);
      sync_treatment_controls();
      rebuild_plugin_panel();
      ::SetTimer(hwnd, kTimerSync, 50, nullptr);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_SIZE:
      release_back_buffer();
      if (g_power_button != nullptr && wparam != SIZE_MINIMIZED && !g_in_layout) {
        apply_layout(g_shown_rows, g_note_rows, false);
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = ::BeginPaint(hwnd, &paint);
      RECT client{};
      ::GetClientRect(hwnd, &client);
      const int width = client.right;
      const int height = client.bottom;
      if (width > 0 && height > 0) {
        if (g_back_dc == nullptr || g_back_size.cx != width || g_back_size.cy != height) {
          release_back_buffer();
          g_back_dc = ::CreateCompatibleDC(dc);
          g_back_bitmap = ::CreateCompatibleBitmap(dc, width, height);
          g_back_old = static_cast<HBITMAP>(::SelectObject(g_back_dc, g_back_bitmap));
          g_back_size = SIZE{width, height};
        }
        paint_client(hwnd, g_back_dc);
        ::BitBlt(dc, 0, 0, width, height, g_back_dc, 0, 0, SRCCOPY);
      }
      ::EndPaint(hwnd, &paint);
      return 0;
    }

    case WM_PRINTCLIENT:
      paint_client(hwnd, reinterpret_cast<HDC>(wparam));
      return 0;

    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      HWND control = reinterpret_cast<HWND>(lparam);
      ::SetBkMode(dc, TRANSPARENT);
      ::SetBkColor(dc, ui::window_color());
      COLORREF color = kText2Color;
      if (is_value_label(control)) {
        color = kAccentColor;
      }
      ::SetTextColor(dc, color);
      return reinterpret_cast<LRESULT>(ui::window_brush());
    }

    // 自绘按钮在收到 WM_DRAWITEM 前会先擦一次背景，默认按钮刷子是浅灰色，
    // 位置更新时就会闪出白边。统一换成窗口底色刷子消除这类闪烁。
    case WM_CTLCOLORBTN: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      ::SetBkColor(dc, ui::window_color());
      return reinterpret_cast<LRESULT>(ui::window_brush());
    }

    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
      ui::ButtonKind kind = ui::ButtonKind::Secondary;
      bool checked = false;
      const UINT id = item->CtlID;
      if (id >= static_cast<UINT>(kPresetButtonBase) &&
          id < static_cast<UINT>(kPresetButtonBase) + g_presets.size()) {
        kind = ui::ButtonKind::Preset;
        checked = static_cast<int>(id - kPresetButtonBase) == g_preset_index;
      } else if (id == static_cast<UINT>(kCompareButton)) {
        kind = ui::ButtonKind::Primary;
      } else if (id == static_cast<UINT>(kPowerButton)) {
        kind = ui::ButtonKind::Power;
        checked = !g_bypass;
      } else if (id == static_cast<UINT>(kAdvancedButton)) {
        // 高级入口是一条轻量文字按钮，不额外画边框。
        kind = ui::ButtonKind::Ghost;
        checked = g_advanced;
      } else if (id == static_cast<UINT>(kInputButton)) {
        kind = ui::ButtonKind::DeviceInput;
      } else if (id == static_cast<UINT>(kOutputButton)) {
        kind = ui::ButtonKind::DeviceOutput;
      } else if (id >= static_cast<UINT>(kPluginSlotBase) &&
                 id < static_cast<UINT>(kPluginSlotBase) + 3) {
        const int slot = static_cast<int>(id - kPluginSlotBase);
        kind = g_settings.plugin_slots[slot].vst3 ? ui::ButtonKind::SlotVst3
                                                  : ui::ButtonKind::SlotVst2;
        checked = g_plugin_failed[slot];
      } else if (id >= static_cast<UINT>(kEffectBase) &&
                 id < static_cast<UINT>(kEffectBase) + 4) {
        kind = ui::ButtonKind::Toggle;
        const bool effects[4] = {g_settings.clarity, g_settings.body, g_settings.leveler,
                                 g_settings.deesser};
        checked = effects[id - kEffectBase];
      } else if (id >= static_cast<UINT>(kOptionBase) &&
                 id < static_cast<UINT>(kOptionBase) + 3) {
        kind = ui::ButtonKind::Toggle;
        const bool options[3] = {g_settings.builtin_denoise, g_settings.start_with_windows,
                                 g_settings.start_minimized};
        checked = options[id - kOptionBase];
      } else if (id >= static_cast<UINT>(kParamTabBase) &&
                 id < static_cast<UINT>(kParamTabBase) + 3) {
        kind = ui::ButtonKind::Preset;
        checked = static_cast<int>(id - kParamTabBase) == g_active_param_group;
      }
      ui::draw_button(*item, kind, checked);
      return TRUE;
    }

    case WM_DESTROY:
      release_back_buffer();
      ::PostQuitMessage(0);
      return 0;

    // 单击托盘图标显示/隐藏主界面；降噪开关只在主界面里操作。
    case kTrayMessage:
      switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
          toggle_window_visible();
          break;
        case WM_RBUTTONUP:
          show_context_menu();
          break;
        default:
          break;
      }
      return 0;

    // 第二个实例被拦下后发来的唤醒消息：把本实例窗口显示出来即可。
    case kWakeMessage:
      show_window();
      return 0;

    case WM_COMMAND: {
      const UINT id = LOWORD(wparam);

      if (id == kInputButton) {
        show_device_menu(true);
        return 0;
      }
      if (id == kOutputButton) {
        show_device_menu(false);
        return 0;
      }
      if (id == kCompareButton) {
        start_compare();
        return 0;
      }
      if (id == kPowerButton) {
        toggle_bypass();
        return 0;
      }
      if (id == kAdvancedButton) {
        toggle_advanced();
        return 0;
      }
      if (id >= static_cast<UINT>(kPluginSlotBase) &&
          id < static_cast<UINT>(kPluginSlotBase) + 3) {
        show_plugin_menu(static_cast<int>(id - kPluginSlotBase));
        return 0;
      }
      if (id >= static_cast<UINT>(kParamTabBase) &&
          id < static_cast<UINT>(kParamTabBase) + 3) {
        select_param_group(static_cast<int>(id - kParamTabBase));
        return 0;
      }
      if (id >= static_cast<UINT>(kOptionBase) && id < static_cast<UINT>(kOptionBase) + 3) {
        const int index = static_cast<int>(id - kOptionBase);
        if (index == 0) {
          g_settings.builtin_denoise = !g_settings.builtin_denoise;
          g_settings.save();
          sync_treatment_controls();
          // 内置降噪开关需要重建处理链。
          restart_engine();
        } else if (index == 1) {
          g_settings.start_with_windows = !g_settings.start_with_windows;
          apply_autostart(g_settings.start_with_windows);
          g_settings.save();
          sync_treatment_controls();
        } else {
          g_settings.start_minimized = !g_settings.start_minimized;
          g_settings.save();
          sync_treatment_controls();
        }
        return 0;
      }
      if (id >= static_cast<UINT>(kEffectBase) && id < static_cast<UINT>(kEffectBase) + 4) {
        const int index = static_cast<int>(id - kEffectBase);
        bool& target = index == 0 ? g_settings.clarity
                       : index == 1 ? g_settings.body
                       : index == 2 ? g_settings.leveler
                                    : g_settings.deesser;
        target = !target;  // ownerdraw 需手动翻转
        ::SendMessageW(g_effect_checks[index], BM_SETCHECK,
                       target ? BST_CHECKED : BST_UNCHECKED, 0);
        ::InvalidateRect(g_effect_checks[index], nullptr, FALSE);
        g_engine.set_effects(g_settings.clarity, g_settings.body, g_settings.leveler,
                             g_settings.deesser);
        g_settings.save();
        return 0;
      }
      if (id >= static_cast<UINT>(kPresetButtonBase) &&
          id < static_cast<UINT>(kPresetButtonBase) + g_presets.size()) {
        select_preset(static_cast<int>(id - kPresetButtonBase));
        return 0;
      }
      if (id >= kMenuPresetBase && id < kMenuPresetBase + g_presets.size()) {
        select_preset(static_cast<int>(id - kMenuPresetBase));
        return 0;
      }
      if (id == kMenuBypass) {
        toggle_bypass();
        return 0;
      }
      if (id == kMenuShow) {
        show_window();
        return 0;
      }
      if (id == kMenuExit) {
        cleanup_and_quit();
        return 0;
      }
      break;
    }

    case WM_HSCROLL: {
      HWND slider = reinterpret_cast<HWND>(lparam);
      const bool ended = LOWORD(wparam) == TB_ENDTRACK;

      if (slider == g_gain_slider || slider == g_out_gain_slider) {
        const LRESULT position = ::SendMessageW(slider, TBM_GETPOS, 0, 0);
        const float db = (static_cast<float>(position) - 600.0f) / 10.0f;
        const bool input = slider == g_gain_slider;
        if (input) {
          g_settings.gain_db = db;
          g_engine.set_input_gain_db(db);
        } else {
          g_settings.output_gain_db = db;
          g_engine.set_output_gain_db(db);
        }

        wchar_t text[32]{};
        swprintf_s(text, L"%+.1f dB", static_cast<double>(db));
        set_control_text(input ? g_gain_value : g_out_gain_value, text);
        if (ended) {
          g_settings.save();
        }
        return 0;
      }

      if (slider == g_denoise_slider) {
        const LRESULT position = ::SendMessageW(slider, TBM_GETPOS, 0, 0);
        const float amount = static_cast<float>(position) / 100.0f;
        g_settings.denoise_amount = amount;
        g_engine.set_denoise_amount(amount);

        wchar_t text[32]{};
        swprintf_s(text, L"%.0f%%", static_cast<double>(amount * 100.0f));
        set_control_text(g_denoise_value, text);
        if (ended) {
          g_settings.save();
        }
        return 0;
      }

      for (std::size_t i = 0; i < g_param_sliders.size(); ++i) {
        if (g_param_sliders[i] != slider) {
          continue;
        }
        const std::size_t index = g_param_slider_index[i];
        if (index >= g_plugin_params.size()) {
          return 0;
        }
        const LRESULT position = ::SendMessageW(slider, TBM_GETPOS, 0, 0);
        const float value = static_cast<float>(position) / 1000.0f;
        g_plugin_params[index].value = value;
        g_engine.set_plugin_parameter(g_plugin_params[index].slot,
                                      g_plugin_params[index].id, value);

        wchar_t text[16]{};
        swprintf_s(text, L"%.0f%%", static_cast<double>(value * 100.0f));
        set_control_text(g_param_value_labels[i], text);

        if (ended) {
          save_plugin_params();
        }
        return 0;
      }
      break;
    }

    case WM_TIMER:
      if (wparam == kTimerSync) {
        sync_plugin_parameter_from_editor();
        return 0;
      }
      if (wparam == kTimerStatus) {
        update_status();
        if (g_plugin_params_dirty) {
          save_plugin_params();
          g_plugin_params_dirty = false;
        }
        return 0;
      }
      if (wparam == kTimerCompare) {
        ::KillTimer(hwnd, kTimerCompare);
        g_engine.set_bypass(false);
        g_bypass = false;
        // 对比结束必须清掉页脚提示，否则会一直停在「对比中」。
        g_footer_line.clear();
        restore_compare_button();
        update_status();
        return 0;
      }
      if (wparam == kTimerExit) {
        ::KillTimer(hwnd, kTimerExit);
        ::SendMessageW(hwnd, WM_COMMAND, kMenuExit, 0);
        return 0;
      }
      break;

    case WM_CLOSE:
      ::ShowWindow(hwnd, SW_HIDE);  // 关闭窗口只是收起，程序继续驻留托盘
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  std::vector<std::wstring> args;
  {
    int count = 0;
    LPWSTR* raw = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    for (int i = 0; raw != nullptr && i < count; ++i) {
      args.emplace_back(raw[i]);
    }
    if (raw != nullptr) {
      ::LocalFree(raw);
    }
  }

  double exit_after = 0.0;
  bool show_on_start = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--exit-after" && i + 1 < args.size()) {
      exit_after = _wtof(args[++i].c_str());
    } else if (args[i] == L"--show") {
      show_on_start = true;
    }
  }

  // 单实例：已有一个进程在跑时，不再启动第二份。
  // 用命名互斥体判定，并把已有实例的主窗口唤到前台，避免出现多个托盘图标和
  // 多个降噪引擎同时抢同一对音频端点。
  ::CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
  const DWORD mutex_error = ::GetLastError();
  if (mutex_error == ERROR_ALREADY_EXISTS) {
    // 已有实例可能还在启动（窗口尚未创建），短暂等待后再唤醒。
    HWND existing = nullptr;
    for (int attempt = 0; attempt < 40 && existing == nullptr; ++attempt) {
      existing = ::FindWindowW(L"MicStudioWindow", nullptr);
      if (existing == nullptr) {
        ::Sleep(50);
      }
    }
    if (existing != nullptr) {
      // 已存在实例大概率是隐藏在托盘的，先还原再置前。
      if (::IsIconic(existing)) {
        ::ShowWindow(existing, SW_RESTORE);
      } else {
        ::ShowWindow(existing, SW_SHOWNORMAL);
      }
      // 本进程是被拦下的新实例，允许它把前台权限让给已有窗口。
      ::AllowSetForegroundWindow(ASFW_ANY);
      ::SetForegroundWindow(existing);
      // 主动通知已有实例刷新界面，避免只恢复到旧状态。
      ::PostMessageW(existing, kWakeMessage, 0, 0);
    }
    return 0;
  }

  ms::platform::ComApartment com;
  if (!com.usable()) {
    log_line("COM 初始化失败");
    return 1;
  }
  ui::prepare_process();  // 必须先于任何通用控件创建，深色主题才会生效

  INITCOMMONCONTROLSEX controls{};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_BAR_CLASSES | ICC_TAB_CLASSES;
  ::InitCommonControlsEx(&controls);

  g_settings = ms::config::Settings::load();
  // 注册表与设置保持一致（例如用户手动删了启动项后，这里会补回）。
  apply_autostart(g_settings.start_with_windows);
  g_presets = ms::presets::PresetLibrary::all();
  for (std::size_t i = 0; i < g_presets.size(); ++i) {
    if (g_presets[i].id == g_settings.preset_id) {
      g_preset_index = static_cast<int>(i);
    }
  }

  const auto inputs = ms::audio_io::enumerate_devices(ms::audio_io::Flow::Capture);
  const auto outputs = ms::audio_io::enumerate_devices(ms::audio_io::Flow::Render);
  if (!inputs.error.ok() || !outputs.error.ok()) {
    log_line("枚举设备失败");
    return 2;
  }
  g_inputs = inputs.items;
  g_outputs = outputs.items;

  const int input_index = resolve_endpoint(g_inputs, g_settings.input_id, g_settings.input_name);
  const int output_index = resolve_endpoint(g_outputs, g_settings.output_id, g_settings.output_name);
  if (input_index < 0 || output_index < 0) {
    log_line("设备配置缺失或已失效，请先运行 engine-cli run ... --save 写入配置");
    return 2;
  }
  g_input_endpoint = endpoint_position(g_inputs, input_index);
  g_output_endpoint = endpoint_position(g_outputs, output_index);
  if (g_input_endpoint < 0 || g_output_endpoint < 0) {
    log_line("设备配置缺失或已失效，请先运行 engine-cli run ... --save 写入配置");
    return 2;
  }

  const auto& input = g_inputs[static_cast<std::size_t>(g_input_endpoint)];
  const auto& sink = g_outputs[static_cast<std::size_t>(g_output_endpoint)];

  // 虚拟麦克风：优先用设置里记录的 id
  int mic_index = resolve_endpoint(g_inputs, g_settings.mic_id, g_settings.mic_name);
  if (mic_index < 0) {
    for (const auto& device : g_inputs) {
      if (device.friendlyName.find(L"CABLE Output") != std::wstring::npos) {
        mic_index = static_cast<int>(device.index);
        break;
      }
    }
  }
  if (mic_index < 0) {
    log_line("找不到虚拟麦克风端点");
    return 2;
  }
  const auto& mic = g_inputs[static_cast<std::size_t>(mic_index)];

  // 异常退出自检：默认设备若还停在虚拟麦克风上，先切回真实麦克风
  if (ms::platform::current_default_capture() == mic.id) {
    ms::platform::restore_default_capture(input.id);
    log_line("检测到上次异常退出留下的默认设备，已切回真实麦克风");
  }

  ms::engine::EngineOptions options;
  options.input_endpoint_id = input.id;
  options.output_endpoint_id = sink.id;
  options.input_gain_db = g_settings.gain_db;
  options.output_gain_db = g_settings.output_gain_db;
  options.denoise_amount = g_settings.denoise_amount;
  options.clarity = g_settings.clarity;
  options.body = g_settings.body;
  options.leveler = g_settings.leveler;
  options.deesser = g_settings.deesser;

  auto error = start_engine_with_plugin(options);
  if (!error.ok()) {
    log_line("启动失败：" + ms::platform::to_utf8(error.message));
    return 3;
  }
  const auto plugin_params = g_engine.plugin_parameters();
  for (int slot = 0; slot < 3; ++slot) {
    if (g_settings.plugin_slots[slot].path.empty() || !g_plugin_error[slot].empty()) {
      continue;
    }
    std::size_t count = 0;
    for (const auto& parameter : plugin_params) {
      if (parameter.slot == slot) {
        ++count;
      }
    }
    log_line("插件槽 " + std::to_string(slot + 1) + " 已加载：" +
             ms::platform::to_utf8(g_settings.plugin_slots[slot].path) + "（" +
             std::to_string(count) + " 个参数）");
  }
  if (!g_settings.builtin_denoise) {
    log_line("内置降噪：已关闭");
  } else if (g_engine.stats().deepfilter_active) {
    log_line("内置降噪：DeepFilterNet");
  } else {
    log_line("内置降噪：DeepFilterNet 加载失败（" +
             ms::platform::to_utf8(g_engine.builtin_denoise_error()) + "）");
  }

  if (g_settings.set_default_mic) {
    g_previous_default = ms::platform::set_default_capture(mic.id);
    g_default_switched = !g_previous_default.empty();
  }

  if (!ui::initialize()) {
    log_line("UI 初始化失败（GDI+ 不可用）");
    g_engine.stop();
    return 5;
  }
  ui::register_control_classes(::GetModuleHandleW(nullptr));

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = window_proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"MicStudioWindow";
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.hIcon = load_app_icon(::GetSystemMetrics(SM_CXICON));
  wc.hIconSm = load_app_icon(::GetSystemMetrics(SM_CXSMICON));
  ::RegisterClassExW(&wc);

  // 固定窄窗：不可拉伸，展开高级区只向下增高。
  g_window = ::CreateWindowExW(
      0, wc.lpszClassName, L"Mic Studio",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT,
      CW_USEDEFAULT, kWindowWidth, 520, nullptr, nullptr, wc.hInstance, nullptr);
  if (g_window == nullptr) {
    ui::shutdown();
    g_engine.stop();
    return 4;
  }
  ui::apply_dwm(g_window);

  g_icon.cbSize = sizeof(g_icon);
  g_icon.hWnd = g_window;
  g_icon.uID = kIconId;
  g_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_icon.uCallbackMessage = kTrayMessage;
  g_icon.hIcon = load_app_icon(::GetSystemMetrics(SM_CXSMICON));
  wcscpy_s(g_icon.szTip, L"Mic Studio");
  g_tray_added = ::Shell_NotifyIconW(NIM_ADD, &g_icon) != FALSE;

  update_status();
  ::SetTimer(g_window, kTimerStatus, 1000, nullptr);
  // 默认显示主界面；“启动时最小化到托盘”打开后只驻留托盘。
  if (show_on_start || !g_settings.start_minimized) {
    ::ShowWindow(g_window, SW_SHOWNORMAL);
    ::SetForegroundWindow(g_window);
  }
  if (exit_after > 0.0) {
    ::SetTimer(g_window, kTimerExit, static_cast<UINT>(exit_after * 1000.0), nullptr);
  }

  log_line("主窗口已启动：" + ms::platform::to_utf8(input.friendlyName) + " → " +
           ms::platform::to_utf8(sink.friendlyName));

  MSG msg{};
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  g_engine.stop();
  ui::shutdown();
  log_line("已退出");
  return 0;
}
