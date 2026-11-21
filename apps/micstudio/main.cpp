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

// 启用 Common Controls v6，深色下拉框与自绘按钮依赖它。
#pragma comment(linker, \
                "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                "language='*'\"")

using namespace app;

namespace {

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
  // mic_index 是端点自身的 index，不等于它在 g_inputs 中的位置：枚举跳过坏端点时两者会错位。
  const int mic_position = endpoint_position(g_inputs, mic_index);
  if (mic_position < 0) {
    log_line("找不到虚拟麦克风端点");
    return 2;
  }
  const auto& mic = g_inputs[static_cast<std::size_t>(mic_position)];

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
