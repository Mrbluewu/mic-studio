#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ms::presets {

// 场景预设：面向用户的是"场景"，不是参数。
// 每个模板背后就是一条链的初始参数，后续 VST 槽位也在这里扩展。
struct PresetTemplate {
  std::string id;
  std::string name;      // 会议
  std::string scenario;  // 别人听不出你在哪
  float denoise_amount = 0.0f;
  float limiter_ceiling_db = -1.0f;
};

class PresetLibrary {
 public:
  static const std::vector<PresetTemplate>& all();
  static const PresetTemplate* find(std::string_view id);
};

}  // namespace ms::presets
