#include "presets/preset_library.h"

namespace ms::presets {

const std::vector<PresetTemplate>& PresetLibrary::all() {
  // 参数取值的依据：
  //   denoise 越高降噪越狠，但过高会伤齿音与气息；
  //   直播/录制留更多原始细节，会议优先"干净"。
  //   场景只决定降噪强度，不碰输入增益——增益完全由用户手动控制。
  static const std::vector<PresetTemplate> kPresets = {
      {"meeting", "会议", "别人听不出你在哪", 0.70f, -1.0f},
      {"noisy", "嘈杂环境", "空调、键盘、旁边有人说话", 0.95f, -1.0f},
      {"stream", "直播 / 录制", "保留细节与空气感", 0.50f, -1.0f},
      {"raw", "原声", "不处理，只做限幅保护", 0.0f, -1.0f},
  };
  return kPresets;
}

const PresetTemplate* PresetLibrary::find(std::string_view id) {
  for (const auto& preset : all()) {
    if (preset.id == id) {
      return &preset;
    }
  }
  return nullptr;
}

}  // namespace ms::presets
