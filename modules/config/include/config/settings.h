#pragma once

#include <string>

namespace ms::config {

// 一个降噪插件槽：槽内只能是 VST2 或 VST3 之一。
// params 编码为 "id=value;id=value"，值为归一化 0..1。
struct PluginSlotSettings {
  bool vst3 = false;
  std::wstring path;
  std::wstring params;
};

// 用户设置：记住上次用的设备、场景与效果，让"一键启动"真的只需要一次点击。
// 存储：%APPDATA%\MicStudio\settings.ini（Windows 原生 INI，UTF-16）。
// 键名与方案中的 JSON 字段一一对应，后续换 JSON 不需要改调用方。
struct Settings {
  std::wstring input_id;
  std::wstring input_name;
  std::wstring output_id;
  std::wstring output_name;
  std::wstring mic_id;
  std::wstring mic_name;

  std::string preset_id = "meeting";
  float gain_db = 0.0f;
  float output_gain_db = 0.0f;
  float denoise_amount = 0.7f;

  bool clarity = false;
  bool body = false;
  bool leveler = false;
  bool deesser = false;

  // 最多 3 个插件槽，按顺序串接在内置降噪之后。
  PluginSlotSettings plugin_slots[3];

  // 内置降噪（DeepFilterNet）开关；关闭后只用插件降噪。
  bool builtin_denoise = true;

  bool start_with_windows = false;
  bool start_minimized = false;
  bool set_default_mic = true;

  static std::wstring directory();
  static std::wstring file_path();

  // 文件不存在或读取失败时返回默认值，不抛异常。
  static Settings load();
  bool save() const;
};

}  // namespace ms::config
