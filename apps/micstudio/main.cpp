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

// 首次启动没有设备记录时按系统默认端点补齐：麦克风优先通信默认，输出优先控制台默认；
// 两种角色都没标出来时退到第一台可用设备。excluded_id 用于把虚拟麦克风排除在输入之外。
int pick_default_endpoint(const std::vector<ms::audio_io::EndpointInfo>& items, bool communications,
                          const std::wstring& excluded_id) {
  auto find = [&](bool comm) {
    for (const auto& item : items) {
      if (!excluded_id.empty() && item.id == excluded_id) {
        continue;
      }
      if (comm ? item.isDefaultCommunications : item.isDefaultConsole) {
        return static_cast<int>(item.index);
      }
    }
    return -1;
  };

  int index = communications ? find(true) : find(false);
  if (index < 0) {
    index = communications ? find(false) : find(true);
  }
  if (index < 0) {
    for (const auto& item : items) {
      if (excluded_id.empty() || item.id != excluded_id) {
        return static_cast<int>(item.index);
      }
    }
  }
  return index >= 0 ? index : static_cast<int>(items.front().index);
}

// 启动阶段的用户可见提示：同时写日志，避免只留一行没人看的记录。
void show_startup_notice(const std::wstring& text) {
  log_line(ms::platform::to_utf8(text));
  ::MessageBoxW(nullptr, text.c_str(), L"Mic Studio", MB_OK | MB_ICONWARNING);
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

  // 虚拟麦克风（CABLE Output）：首次启动时用作系统默认录音设备，
  // 选输入时必须排除它，否则会把虚拟麦克风当真实输入形成回环。
  std::wstring virtual_mic_id;
  std::wstring virtual_sink_id;
  for (const auto& device : g_inputs) {
    if (device.friendlyName.find(L"CABLE Output") != std::wstring::npos) {
      virtual_mic_id = device.id;
      break;
    }
  }
  for (const auto& device : g_outputs) {
    if (device.friendlyName.find(L"CABLE Input") != std::wstring::npos) {
      virtual_sink_id = device.id;
      break;
    }
  }

  // 设备配置整体为空 = 真正的首次启动（装完直接打开，没有跑过 engine-cli --save）。
  const bool fresh_device_config = g_settings.input_id.empty() &&
                                   g_settings.input_name.empty() &&
                                   g_settings.output_id.empty() &&
                                   g_settings.output_name.empty();
  int input_index = resolve_endpoint(g_inputs, g_settings.input_id, g_settings.input_name);
  int output_index = resolve_endpoint(g_outputs, g_settings.output_id, g_settings.output_name);

  if (fresh_device_config) {
    if (g_inputs.empty()) {
      show_startup_notice(L"未检测到可用的麦克风。请连接麦克风后重新启动。");
      return 2;
    }
    if (g_outputs.empty()) {
      show_startup_notice(L"未检测到可用的输出设备。请连接播放设备后重新启动。");
      return 2;
    }
    input_index = pick_default_endpoint(g_inputs, true, virtual_mic_id);
    // 输出优先写虚拟声卡：只有写进 CABLE Input，其他会议软件才能在 CABLE Output 上读到
    // 降噪后的声音；没装虚拟声卡时再退回系统默认输出，避免首启直接把麦克风播到扬声器。
    output_index = resolve_endpoint(g_outputs, virtual_sink_id, L"");
    const bool used_virtual_sink = output_index >= 0;
    if (!used_virtual_sink) {
      output_index = pick_default_endpoint(g_outputs, false, L"");
    }
    const int input_pos = endpoint_position(g_inputs, input_index);
    const int output_pos = endpoint_position(g_outputs, output_index);
    const auto& picked_input = g_inputs[static_cast<std::size_t>(input_pos)];
    const auto& picked_output = g_outputs[static_cast<std::size_t>(output_pos)];
    if (!virtual_mic_id.empty() && picked_input.id == virtual_mic_id) {
      show_startup_notice(L"未检测到可用的真实麦克风（只剩虚拟麦克风）。请连接麦克风后重新启动。");
      return 2;
    }
    g_settings.input_id = picked_input.id;
    g_settings.input_name = picked_input.friendlyName;
    g_settings.output_id = picked_output.id;
    g_settings.output_name = picked_output.friendlyName;
    log_line("首次启动：麦克风 \"" + ms::platform::to_utf8(picked_input.friendlyName) +
             "\" → 输出 \"" + ms::platform::to_utf8(picked_output.friendlyName) + "\"" +
             (used_virtual_sink ? "（虚拟声卡）" : ""));
    // 没有系统默认设备时明确告知用户实际选用了哪一台，而不是悄悄换设备。
    // 若默认端点就是虚拟麦克风（上次异常退出留下的），说明默认设置本身存在，
    // 只是被上面的排除规则跳过，这里只记日志，不弹窗打扰。
    bool has_default_input = false;
    for (const auto& device : g_inputs) {
      if (device.isDefaultCommunications || device.isDefaultConsole) {
        has_default_input = true;
        break;
      }
    }
    if (!has_default_input) {
      show_startup_notice(L"未找到系统默认麦克风，已自动选用「" + picked_input.friendlyName + L"」。");
    } else if (!picked_input.isDefaultCommunications && !picked_input.isDefaultConsole) {
      log_line("系统默认录音设备是虚拟麦克风，已改用真实麦克风 \"" +
               ms::platform::to_utf8(picked_input.friendlyName) + "\"");
    }
    if (!used_virtual_sink && !picked_output.isDefaultConsole) {
      show_startup_notice(L"未找到系统默认输出设备，已自动选用「" +
                          picked_output.friendlyName + L"」。");
    }
  } else if (input_index < 0 || output_index < 0) {
    show_startup_notice(L"设置里记录的音频设备已不可用。请重新选择麦克风和输出设备后重试。");
    return 2;
  }

  g_input_endpoint = endpoint_position(g_inputs, input_index);
  g_output_endpoint = endpoint_position(g_outputs, output_index);
  if (g_input_endpoint < 0 || g_output_endpoint < 0) {
    show_startup_notice(L"音频设备定位失败，请重新选择设备后重试。");
    return 2;
  }

  const auto& input = g_inputs[static_cast<std::size_t>(g_input_endpoint)];
  const auto& sink = g_outputs[static_cast<std::size_t>(g_output_endpoint)];

  // 虚拟麦克风：优先用设置里记录的 id，没有再按名字找 CABLE Output。
  int mic_index = resolve_endpoint(g_inputs, g_settings.mic_id, g_settings.mic_name);
  if (mic_index < 0 && !virtual_mic_id.empty()) {
    mic_index = resolve_endpoint(g_inputs, virtual_mic_id, L"");
  }
  // mic_index 是端点自身的 index，不等于它在 g_inputs 中的位置：枚举跳过坏端点时两者会错位。
  const int mic_position = endpoint_position(g_inputs, mic_index);
  const ms::audio_io::EndpointInfo* mic =
      mic_position >= 0 ? &g_inputs[static_cast<std::size_t>(mic_position)] : nullptr;
  if (mic == nullptr) {
    if (!fresh_device_config) {
      show_startup_notice(L"未找到虚拟麦克风（CABLE Output）。请安装 VB-Audio Virtual Cable 后重试。");
      return 2;
    }
    // 首次启动没有虚拟麦克风也继续：降噪链路可用，只是不自动切换系统默认录音设备。
    log_line("首次启动：未找到虚拟麦克风（CABLE Output），本次不切换系统默认录音设备");
  } else if (fresh_device_config) {
    g_settings.mic_id = mic->id;
    g_settings.mic_name = mic->friendlyName;
  }
  if (fresh_device_config) {
    g_settings.save();
  }

  // 异常退出自检：默认设备若还停在虚拟麦克风上，先切回真实麦克风
  if (mic != nullptr && ms::platform::current_default_capture() == mic->id) {
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

  if (g_settings.set_default_mic && mic != nullptr) {
    g_previous_default = ms::platform::set_default_capture(mic->id);
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
  // CreateWindowExW 的宽高是含标题栏/边框的外框尺寸，而布局按客户区 300px 宽绘制。
  // 首次启动若不做这一步，客户区只有 284px 宽，卡片右沿会被窗口边缘裁掉；
  // 高度也会停在硬编码的 520 外框，折叠态 448 不生效，页脚与底边留白错位。
  // 这里走与展开/收起相同的布局路径，把客户区定成 300 × 折叠态高度。
  apply_layout(true);
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
