#pragma once

#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "audio_io/device_enumerator.h"
#include "config/settings.h"
#include "engine/audio_engine.h"
#include "platform/error.h"
#include "presets/preset_library.h"
#include "ui_theme.h"

// 主程序的共享状态与跨编译单元声明。
// 实现按职责分在 app_*.cpp 中；仅单个 .cpp 使用的辅助函数留在该文件的匿名 namespace 里。
namespace app {

// ---- 消息与定时器 ----
inline constexpr UINT kTrayMessage = WM_APP + 1;
// 第二个实例启动时发给已有实例，让它把自己显示出来。
inline constexpr UINT kWakeMessage = WM_APP + 2;
inline constexpr UINT kMenuPresetBase = 1000;
inline constexpr UINT kMenuBypass = 1100;
inline constexpr UINT kMenuShow = 1101;
inline constexpr UINT kMenuExit = 1102;
inline constexpr UINT_PTR kTimerStatus = 1;
inline constexpr UINT_PTR kTimerExit = 2;
inline constexpr UINT_PTR kTimerCompare = 3;
inline constexpr UINT_PTR kTimerEditor = 4;
inline constexpr UINT_PTR kTimerSync = 5;
inline constexpr UINT kIconId = 1;
// 单实例互斥体名（Local 作用域：同一用户会话内唯一即可）。
inline constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MicStudio.SingleInstance";

// ---- 控件 ID ----
inline constexpr int kInputButton = 100;
inline constexpr int kOutputButton = 101;
inline constexpr int kGainSlider = 110;
inline constexpr int kDenoiseSlider = 111;
inline constexpr int kOutputGainSlider = 113;
inline constexpr int kCompareButton = 200;
inline constexpr int kPowerButton = 201;
inline constexpr int kAdvancedButton = 202;
inline constexpr int kSystemSoundButton = 203;
inline constexpr int kPluginSlotBase = 210;
inline constexpr int kEffectBase = 220;
inline constexpr int kOptionBase = 230;
inline constexpr int kPresetButtonBase = 300;

// 客户区固定 300px：与设计稿一致，窗口不可拉伸。
inline constexpr int kWindowWidth = 300;

// ---- 窗口与控件 ----
inline HWND g_window = nullptr;
inline HWND g_input_button = nullptr;
inline HWND g_output_button = nullptr;
inline HWND g_compare_button = nullptr;
inline HWND g_power_button = nullptr;
inline HWND g_advanced_button = nullptr;
inline HWND g_system_sound_button = nullptr;
inline HWND g_gain_slider = nullptr;
inline HWND g_gain_value = nullptr;
inline HWND g_out_gain_slider = nullptr;
inline HWND g_out_gain_value = nullptr;
inline HWND g_denoise_slider = nullptr;
inline HWND g_denoise_value = nullptr;
inline HWND g_effect_checks[4]{};
inline HWND g_option_checks[3]{};
inline HWND g_plugin_slot_button[3]{};
inline HWND g_meter = nullptr;
inline int g_input_endpoint = -1;
inline int g_output_endpoint = -1;
inline HWND g_editor_window = nullptr;
inline int g_editor_width = 0;
inline int g_editor_height = 0;
inline int g_editor_slot = -1;
inline std::vector<HWND> g_preset_buttons;

inline ui::Layout g_layout{};
inline std::wstring g_footer_line;
inline double g_latency_ms = 0.0;
inline bool g_in_layout = false;
inline HDC g_back_dc = nullptr;
inline HBITMAP g_back_bitmap = nullptr;
inline HBITMAP g_back_old = nullptr;
inline SIZE g_back_size{0, 0};

// 插件参数只在插件自己的编辑器里调整；这里只保留一份快照用于持久化。
inline std::vector<ms::engine::PluginParameterInfo> g_plugin_params;
inline std::wstring g_plugin_error[3];
inline bool g_plugin_failed[3] = {false, false, false};
inline bool g_plugin_params_dirty = false;

inline NOTIFYICONDATAW g_icon{};
inline ms::engine::AudioEngine g_engine;
inline ms::config::Settings g_settings;
inline std::vector<ms::presets::PresetTemplate> g_presets;
inline std::vector<ms::audio_io::EndpointInfo> g_inputs;
inline std::vector<ms::audio_io::EndpointInfo> g_outputs;
inline int g_preset_index = 0;
inline bool g_bypass = false;
// 高级区默认收起：主界面只留下开关、设备、降噪强度与底噪对比。
inline bool g_advanced = false;
inline bool g_tray_added = false;
inline bool g_default_switched = false;
inline std::wstring g_previous_default;

// ---- 控件、布局与绘制（app_ui.cpp）----
void set_control_text(HWND control, const std::wstring& text);
void update_status();
void highlight_preset(int index);
void sync_treatment_controls();
void select_preset(int index);
void toggle_advanced();
void apply_layout(bool resize_window);
void create_controls(HWND hwnd);
void release_back_buffer();
void paint_client(HWND hwnd, HDC dc);

// ---- 插件与引擎重启（app_plugins.cpp）----
ms::platform::Error start_engine_with_plugin(const ms::engine::EngineOptions& base_options);
void save_plugin_params();
void update_plugin_slot_buttons();
void open_plugin_editor_window(int slot);
void close_plugin_editor_window();
bool restart_engine();
void sync_plugin_parameter_from_editor();
void show_plugin_menu(int slot);

// ---- 窗口过程与交互（app_window.cpp）----
void update_device_buttons();
void open_system_sound_panel();
bool apply_autostart(bool enabled);
void log_line(const std::string& text);
HICON load_app_icon(int size);
LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

}  // namespace app
