#include "vst2_host/vst2_slot.h"

#include "platform/utf8.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace ms::vst2_host {

// VST2 ABI 结构（对应 SDK 的 AEffect，x64 布局）。这里自行声明，
// 避免为了一个插槽引入完整的 VST2 SDK。
struct AEffect {
  std::int32_t magic;
  intptr_t (*dispatcher)(AEffect*, std::int32_t, std::int32_t, intptr_t, void*, float);
  void (*process)(AEffect*, float**, float**, std::int32_t);
  void (*setParameter)(AEffect*, std::int32_t, float);
  float (*getParameter)(AEffect*, std::int32_t);
  std::int32_t numPrograms;
  std::int32_t numParams;
  std::int32_t numInputs;
  std::int32_t numOutputs;
  std::int32_t flags;
  intptr_t reserved1;
  intptr_t reserved2;
  std::int32_t initialDelay;
  std::int32_t realQualities;
  std::int32_t offQualities;
  float ioRatio;
  void* object;
  void* user;
  std::int32_t uniqueID;
  std::int32_t version;
  void (*processReplacing)(AEffect*, float**, float**, std::int32_t);
  void (*processDoubleReplacing)(AEffect*, double**, double**, std::int32_t);
  char future[56];
};

namespace {

constexpr std::int32_t kEffectMagic = 0x56737450;  // 'VstP'
constexpr std::int32_t kCanReplacing = 1 << 4;
constexpr std::int32_t kHasEditor = 1 << 0;
constexpr int kProcessBlock = 1024;  // 送给插件的固定块长
constexpr int kHostVersion = 2400;   // VST 2.4

// 插件 → 宿主
enum : std::int32_t {
  effOpen = 0,
  effClose = 1,
  effGetParamName = 8,
  effSetSampleRate = 10,
  effSetBlockSize = 11,
  effMainsChanged = 12,
  effEditGetRect = 13,
  effEditOpen = 14,
  effEditClose = 15,
  effEditIdle = 19,
};

// effEditGetRect 返回的编辑器矩形（VST2 ABI）
struct ERect {
  std::int16_t top;
  std::int16_t left;
  std::int16_t bottom;
  std::int16_t right;
};

// 宿主 → 插件
enum : std::int32_t {
  audioMasterAutomate = 0,
  audioMasterVersion = 1,
  audioMasterCurrentId = 2,
  audioMasterIdle = 3,
  audioMasterGetTime = 7,
  audioMasterGetSampleRate = 16,
  audioMasterGetBlockSize = 17,
  audioMasterGetInputLatency = 18,
  audioMasterGetOutputLatency = 19,
  audioMasterWillReplaceOrAccumulate = 22,
  audioMasterGetCurrentProcessLevel = 23,
  audioMasterGetVendorString = 32,
  audioMasterGetProductString = 33,
  audioMasterGetVendorVersion = 34,
  audioMasterCanDo = 37,
  audioMasterUpdateDisplay = 42,
};

using PluginEntryProc = AEffect* (*)(intptr_t (*)(AEffect*, std::int32_t, std::int32_t, intptr_t,
                                                  void*, float));

template <typename Proc>
Proc resolve_entry(HMODULE module, const char* name) {
  return reinterpret_cast<Proc>(reinterpret_cast<void*>(::GetProcAddress(module, name)));
}

void copy_text(char* destination, const char* source) {
  if (destination == nullptr) {
    return;
  }
  ::strcpy_s(destination, 64, source);
}

}  // namespace

Vst2Slot::Vst2Slot() = default;

Vst2Slot::~Vst2Slot() { close(); }

platform::Error Vst2Slot::load(const std::wstring& path) {
  close();

  HMODULE module = ::LoadLibraryW(path.c_str());
  if (module == nullptr) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed,
                                 static_cast<long>(::GetLastError()),
                                 L"加载 VST2 插件失败：" + path);
  }

  PluginEntryProc entry = resolve_entry<PluginEntryProc>(module, "VSTPluginMain");
  if (entry == nullptr) {
    entry = resolve_entry<PluginEntryProc>(module, "main");
  }
  if (entry == nullptr) {
    ::FreeLibrary(module);
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, E_INVALIDARG,
                                 L"不是有效的 VST2 插件（缺少 VSTPluginMain）：" + path);
  }

  AEffect* effect = entry(&Vst2Slot::host_callback);
  if (effect == nullptr || effect->magic != kEffectMagic || effect->dispatcher == nullptr) {
    if (effect != nullptr && effect->dispatcher != nullptr) {
      effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    }
    ::FreeLibrary(module);
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, E_INVALIDARG,
                                 L"插件未返回有效的 AEffect：" + path);
  }

  // reserved1 留给宿主自己，插件不应改写；回调里靠它找回 this。
  effect->reserved1 = reinterpret_cast<intptr_t>(this);
  effect->user = this;
  effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);

  if (effect->numInputs < 1 || effect->numInputs > 2 || effect->numOutputs < 1 ||
      effect->numOutputs > 2) {
    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    ::FreeLibrary(module);
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, E_INVALIDARG,
                                 L"只支持单声道/立体声效果器：" + path);
  }
  if ((effect->flags & kCanReplacing) == 0 || effect->processReplacing == nullptr) {
    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
    ::FreeLibrary(module);
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, E_INVALIDARG,
                                 L"插件不支持 32-bit float 处理（processReplacing）：" + path);
  }

  module_ = module;
  effect_ = effect;
  num_outputs_ = static_cast<int>(effect->numOutputs);
  latency_frames_ = std::max<std::int32_t>(0, effect->initialDelay);
  block_size_ = kProcessBlock;

  in_l_.assign(static_cast<std::size_t>(block_size_), 0.0f);
  in_r_.assign(static_cast<std::size_t>(block_size_), 0.0f);
  out_l_.assign(static_cast<std::size_t>(block_size_), 0.0f);
  out_r_.assign(static_cast<std::size_t>(block_size_), 0.0f);
  return {};
}

void Vst2Slot::prepare(const dsp_core::ProcessContext& context) {
  if (effect_ == nullptr) {
    return;
  }

  sample_rate_ = context.sampleRate > 0.0 ? context.sampleRate : 48000.0;
  block_size_ = kProcessBlock;

  effect_->dispatcher(effect_, effSetSampleRate, 0, 0, nullptr,
                      static_cast<float>(sample_rate_));
  effect_->dispatcher(effect_, effSetBlockSize, 0, block_size_, nullptr, 0.0f);
  effect_->dispatcher(effect_, effMainsChanged, 0, 0, nullptr, 0.0f);
  effect_->dispatcher(effect_, effMainsChanged, 0, 1, nullptr, 0.0f);
  latency_frames_ = std::max<std::int32_t>(0, effect_->initialDelay);

  // 预跑一段低电平噪声：部分插件（如 DeepFilterNet）在首次处理"非静音"音频时
  // 才初始化模型，若留到实时线程会阻塞几百毫秒，把环缓冲水位冲高、永久增加延迟。
  if (!in_l_.empty()) {
    float* inputs[2] = {in_l_.data(), in_r_.data()};
    float* outputs[2] = {out_l_.data(), out_r_.data()};
    std::uint32_t seed = 0x12345678u;
    for (int block = 0; block < 64; ++block) {
      for (std::size_t i = 0; i < in_l_.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float sample = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 1.0e-3f;
        in_l_[i] = sample;
        in_r_[i] = sample;
      }
      std::fill(out_l_.begin(), out_l_.end(), 0.0f);
      std::fill(out_r_.begin(), out_r_.end(), 0.0f);
      effect_->processReplacing(effect_, inputs, outputs, block_size_);
    }
  }
  reset();
}

void Vst2Slot::process(float* data, int frames) {
  if (effect_ == nullptr || frames <= 0) {
    return;
  }

  int offset = 0;
  while (offset < frames) {
    const int count = std::min(frames - offset, block_size_);
    for (int i = 0; i < count; ++i) {
      const std::size_t index = static_cast<std::size_t>(i);
      const float sample = data[offset + i];
      in_l_[index] = sample;
      in_r_[index] = sample;
      out_l_[index] = 0.0f;
      out_r_[index] = 0.0f;
    }

    float* inputs[2] = {in_l_.data(), in_r_.data()};
    float* outputs[2] = {out_l_.data(), out_r_.data()};
    effect_->processReplacing(effect_, inputs, outputs, count);

    if (num_outputs_ > 1) {
      for (int i = 0; i < count; ++i) {
        const std::size_t index = static_cast<std::size_t>(i);
        data[offset + i] = 0.5f * (out_l_[index] + out_r_[index]);
      }
    } else {
      std::copy_n(out_l_.begin(), count, data + offset);
    }
    offset += count;
  }
}

void Vst2Slot::reset() {
  std::fill(in_l_.begin(), in_l_.end(), 0.0f);
  std::fill(in_r_.begin(), in_r_.end(), 0.0f);
  std::fill(out_l_.begin(), out_l_.end(), 0.0f);
  std::fill(out_r_.begin(), out_r_.end(), 0.0f);
}

std::vector<Vst2Slot::ParameterInfo> Vst2Slot::parameters() const {
  std::vector<ParameterInfo> result;
  if (effect_ == nullptr || effect_->numParams <= 0) {
    return result;
  }

  result.reserve(static_cast<std::size_t>(effect_->numParams));
  for (int i = 0; i < effect_->numParams; ++i) {
    char name[64] = {};
    effect_->dispatcher(effect_, effGetParamName, i, 0, name, 0.0f);

    ParameterInfo parameter;
    parameter.id = i;
    parameter.name =
        name[0] != '\0' ? platform::from_utf8(name) : (L"参数 " + std::to_wstring(i));
    parameter.value =
        effect_->getParameter != nullptr ? effect_->getParameter(effect_, i) : 0.0f;
    result.push_back(std::move(parameter));
  }
  return result;
}

void Vst2Slot::set_parameter(int index, float value) {
  if (effect_ == nullptr || effect_->setParameter == nullptr) {
    return;
  }
  if (index < 0 || index >= effect_->numParams) {
    return;
  }
  effect_->setParameter(effect_, index, std::clamp(value, 0.0f, 1.0f));
}

void Vst2Slot::record_parameter_change(int index, float value) {
  if (index < 0 || effect_ == nullptr || index >= effect_->numParams) {
    return;
  }
  pending_changes_.try_push({index, std::clamp(value, 0.0f, 1.0f)});
}

std::vector<Vst2Slot::ParameterChange> Vst2Slot::take_parameter_changes() {
  std::vector<ParameterChange> result;
  ParameterChange change;
  while (pending_changes_.try_pop(change)) {
    result.push_back(change);
  }
  return result;
}

bool Vst2Slot::has_editor() const {
  return effect_ != nullptr && (effect_->flags & kHasEditor) != 0;
}

bool Vst2Slot::open_editor(void* parent, int& width, int& height) {
  if (!has_editor() || parent == nullptr) {
    return false;
  }
  if (editor_open_) {
    width = editor_width_;
    height = editor_height_;
    return true;
  }

  if (effect_->dispatcher(effect_, effEditOpen, 0, 0, parent, 0.0f) == 0) {
    return false;
  }
  editor_open_ = true;

  ERect* rect = nullptr;
  effect_->dispatcher(effect_, effEditGetRect, 0, 0, &rect, 0.0f);
  if (rect != nullptr) {
    editor_width_ = rect->right - rect->left;
    editor_height_ = rect->bottom - rect->top;
  }
  if (editor_width_ <= 0) {
    editor_width_ = 720;
  }
  if (editor_height_ <= 0) {
    editor_height_ = 480;
  }

  width = editor_width_;
  height = editor_height_;
  return true;
}

void Vst2Slot::close_editor() {
  if (effect_ != nullptr && editor_open_) {
    effect_->dispatcher(effect_, effEditClose, 0, 0, nullptr, 0.0f);
    editor_open_ = false;
  }
  editor_width_ = 0;
  editor_height_ = 0;
}

void Vst2Slot::editor_idle() {
  if (effect_ != nullptr && editor_open_) {
    effect_->dispatcher(effect_, effEditIdle, 0, 0, nullptr, 0.0f);
  }
}

bool Vst2Slot::editor_size(int& width, int& height) const {
  if (!editor_open_ || editor_width_ <= 0 || editor_height_ <= 0) {
    return false;
  }
  width = editor_width_;
  height = editor_height_;
  return true;
}

intptr_t Vst2Slot::host_callback(AEffect* effect, std::int32_t opcode, std::int32_t index,
                                 intptr_t value, void* ptr, float opt) {
  auto* self = effect != nullptr ? reinterpret_cast<Vst2Slot*>(effect->reserved1) : nullptr;
  (void)value;

  switch (opcode) {
    case audioMasterAutomate:
      if (self != nullptr) {
        self->record_parameter_change(index, opt);
      }
      return 0;
    case audioMasterVersion:
      return kHostVersion;
    case audioMasterIdle:
      return 0;
    case audioMasterGetTime:
      return 0;
    case audioMasterGetSampleRate:
      return static_cast<intptr_t>(self != nullptr ? self->sample_rate_ : 48000.0);
    case audioMasterGetBlockSize:
      return self != nullptr ? self->block_size_ : kProcessBlock;
    case audioMasterGetInputLatency:
    case audioMasterGetOutputLatency:
      return 0;
    case audioMasterWillReplaceOrAccumulate:
      return 1;
    case audioMasterGetCurrentProcessLevel:
      return 2;  // kVstProcessLevelRealtime
    case audioMasterGetVendorString:
      copy_text(static_cast<char*>(ptr), "MicStudio");
      return 0;
    case audioMasterGetProductString:
      copy_text(static_cast<char*>(ptr), "MicStudio Voice Engine");
      return 0;
    case audioMasterGetVendorVersion:
      return 1000;
    case audioMasterCanDo: {
      const auto* text = static_cast<const char*>(ptr);
      if (text == nullptr) {
        return 0;
      }
      if (std::strcmp(text, "startStopProcess") == 0 || std::strcmp(text, "bypass") == 0) {
        return 1;
      }
      return 0;
    }
    case audioMasterUpdateDisplay:
      return 0;
    default:
      return 0;
  }
}

void Vst2Slot::close() {
  close_editor();
  ParameterChange discarded;
  while (pending_changes_.try_pop(discarded)) {
  }
  if (effect_ != nullptr) {
    effect_->dispatcher(effect_, effClose, 0, 0, nullptr, 0.0f);
    effect_ = nullptr;
  }
  if (module_ != nullptr) {
    ::FreeLibrary(static_cast<HMODULE>(module_));
    module_ = nullptr;
  }
  num_outputs_ = 0;
  latency_frames_ = 0;
  in_l_.clear();
  in_r_.clear();
  out_l_.clear();
  out_r_.clear();
}

}  // namespace ms::vst2_host
