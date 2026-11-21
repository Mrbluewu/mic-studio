#include "vst3_host/vst3_slot.h"

#include "platform/utf8.h"
#include "dsp_core/bounded_parameter_queue.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/connectionproxy.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ms::vst3_host {
namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr int kProcessBlock = 1024;
constexpr const char* kAudioModuleClass = "Audio Module Class";

// 最小 IComponentHandler：只承接编辑器的参数通知，不做自动化录制。
class HostComponentHandler final : public IComponentHandler {
 public:
  tresult PLUGIN_API beginEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
  tresult PLUGIN_API performEdit(ParamID id, ParamValue value) SMTG_OVERRIDE {
    pending_.try_push({static_cast<int>(id), static_cast<float>(std::clamp(value, 0.0, 1.0))});
    return kResultOk;
  }
  tresult PLUGIN_API endEdit(ParamID) SMTG_OVERRIDE { return kResultOk; }
  tresult PLUGIN_API restartComponent(int32 flags) SMTG_OVERRIDE {
    (void)flags;
    return kResultOk;
  }

  std::vector<Vst3Slot::ParameterChange> take_changes() {
    std::vector<Vst3Slot::ParameterChange> result;
    Vst3Slot::ParameterChange change;
    while (pending_.try_pop(change)) {
      result.push_back(change);
    }
    return result;
  }

  tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
    QUERY_INTERFACE(_iid, obj, FUnknown::iid, IComponentHandler)
    QUERY_INTERFACE(_iid, obj, IComponentHandler::iid, IComponentHandler)
    *obj = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
  uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }

 private:
  ms::dsp_core::BoundedParameterQueue<Vst3Slot::ParameterChange, 2048> pending_;
};

// 最小 IPlugFrame：响应插件发起的窗口尺寸变化。
class HostPlugFrame final : public IPlugFrame {
 public:
  explicit HostPlugFrame(std::function<void(int, int)> on_resize)
      : on_resize_(std::move(on_resize)) {}

  tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) SMTG_OVERRIDE {
    if (view == nullptr || newSize == nullptr) {
      return kInvalidArgument;
    }
    view->onSize(newSize);
    if (on_resize_) {
      on_resize_(newSize->getWidth(), newSize->getHeight());
    }
    return kResultOk;
  }

  tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE {
    QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPlugFrame)
    QUERY_INTERFACE(_iid, obj, IPlugFrame::iid, IPlugFrame)
    *obj = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
  uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }

 private:
  std::function<void(int, int)> on_resize_;
};

}  // namespace

struct Vst3Slot::Impl {
  // 声明顺序决定析构顺序（逆序）：接口先释放，宿主与模块最后卸载。
  VST3::Hosting::Module::Ptr module;
  IPtr<HostApplication> host_app;
  HostComponentHandler handler;
  IPtr<IComponent> component;
  IPtr<IAudioProcessor> processor;
  IPtr<IEditController> controller;
  IPtr<ConnectionProxy> component_proxy;
  IPtr<ConnectionProxy> controller_proxy;
  bool controller_from_component = false;

  // 编辑器视图（仅 UI 线程访问）
  IPtr<IPlugView> view;
  std::unique_ptr<HostPlugFrame> plug_frame;
  int editor_width = 0;
  int editor_height = 0;

  int block_size = kProcessBlock;
  double sample_rate = 48000.0;
  int latency_frames = 0;
  bool active = false;
  int64 sample_offset = 0;

  int input_channels = 0;
  int output_channels = 0;

  ms::dsp_core::BoundedParameterQueue<Vst3Slot::ParameterChange, 2048> pending_params;
  ParameterChanges in_changes{64};
  ParameterChanges out_changes{64};
  ProcessContext context{};

  std::vector<float> in_l;
  std::vector<float> in_r;
  std::vector<float> out_l;
  std::vector<float> out_r;

  void unload();
};

void Vst3Slot::Impl::unload() {
  // 先摘掉编辑器，避免父窗口销毁后插件仍引用视图。
  if (view != nullptr) {
    view->setFrame(nullptr);
    view->removed();
    view = nullptr;
  }
  plug_frame.reset();
  editor_width = 0;
  editor_height = 0;

  if (component_proxy != nullptr) {
    component_proxy->disconnect();
    component_proxy = nullptr;
  }
  if (controller_proxy != nullptr) {
    controller_proxy->disconnect();
    controller_proxy = nullptr;
  }
  if (processor != nullptr) {
    if (active) {
      processor->setProcessing(false);
    }
    processor = nullptr;
  }
  if (component != nullptr) {
    if (active) {
      component->setActive(false);
    }
    component->terminate();
  }
  active = false;
  if (controller != nullptr && !controller_from_component) {
    controller->terminate();
  }
  controller = nullptr;
  component = nullptr;
  host_app = nullptr;
  module = nullptr;

  input_channels = 0;
  output_channels = 0;
  latency_frames = 0;
  in_l.clear();
  in_r.clear();
  out_l.clear();
  out_r.clear();
}

Vst3Slot::Vst3Slot() : impl_(std::make_unique<Impl>()) {}

Vst3Slot::~Vst3Slot() { impl_->unload(); }

platform::Error Vst3Slot::load(const std::wstring& path) {
  impl_->unload();

  std::string module_error;
  auto module = VST3::Hosting::Module::create(platform::to_utf8(path), module_error);
  if (!module) {
    std::wstring message = L"加载 VST3 模块失败：" + path;
    if (!module_error.empty()) {
      message += L"（" + platform::from_utf8(module_error) + L"）";
    }
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, 0, std::move(message));
  }

  const auto factory = module->getFactory();
  VST3::Hosting::ClassInfo audio_class;
  bool found = false;
  for (const auto& info : factory.classInfos()) {
    if (info.category() == kAudioModuleClass) {
      audio_class = info;
      found = true;
      break;
    }
  }
  if (!found) {
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, 0,
                                 L"模块中没有音频效果器类：" + path);
  }

  auto component = factory.createInstance<IComponent>(audio_class.ID());
  if (!component) {
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, 0,
                                 L"创建 VST3 组件失败：" + path);
  }

  auto host_app = owned(new HostApplication());
  if (component->initialize(host_app) != kResultOk) {
    component->terminate();
    return platform::Error::make(platform::ErrorCode::StreamInitFailed, 0,
                                 L"VST3 组件初始化失败：" + path);
  }

  auto processor = FUnknownPtr<IAudioProcessor>(component.get());
  if (!processor) {
    component->terminate();
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, 0,
                                 L"插件不是音频处理器：" + path);
  }

  if (component->getBusCount(kAudio, kInput) < 1 || component->getBusCount(kAudio, kOutput) < 1) {
    component->terminate();
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, 0,
                                 L"插件缺少音频输入或输出总线：" + path);
  }

  BusInfo input_bus{};
  BusInfo output_bus{};
  const bool input_ok = component->getBusInfo(kAudio, kInput, 0, input_bus) == kResultOk &&
                        input_bus.busType == kMain && input_bus.channelCount >= 1 &&
                        input_bus.channelCount <= 2;
  const bool output_ok = component->getBusInfo(kAudio, kOutput, 0, output_bus) == kResultOk &&
                         output_bus.busType == kMain && output_bus.channelCount >= 1 &&
                         output_bus.channelCount <= 2;
  if (!input_ok || !output_ok) {
    component->terminate();
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, 0,
                                 L"只支持单声道/立体声主总线：" + path);
  }

  component->activateBus(kAudio, kInput, 0, true);
  component->activateBus(kAudio, kOutput, 0, true);

  IPtr<IEditController> controller = FUnknownPtr<IEditController>(component.get());
  const bool controller_from_component = controller != nullptr;
  if (!controller) {
    // 分离式编辑器：插件通过 controller class id 暴露独立的 IEditController。
    TUID controller_cid{};
    if (component->getControllerClassId(controller_cid) == kResultTrue) {
      controller = factory.createInstance<IEditController>(VST3::UID(controller_cid));
      if (controller && controller->initialize(host_app) != kResultOk) {
        controller = nullptr;
      }
    }
  }
  if (controller) {
    controller->setComponentHandler(&impl_->handler);

    // 处理器与编辑器分离时，需要双向消息连接，编辑器才能拿到处理器状态与参数。
    if (!controller_from_component) {
      IPtr<IConnectionPoint> component_cp = FUnknownPtr<IConnectionPoint>(component.get());
      IPtr<IConnectionPoint> controller_cp = FUnknownPtr<IConnectionPoint>(controller.get());
      if (component_cp && controller_cp) {
        auto component_proxy = owned(new ConnectionProxy(component_cp.get()));
        auto controller_proxy = owned(new ConnectionProxy(controller_cp.get()));
        if (component_proxy->connect(controller_cp.get()) == kResultTrue) {
          controller_proxy->connect(component_cp.get());
        }
        impl_->component_proxy = component_proxy;
        impl_->controller_proxy = controller_proxy;
      }
    }
  }

  impl_->module = std::move(module);
  impl_->host_app = std::move(host_app);
  impl_->component = std::move(component);
  impl_->processor = std::move(processor);
  impl_->controller = std::move(controller);
  impl_->controller_from_component = controller_from_component;
  impl_->input_channels = static_cast<int>(input_bus.channelCount);
  impl_->output_channels = static_cast<int>(output_bus.channelCount);
  impl_->block_size = kProcessBlock;

  const auto block = static_cast<std::size_t>(impl_->block_size);
  impl_->in_l.assign(block, 0.0f);
  impl_->in_r.assign(block, 0.0f);
  impl_->out_l.assign(block, 0.0f);
  impl_->out_r.assign(block, 0.0f);
  return {};
}

void Vst3Slot::prepare(const dsp_core::ProcessContext& context) {
  Impl& state = *impl_;
  if (state.processor == nullptr || state.component == nullptr) {
    return;
  }

  if (state.active) {
    state.processor->setProcessing(false);
    state.component->setActive(false);
    state.active = false;
  }

  state.sample_rate = context.sampleRate > 0.0 ? context.sampleRate : 48000.0;
  state.block_size = kProcessBlock;
  state.sample_offset = 0;

  ProcessSetup setup{};
  setup.processMode = kRealtime;
  setup.symbolicSampleSize = kSample32;
  setup.maxSamplesPerBlock = state.block_size;
  setup.sampleRate = state.sample_rate;

  if (state.processor->setupProcessing(setup) != kResultOk) {
    return;
  }
  if (state.component->setActive(true) != kResultOk) {
    return;
  }
  if (state.processor->setProcessing(true) != kResultOk) {
    state.component->setActive(false);
    return;
  }
  state.active = true;
  state.latency_frames = std::max<int32>(0, state.processor->getLatencySamples());

  state.context = ProcessContext{};
  state.context.sampleRate = state.sample_rate;
  state.context.projectTimeSamples = 0;
  state.context.state = ProcessContext::kPlaying;
  state.context.tempo = 120.0;
  state.context.timeSigNumerator = 4;
  state.context.timeSigDenominator = 4;

  // 预跑低电平噪声：部分插件在首次处理"非静音"音频时才初始化模型，
  // 若留到实时线程会阻塞几百毫秒，把环缓冲水位冲高、永久增加延迟。
  std::vector<float> warmup(static_cast<std::size_t>(state.block_size), 0.0f);
  std::uint32_t seed = 0x12345678u;
  for (int block = 0; block < 64; ++block) {
    for (auto& sample : warmup) {
      seed = seed * 1664525u + 1013904223u;
      sample = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 1.0e-3f;
    }
    process(warmup.data(), state.block_size);
  }
  reset();
}

void Vst3Slot::process(float* data, int frames) {
  Impl& state = *impl_;
  if (state.processor == nullptr || state.component == nullptr || !state.active || frames <= 0) {
    return;
  }

  Vst3Slot::ParameterChange change;
  for (std::size_t count = 0; count < state.pending_params.capacity() &&
                              state.pending_params.try_pop(change); ++count) {
    int32 queue_index = 0;
    auto* queue = state.in_changes.addParameterData(static_cast<ParamID>(change.id), queue_index);
    if (queue != nullptr) {
      int32 point_index = 0;
      // 单个参数点写入失败（该参数队列在单块内写满）只丢弃这一个点，
      // 不能 clearQueue，否则本块已排队的其它参数会被一并清掉。
      queue->addPoint(0, static_cast<ParamValue>(change.value), point_index);
    }
  }

  int offset = 0;
  while (offset < frames) {
    const int count = std::min(frames - offset, state.block_size);
    for (int i = 0; i < count; ++i) {
      const auto index = static_cast<std::size_t>(i);
      const float sample = data[offset + i];
      state.in_l[index] = sample;
      state.in_r[index] = sample;
      state.out_l[index] = 0.0f;
      state.out_r[index] = 0.0f;
    }

    float* input_channels[2] = {state.in_l.data(), state.in_r.data()};
    AudioBusBuffers input_bus{};
    input_bus.numChannels = state.input_channels;
    input_bus.channelBuffers32 = input_channels;

    float* output_channels[2] = {state.out_l.data(), state.out_r.data()};
    AudioBusBuffers output_bus{};
    output_bus.numChannels = state.output_channels;
    output_bus.channelBuffers32 = output_channels;

    ProcessData process_data{};
    process_data.processMode = kRealtime;
    process_data.symbolicSampleSize = kSample32;
    process_data.numSamples = count;
    process_data.numInputs = 1;
    process_data.numOutputs = 1;
    process_data.inputs = &input_bus;
    process_data.outputs = &output_bus;
    process_data.inputParameterChanges = &state.in_changes;
    process_data.outputParameterChanges = &state.out_changes;
    process_data.processContext = &state.context;

    state.processor->process(process_data);
    state.context.projectTimeSamples += count;

    for (int i = 0; i < count; ++i) {
      const auto index = static_cast<std::size_t>(i);
      if (state.output_channels > 1) {
        data[offset + i] = 0.5f * (state.out_l[index] + state.out_r[index]);
      } else {
        data[offset + i] = state.out_l[index];
      }
    }
    offset += count;
  }

  state.in_changes.clearQueue();
  state.out_changes.clearQueue();
}

void Vst3Slot::reset() {
  std::fill(impl_->in_l.begin(), impl_->in_l.end(), 0.0f);
  std::fill(impl_->in_r.begin(), impl_->in_r.end(), 0.0f);
  std::fill(impl_->out_l.begin(), impl_->out_l.end(), 0.0f);
  std::fill(impl_->out_r.begin(), impl_->out_r.end(), 0.0f);
}

bool Vst3Slot::loaded() const noexcept {
  return impl_ != nullptr && impl_->processor != nullptr;
}

std::vector<Vst3Slot::ParameterInfo> Vst3Slot::parameters() const {
  std::vector<ParameterInfo> result;
  const Impl& state = *impl_;
  if (state.controller == nullptr) {
    return result;
  }

  const int32 count = state.controller->getParameterCount();
  if (count <= 0) {
    return result;
  }

  result.reserve(static_cast<std::size_t>(count));
  for (int32 i = 0; i < count; ++i) {
    Steinberg::Vst::ParameterInfo info{};
    if (state.controller->getParameterInfo(i, info) != kResultOk) {
      continue;
    }
    ParameterInfo parameter;
    parameter.id = static_cast<int>(info.id);
    parameter.name = platform::from_utf8(VST3::StringConvert::convert(info.title));
    parameter.value = static_cast<float>(state.controller->getParamNormalized(info.id));
    result.push_back(std::move(parameter));
  }
  return result;
}

void Vst3Slot::set_parameter(int id, float value) {
  Impl& state = *impl_;
  if (id < 0) {
    return;
  }
  state.pending_params.try_push({id, std::clamp(value, 0.0f, 1.0f)});
}

std::vector<Vst3Slot::ParameterChange> Vst3Slot::take_parameter_changes() {
  return impl_->handler.take_changes();
}

int Vst3Slot::latency_frames() const noexcept { return impl_->latency_frames; }

bool Vst3Slot::has_editor() const { return impl_->controller != nullptr; }

bool Vst3Slot::open_editor(void* parent, int& width, int& height) {
  Impl& state = *impl_;
  if (state.controller == nullptr || parent == nullptr) {
    return false;
  }
  if (state.view != nullptr) {
    width = state.editor_width;
    height = state.editor_height;
    return true;
  }

  IPtr<IPlugView> view = owned(state.controller->createView(ViewType::kEditor));
  if (view == nullptr) {
    return false;
  }
  if (view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
    return false;
  }

  state.plug_frame = std::make_unique<HostPlugFrame>([&state](int new_width, int new_height) {
    state.editor_width = new_width;
    state.editor_height = new_height;
  });
  if (view->setFrame(state.plug_frame.get()) != kResultOk) {
    state.plug_frame.reset();
    return false;
  }
  if (view->attached(parent, kPlatformTypeHWND) != kResultOk) {
    view->setFrame(nullptr);
    state.plug_frame.reset();
    return false;
  }

  ViewRect rect{};
  if (view->getSize(&rect) == kResultOk) {
    state.editor_width = rect.getWidth();
    state.editor_height = rect.getHeight();
  }
  if (state.editor_width <= 0) {
    state.editor_width = 720;
  }
  if (state.editor_height <= 0) {
    state.editor_height = 480;
  }

  state.view = std::move(view);
  width = state.editor_width;
  height = state.editor_height;
  return true;
}

void Vst3Slot::close_editor() {
  Impl& state = *impl_;
  if (state.view != nullptr) {
    state.view->setFrame(nullptr);
    state.view->removed();
    state.view = nullptr;
  }
  state.plug_frame.reset();
  state.editor_width = 0;
  state.editor_height = 0;
}

void Vst3Slot::editor_idle() {
  // VST3 编辑器自带走时器，无需宿主周期驱动。
}

bool Vst3Slot::editor_size(int& width, int& height) const {
  const Impl& state = *impl_;
  if (state.view == nullptr || state.editor_width <= 0 || state.editor_height <= 0) {
    return false;
  }
  width = state.editor_width;
  height = state.editor_height;
  return true;
}

}  // namespace ms::vst3_host
