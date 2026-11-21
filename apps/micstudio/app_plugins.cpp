#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "audio_io/device_enumerator.h"
#include "config/settings.h"
#include "engine/audio_engine.h"
#include "platform/com.h"
#include "platform/default_endpoint.h"
#include "platform/utf8.h"
#include "presets/preset_library.h"
#include "resource.h"
#include "ui_theme.h"

#include "app.h"

using namespace app;

namespace {

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
  if (restart_engine()) {
    g_settings.save();
  } else {
    update_plugin_slot_buttons();
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
    update_plugin_slot_buttons();
  }
  log_line("已移除插件槽 " + std::to_string(slot + 1));
}

}  // namespace

namespace app {

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

void save_plugin_params() {
  for (int slot = 0; slot < 3; ++slot) {
    g_settings.plugin_slots[slot].params = encode_plugin_params(slot);
  }
  g_settings.save();
}

// 插件槽单击后进这里：同一时间只保留一个编辑器窗口，重复点击只把已有窗口提到前台。
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

// 插件参数只在插件自己的编辑器里调整，主界面只负责显示槽里装的是谁。
void update_plugin_slot_buttons() {
  if (g_plugin_slot_button[0] == nullptr) {
    return;
  }
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
}

void close_plugin_editor_window() {
  if (g_editor_window == nullptr) {
    return;
  }
  // 关闭前先收一次参数改动，避免最后一次调整来不及进设置。
  sync_plugin_parameter_from_editor();
  if (g_plugin_params_dirty) {
    save_plugin_params();
    g_plugin_params_dirty = false;
  }
  // 先让插件从宿主窗口摘下自己的子窗口，再销毁宿主，避免部分 VST3 崩溃。
  g_engine.close_plugin_editor(g_editor_slot);
  g_editor_slot = -1;
  ::DestroyWindow(g_editor_window);
  g_editor_window = nullptr;
}

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
    update_plugin_slot_buttons();
    return false;
  }

  g_settings.input_id = input.id;
  g_settings.input_name = input.friendlyName;
  g_settings.output_id = sink.id;
  g_settings.output_name = sink.friendlyName;
  g_settings.save();
  update_plugin_slot_buttons();
  update_status();
  return true;
}

// 参数不在主界面外显，这里只把插件编辑器里的改动收进快照，交给定时器落盘。
void sync_plugin_parameter_from_editor() {
  if (g_engine.take_plugin_parameter_changes().empty()) {
    return;
  }
  g_plugin_params = g_engine.plugin_parameters();
  g_plugin_params_dirty = true;
}

void show_plugin_menu(int slot) {
  if (slot < 0 || slot >= 3) {
    return;
  }
  const auto& config = g_settings.plugin_slots[slot];
  const bool loaded = !config.path.empty();
  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING, 1, loaded ? L"更换为 VST2 插件…" : L"添加 VST2 插件…");
#ifdef MS_ENABLE_VST3
  ::AppendMenuW(menu, MF_STRING, 2, loaded ? L"更换为 VST3 插件…" : L"添加 VST3 插件…");
#else
  // 关闭 VST3 的构建里不提供入口，避免用户选了文件之后才收到"不支持"。
  ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 2, L"VST3 支持未编译进本次构建");
#endif
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

}  // namespace app
