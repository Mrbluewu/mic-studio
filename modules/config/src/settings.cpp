#include "config/settings.h"

#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace ms::config {
namespace {

constexpr wchar_t kSection[] = L"mic-studio";

std::wstring read_string(const wchar_t* key, const std::wstring& fallback,
                         DWORD capacity = 1024) {
  std::vector<wchar_t> buffer(capacity, L'\0');
  const DWORD chars = ::GetPrivateProfileStringW(kSection, key, fallback.c_str(), buffer.data(),
                                                 capacity, Settings::file_path().c_str());
  return std::wstring(buffer.data(), chars);
}

bool ends_with_vst3(const std::wstring& path) {
  if (path.size() < 5) {
    return false;
  }
  return ::_wcsicmp(path.c_str() + path.size() - 5, L".vst3") == 0;
}

// DeepFilterNet 现在是内置降噪后端，不能再作为普通插件重复加载。
bool is_builtin_denoise_module(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
  return ::_wcsicmp(name.c_str(), L"DeepFilterNet.dll") == 0 ||
         ::_wcsicmp(name.c_str(), L"DeepFilterNet.vst3") == 0;
}

int read_int(const wchar_t* key, int fallback) {
  return static_cast<int>(::GetPrivateProfileIntW(kSection, key, fallback, Settings::file_path().c_str()));
}

void write_string(const wchar_t* key, const std::wstring& value) {
  ::WritePrivateProfileStringW(kSection, key, value.c_str(), Settings::file_path().c_str());
}

void write_int(const wchar_t* key, int value) {
  ::WritePrivateProfileStringW(kSection, key, std::to_wstring(value).c_str(),
                               Settings::file_path().c_str());
}

std::string to_narrow(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (wchar_t ch : text) {
    out.push_back(static_cast<char>(ch));
  }
  return out;
}

std::wstring to_wide(const std::string& text) {
  std::wstring out;
  out.reserve(text.size());
  for (char ch : text) {
    out.push_back(static_cast<wchar_t>(ch));
  }
  return out;
}

}  // namespace

std::wstring Settings::directory() {
  wchar_t* appdata = nullptr;
  std::size_t length = 0;
  if (_wdupenv_s(&appdata, &length, L"APPDATA") != 0 || appdata == nullptr) {
    return L".";
  }
  std::wstring path(appdata);
  std::free(appdata);
  path += L"\\MicStudio";
  return path;
}

std::wstring Settings::file_path() { return directory() + L"\\settings.ini"; }

Settings Settings::load() {
  Settings settings;
  if (::GetFileAttributesW(file_path().c_str()) == INVALID_FILE_ATTRIBUTES) {
    return settings;
  }

  settings.input_id = read_string(L"input_id", L"");
  settings.input_name = read_string(L"input_name", L"");
  settings.output_id = read_string(L"output_id", L"");
  settings.output_name = read_string(L"output_name", L"");
  settings.mic_id = read_string(L"mic_id", L"");
  settings.mic_name = read_string(L"mic_name", L"");
  settings.preset_id = to_narrow(read_string(L"preset", L"meeting"));

  settings.gain_db = static_cast<float>(read_int(L"gain_db_x10", 0)) / 10.0f;
  settings.output_gain_db = static_cast<float>(read_int(L"output_gain_db_x10", 0)) / 10.0f;
  settings.denoise_amount = static_cast<float>(read_int(L"denoise_percent", 70)) / 100.0f;

  settings.clarity = read_int(L"clarity", 0) != 0;
  settings.body = read_int(L"body", 0) != 0;
  settings.leveler = read_int(L"leveler", 0) != 0;
  settings.deesser = read_int(L"deesser", 0) != 0;

  for (int i = 0; i < 3; ++i) {
    const std::wstring prefix = L"plugin" + std::to_wstring(i + 1);
    settings.plugin_slots[i].vst3 = read_int((prefix + L"_type").c_str(), 0) != 0;
    settings.plugin_slots[i].path = read_string((prefix + L"_path").c_str(), L"");
    settings.plugin_slots[i].params =
        read_string((prefix + L"_params").c_str(), L"", 4096);
  }

  const bool slots_empty = settings.plugin_slots[0].path.empty() &&
                           settings.plugin_slots[1].path.empty() &&
                           settings.plugin_slots[2].path.empty();
  if (slots_empty) {
    // 旧版本把插件存在 vst2_/vst3_ 键里；首次升级时迁移到前两个槽。
    const std::wstring vst2_path = read_string(L"vst2_path", L"");
    if (!vst2_path.empty()) {
      settings.plugin_slots[0].vst3 = false;
      settings.plugin_slots[0].path = vst2_path;
      settings.plugin_slots[0].params = read_string(L"vst2_params", L"", 4096);
    }
    const std::wstring vst3_path = read_string(L"vst3_path", L"");
    if (!vst3_path.empty()) {
      PluginSlotSettings& slot = settings.plugin_slots[vst2_path.empty() ? 0 : 1];
      slot.vst3 = true;
      slot.path = vst3_path;
      slot.params = read_string(L"vst3_params", L"", 4096);
    }
    // 更早的版本只有一个插件槽；按后缀判断类型。
    if (vst2_path.empty() && vst3_path.empty()) {
      const std::wstring legacy_path = read_string(L"plugin_path", L"");
      if (!legacy_path.empty()) {
        const std::wstring legacy_params = read_string(L"plugin_params", L"", 4096);
        settings.plugin_slots[0].vst3 = ends_with_vst3(legacy_path);
        settings.plugin_slots[0].path = legacy_path;
        settings.plugin_slots[0].params = legacy_params;
      }
    }
  }

  settings.builtin_denoise = read_int(L"builtin_denoise", 1) != 0;

  // DeepFilterNet 已改为内置降噪后端；旧版本把它当普通插件放在槽里，
  // 升级后要清掉这些槽，否则会重复处理并叠加双倍延迟。
  for (auto& slot : settings.plugin_slots) {
    if (is_builtin_denoise_module(slot.path)) {
      slot.path.clear();
      slot.params.clear();
    }
  }

  settings.start_with_windows = read_int(L"start_with_windows", 0) != 0;
  settings.start_minimized = read_int(L"start_minimized", 0) != 0;
  settings.set_default_mic = read_int(L"set_default_mic", 1) != 0;
  return settings;
}

bool Settings::save() const {
  ::CreateDirectoryW(directory().c_str(), nullptr);

  // 首次创建时写入 UTF-16LE BOM：否则 profile API 会按系统代码页写 ANSI，
  // 设备名里的中文在非中文系统上会变成乱码。
  if (::GetFileAttributesW(file_path().c_str()) == INVALID_FILE_ATTRIBUTES) {
    HANDLE file = ::CreateFileW(file_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      const unsigned char bom[2] = {0xFF, 0xFE};
      DWORD written = 0;
      ::WriteFile(file, bom, sizeof(bom), &written, nullptr);
      ::CloseHandle(file);
    }
  }

  write_string(L"input_id", input_id);
  write_string(L"input_name", input_name);
  write_string(L"output_id", output_id);
  write_string(L"output_name", output_name);
  write_string(L"mic_id", mic_id);
  write_string(L"mic_name", mic_name);
  write_string(L"preset", to_wide(preset_id));

  // 浮点用整数存放，避免区域设置影响小数点解析
  write_int(L"gain_db_x10", static_cast<int>(gain_db * 10.0f));
  write_int(L"output_gain_db_x10", static_cast<int>(output_gain_db * 10.0f));
  write_int(L"denoise_percent", static_cast<int>(denoise_amount * 100.0f));

  write_int(L"clarity", clarity ? 1 : 0);
  write_int(L"body", body ? 1 : 0);
  write_int(L"leveler", leveler ? 1 : 0);
  write_int(L"deesser", deesser ? 1 : 0);

  for (int i = 0; i < 3; ++i) {
    const std::wstring prefix = L"plugin" + std::to_wstring(i + 1);
    write_int((prefix + L"_type").c_str(), plugin_slots[i].vst3 ? 1 : 0);
    write_string((prefix + L"_path").c_str(), plugin_slots[i].path);
    write_string((prefix + L"_params").c_str(), plugin_slots[i].params);
  }
  write_int(L"builtin_denoise", builtin_denoise ? 1 : 0);

  write_int(L"start_with_windows", start_with_windows ? 1 : 0);
  write_int(L"start_minimized", start_minimized ? 1 : 0);

  // 旧键传 nullptr 即删除，避免下次启动又迁移回新槽。
  for (const wchar_t* key : {L"plugin_path", L"plugin_params", L"vst2_path", L"vst2_params",
                             L"vst3_path", L"vst3_params"}) {
    ::WritePrivateProfileStringW(kSection, key, nullptr, file_path().c_str());
  }
  write_int(L"set_default_mic", set_default_mic ? 1 : 0);

  return ::GetFileAttributesW(file_path().c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace ms::config
