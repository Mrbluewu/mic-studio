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

std::wstring device_name_of(const std::vector<ms::audio_io::EndpointInfo>& items, int position) {
  if (position < 0 || static_cast<std::size_t>(position) >= items.size()) {
    return L"未选择";
  }
  return items[static_cast<std::size_t>(position)].friendlyName;
}

constexpr COLORREF kText2Color = RGB(164, 182, 194);
constexpr COLORREF kAccentColor = RGB(52, 224, 192);
constexpr COLORREF kDangerColor = RGB(240, 113, 103);

bool is_value_label(HWND control) {
  return control == g_gain_value || control == g_out_gain_value || control == g_denoise_value;
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

}  // namespace

namespace app {

// 经典「声音」控制面板（播放/录制/声音/通信四个标签），不是 Win10/11 的设置页。
void open_system_sound_panel() {
  const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", L"rundll32.exe",
                                           L"shell32.dll,Control_RunDLL mmsys.cpl", nullptr,
                                           SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    log_line("打开系统声音面板失败");
  }
}

void update_device_buttons() {
  set_control_text(g_input_button, device_name_of(g_inputs, g_input_endpoint));
  set_control_text(g_output_button, device_name_of(g_outputs, g_output_endpoint));
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

HICON load_app_icon(int size) {
  HICON icon = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr),
                                               MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size,
                                               LR_DEFAULTCOLOR));
  return icon != nullptr ? icon : ::LoadIconW(nullptr, IDI_APPLICATION);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      g_window = hwnd;  // CreateWindowExW 返回前就需要它来创建子控件/调整尺寸
      g_layout = ui::compute_layout(g_advanced);
      create_controls(hwnd);
      update_device_buttons();
      highlight_preset(g_preset_index);
      sync_treatment_controls();
      update_plugin_slot_buttons();
      ::SetTimer(hwnd, kTimerSync, 50, nullptr);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_SIZE:
      release_back_buffer();
      if (g_power_button != nullptr && wparam != SIZE_MINIMIZED && !g_in_layout) {
        apply_layout(false);
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
      if (id == kSystemSoundButton) {
        open_system_sound_panel();
        return 0;
      }
      if (id >= static_cast<UINT>(kPluginSlotBase) &&
          id < static_cast<UINT>(kPluginSlotBase) + 3) {
        const int slot = static_cast<int>(id - kPluginSlotBase);
        // 已装插件单击直接进插件自己的界面；空槽仍走菜单来选插件。
        if (g_settings.plugin_slots[slot].path.empty()) {
          show_plugin_menu(slot);
        } else {
          open_plugin_editor_window(slot);
        }
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

    // 已装插件的槽单击直接进编辑器，右键仍用来替换/移除插件。
    case WM_CONTEXTMENU: {
      const HWND child = reinterpret_cast<HWND>(wparam);
      for (int slot = 0; slot < 3; ++slot) {
        if (child == g_plugin_slot_button[slot]) {
          show_plugin_menu(slot);
          return 0;
        }
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

}  // namespace app
