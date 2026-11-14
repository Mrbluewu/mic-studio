#include "denoise/deepfilter_denoiser.h"

#include <algorithm>
#include <cwctype>
#include <winerror.h>

namespace ms::denoise {
namespace {

// 在插件参数表里找 “Denoise Amount” 这类强度参数。
// DeepFilterNet 的该参数是 0–100 dB 的衰减上限，归一化值线性对应，
// 用它控制强度既符合插件语义，也避免干湿两路叠加。
int find_amount_parameter(const std::vector<vst2_host::Vst2Slot::ParameterInfo>& parameters) {
  for (const auto& parameter : parameters) {
    std::wstring lowered = parameter.name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (lowered.find(L"amount") != std::wstring::npos) {
      return parameter.id;
    }
  }
  return -1;
}

}  // namespace

DeepFilterDenoiser::~DeepFilterDenoiser() = default;

platform::Error DeepFilterDenoiser::load(const std::wstring& path) {
  if (path.empty()) {
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, E_INVALIDARG,
                                 L"DeepFilterNet 路径为空");
  }
  return slot_.load(path);
}

void DeepFilterDenoiser::prepare(const dsp_core::ProcessContext& context) {
  slot_.prepare(context);
  amount_index_ = find_amount_parameter(slot_.parameters());
  reset();
}

void DeepFilterDenoiser::reset() {
  slot_.reset();
  pushed_amount_ = -1.0f;  // 下次 process 重新下发强度
}

void DeepFilterDenoiser::set_amount(float amount) noexcept {
  amount_.store(std::clamp(amount, 0.0f, 1.0f));
}

void DeepFilterDenoiser::push_amount() {
  const float amount = amount_.load(std::memory_order_relaxed);
  if (amount == pushed_amount_) {
    return;
  }
  slot_.set_parameter(amount_index_, amount);
  pushed_amount_ = amount;
}

void DeepFilterDenoiser::process(float* data, int frames) {
  if (!slot_.loaded() || frames <= 0 ||
      amount_.load(std::memory_order_relaxed) <= 0.001f) {
    return;  // 关闭时完全直通，不引入延迟
  }

  // 强度交给插件原生参数，全程只有一路（湿）信号。
  // 这里不再做宿主端干湿混合：插件自报延迟与真实延迟不符，
  // 混合会变成两路错开约 30 ms 的同一段语音，就是“声音重复”的来源。
  push_amount();
  slot_.process(data, frames);
}

}  // namespace ms::denoise
