#include "engine/audio_engine.h"

#include "analysis/noise_floor.h"
#include "audio_io/capture_stream.h"
#include "audio_io/device_watcher.h"
#include "audio_io/render_stream.h"
#include "dsp_core/basic_processors.h"
#include "dsp_core/format_bridge.h"
#include "dsp_core/processor.h"
#include "dsp_core/rate_converter.h"
#include "denoise/deepfilter_denoiser.h"
#include "denoise/denoiser.h"
#include "effects/effect_cards.h"
#include "engine/clock_drift_controller.h"
#include "engine/tap_buffer.h"
#include "ring_buffer/spsc_ring_buffer.h"
#include "vst2_host/vst2_slot.h"
#include "vst3_host/vst3_slot.h"

#include <winerror.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ms::engine {
namespace {
constexpr std::size_t kCaptureScratchFrames = 4096;
constexpr std::size_t kRenderScratchFrames = 16384;
constexpr std::size_t kRingCapacityFrames = 48000;  // 1 秒，远大于 20 ms 目标水位
constexpr std::size_t kTapFrames = 48000 * 4;       // A/B 对比保留最近 4 秒
constexpr double kConverterFifoFrames = 2.0;        // 重采样器内部只需保留插值所需的最小值
}  // namespace

struct AudioEngine::Impl {
  std::atomic<EngineState> state{EngineState::Stopped};

  audio_io::CaptureStream capture;
  audio_io::RenderStream render;
  audio_io::DeviceWatcher watcher;
  dsp_core::CaptureBridge capture_bridge;
  dsp_core::RenderBridge render_bridge;
  ring_buffer::SpscRingBuffer ring{kRingCapacityFrames};
  ClockDriftController drift;

  // 处理链：48 kHz mono 上依次执行，降噪与 VST 后续也挂在这里。
  dsp_core::Chain chain;
  dsp_core::GainProcessor* gain = nullptr;
  // 输出增益：处理链之后、重采样之前，只影响监听输出，不参与底噪测量。
  std::unique_ptr<dsp_core::GainProcessor> out_gain;
  dsp_core::Limiter* limiter = nullptr;
  denoise::Denoiser* denoise = nullptr;
  denoise::DeepFilterDenoiser* deepfilter = nullptr;
  bool deepfilter_active = false;
  // 内置降噪加载失败的原因（空串表示正常）。
  std::wstring builtin_error;

  // 已加载的插件槽（按槽号记录，链上串接顺序与槽号一致）。
  struct LoadedPlugin {
    int slot = 0;
    bool is_vst3 = false;
    vst2_host::Vst2Slot* vst2 = nullptr;
    vst3_host::Vst3Slot* vst3 = nullptr;
  };
  std::vector<LoadedPlugin> plugins;
  std::vector<std::wstring> plugin_errors;

  effects::ClarityEffect* clarity = nullptr;
  effects::BodyEffect* body = nullptr;
  effects::LevelerEffect* leveler = nullptr;
  effects::DeEsserEffect* deesser = nullptr;

  // 链按“内置降噪后端 + 插件组合”缓存：相同组合的重启只重新 prepare，避免反复加载模型。
  std::wstring chain_signature;

  // 插件参数表在流启动前采样并缓存，GUI 读取时不再触碰插件（避免阻塞音频线程）。
  std::vector<PluginParameterInfo> plugin_info;

  // 输出侧驱动的重采样器：FIFO 内的样本不会被丢弃，长期不漂移。
  dsp_core::RateConverter converter;

  // A/B 双轨缓存：处理前 / 处理后，同一线程写入，天然对齐。
  TapBuffer raw_tap{kTapFrames};
  TapBuffer processed_tap{kTapFrames};
  analysis::NoiseFloorEstimator noise_in;
  analysis::NoiseFloorEstimator noise_out;

  std::vector<float> capture_scratch = std::vector<float>(kCaptureScratchFrames, 0.0f);
  std::vector<float> render_in = std::vector<float>(kRenderScratchFrames, 0.0f);
  std::vector<float> render_scratch = std::vector<float>(kRenderScratchFrames, 0.0f);

  bool warmed_up = false;
  // 取输入时的小数累加器，保证 ppm 级比率修正不会被整块取整吞掉
  double pull_accumulator = 0.0;

  std::atomic<std::uint64_t> captured{0};
  std::atomic<std::uint64_t> rendered{0};
  std::atomic<std::uint64_t> overruns{0};
  std::atomic<std::uint64_t> underruns{0};
  std::atomic<std::uint32_t> fill_frames{0};
  std::atomic<double> applied_ppm{0.0};
  std::atomic<std::uint32_t> recoveries{0};
  std::atomic<bool> bypass{false};

  double engine_rate = 48000.0;
  double target_fill_frames = 960.0;
  std::uint32_t capture_period_frames = 480;
  std::uint32_t render_period_frames = 480;

  EngineOptions options;
  std::wstring active_input_id;
  std::wstring active_output_id;

  std::recursive_mutex pipeline_mutex;
  std::thread watchdog;
  std::atomic<bool> watchdog_stop{false};
  std::atomic<bool> recovery_requested{false};

  platform::Error start_pipeline(const EngineOptions& opts);
  void stop_pipeline();
  void start_watchdog();
  void stop_watchdog();
};

platform::Error AudioEngine::Impl::start_pipeline(const EngineOptions& opts) {
  std::lock_guard<std::recursive_mutex> lock(pipeline_mutex);

  stop_pipeline();

  engine_rate = static_cast<double>(opts.engine_rate == 0 ? 48000 : opts.engine_rate);
  target_fill_frames = engine_rate * opts.target_fill_ms / 1000.0;
  state.store(EngineState::Starting);
  ring.reset();
  captured.store(0);
  rendered.store(0);
  overruns.store(0);
  underruns.store(0);
  applied_ppm.store(0.0);
  warmed_up = false;
  pull_accumulator = 0.0;
  noise_in.reset();
  noise_out.reset();

  auto error = capture.open(opts.input_endpoint_id);
  if (error.failed()) {
    state.store(EngineState::Failed);
    return error;
  }

  error = render.open(opts.output_endpoint_id);
  if (error.failed()) {
    capture.stop();
    state.store(EngineState::Failed);
    return error;
  }

  capture_bridge.prepare(capture.format(), opts.engine_rate);
  render_bridge.prepare(render.format(), opts.engine_rate);
  if (!capture_bridge.supported()) {
    state.store(EngineState::Failed);
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, E_INVALIDARG,
                                 L"采集端点格式不受支持（仅支持 32-bit float / 16-bit int）");
  }
  if (!render_bridge.supported()) {
    state.store(EngineState::Failed);
    return platform::Error::make(platform::ErrorCode::FormatUnsupported, E_INVALIDARG,
                                 L"渲染端点格式不受支持（仅支持 32-bit float / 16-bit int）");
  }

  const double nominal_step = engine_rate / static_cast<double>(render_bridge.device_rate());
  drift.prepare(nominal_step, target_fill_frames);
  converter.prepare(nominal_step);

  // 链按“降噪后端 + 插件组合”缓存，相同组合的重启只重新 prepare。
  auto build_signature = [&opts] {
    std::wstring signature = opts.builtin_denoise ? L"1" : L"0";
    signature += L"|";
    signature += opts.deepfilter_path;
    for (const auto& slot : opts.plugins) {
      signature += L"|";
      signature += slot.vst3 ? L"3" : L"2";
      signature += slot.path;
    }
    return signature;
  };
  const std::wstring signature = build_signature();
  if (chain.size() == 0 || chain_signature != signature) {
    chain = dsp_core::Chain{};
    gain = nullptr;
    limiter = nullptr;
    denoise = nullptr;
    deepfilter = nullptr;
    deepfilter_active = false;
    plugins.clear();
    plugin_errors.clear();
    clarity = nullptr;
    body = nullptr;
    leveler = nullptr;
    deesser = nullptr;
    chain_signature.clear();
    plugin_info.clear();

    auto gain_stage = std::make_unique<dsp_core::GainProcessor>();

    // 内置降噪：固定使用 DeepFilterNet（VST2 DLL），不做其它后端回退。
    std::unique_ptr<denoise::Denoiser> denoise_stage;
    builtin_error.clear();
    if (opts.builtin_denoise) {
      if (opts.deepfilter_path.empty()) {
        builtin_error = L"未指定 DeepFilterNet.dll 路径";
      } else {
        auto dfn = std::make_unique<denoise::DeepFilterDenoiser>();
        const auto load_error = dfn->load(opts.deepfilter_path);
        if (load_error.ok()) {
          deepfilter = dfn.get();
          deepfilter_active = true;
          denoise_stage = std::move(dfn);
        } else {
          builtin_error = load_error.message;
        }
      }
    }

    struct PendingPlugin {
      int slot = 0;
      std::unique_ptr<dsp_core::IProcessor> stage;
    };
    std::vector<PendingPlugin> pending_plugins;
    for (std::size_t i = 0; i < opts.plugins.size() && i < 3; ++i) {
      const auto& slot = opts.plugins[i];
      plugin_errors.push_back(L"");
      if (slot.path.empty()) {
        continue;
      }
      if (slot.vst3) {
        auto stage = std::make_unique<vst3_host::Vst3Slot>();
        const auto plugin_error = stage->load(slot.path);
        if (plugin_error.failed()) {
          plugin_errors.back() = plugin_error.message;
          continue;
        }
        plugins.push_back(LoadedPlugin{static_cast<int>(i), true, nullptr, stage.get()});
        pending_plugins.push_back(PendingPlugin{static_cast<int>(i), std::move(stage)});
      } else {
        auto stage = std::make_unique<vst2_host::Vst2Slot>();
        const auto plugin_error = stage->load(slot.path);
        if (plugin_error.failed()) {
          plugin_errors.back() = plugin_error.message;
          continue;
        }
        plugins.push_back(LoadedPlugin{static_cast<int>(i), false, stage.get(), nullptr});
        pending_plugins.push_back(PendingPlugin{static_cast<int>(i), std::move(stage)});
      }
    }

    auto clarity_stage = std::make_unique<effects::ClarityEffect>();
    auto body_stage = std::make_unique<effects::BodyEffect>();
    auto leveler_stage = std::make_unique<effects::LevelerEffect>();
    auto deesser_stage = std::make_unique<effects::DeEsserEffect>();
    auto dc_stage = std::make_unique<dsp_core::DcBlocker>();
    auto limiter_stage = std::make_unique<dsp_core::Limiter>();
    gain = gain_stage.get();
    denoise = denoise_stage.get();
    clarity = clarity_stage.get();
    body = body_stage.get();
    leveler = leveler_stage.get();
    deesser = deesser_stage.get();
    limiter = limiter_stage.get();
    chain.add(std::move(gain_stage));
    if (denoise_stage) {
      chain.add(std::move(denoise_stage));
    }
    // 高质量降噪/润色交给插件，位置固定在内置降噪之后，按槽号顺序串接。
    for (auto& pending : pending_plugins) {
      chain.add(std::move(pending.stage));
    }
    chain.add(std::move(clarity_stage));
    chain.add(std::move(body_stage));
    chain.add(std::move(leveler_stage));
    chain.add(std::move(deesser_stage));
    chain.add(std::move(dc_stage));
    chain.add(std::move(limiter_stage));
    chain_signature = signature;
  }
  {
    dsp_core::ProcessContext context;
    context.sampleRate = engine_rate;
    context.maxBlockSize = static_cast<int>(kRenderScratchFrames);
    chain.prepare(context);
    chain.reset();
    gain->set_gain_db(opts.input_gain_db);
    out_gain = std::make_unique<dsp_core::GainProcessor>();
    out_gain->set_gain_db(opts.output_gain_db);
    out_gain->prepare(context);
    out_gain->reset();
    if (denoise != nullptr) {
      denoise->set_amount(opts.denoise_amount);
    }
    clarity->set_enabled(opts.clarity);
    body->set_enabled(opts.body);
    leveler->set_enabled(opts.leveler);
    deesser->set_enabled(opts.deesser);
    limiter->set_ceiling_db(-1.0f);
    for (const auto& loaded : plugins) {
      if (loaded.slot >= static_cast<int>(opts.plugins.size())) {
        continue;
      }
      for (const auto& parameter : opts.plugins[loaded.slot].params) {
        if (loaded.vst3 && loaded.vst3 != nullptr) {
          loaded.vst3->set_parameter(parameter.index, parameter.value);
        } else if (loaded.vst2 != nullptr) {
          loaded.vst2->set_parameter(parameter.index, parameter.value);
        }
      }
    }
  }

  // 参数表只在此时（流尚未启动）读取一次，之后由 GUI 读缓存。
  plugin_info.clear();
  for (const auto& loaded : plugins) {
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      for (const auto& parameter : loaded.vst3->parameters()) {
        plugin_info.push_back(
            PluginParameterInfo{loaded.slot, parameter.id, parameter.name, parameter.value});
      }
    } else if (loaded.vst2 != nullptr) {
      for (const auto& parameter : loaded.vst2->parameters()) {
        plugin_info.push_back(
            PluginParameterInfo{loaded.slot, parameter.id, parameter.name, parameter.value});
      }
    }
  }

  capture_period_frames = std::max(1u, capture.format().sampleRate / 100u);
  render_period_frames = std::max(1u, render.buffer_frames() / 2u);

  error = capture.start([this](const std::uint8_t* data, std::uint32_t frames,
                               const audio_io::AudioFormat&) {
    const std::size_t produced = capture_bridge.process(data, frames, capture_scratch.data(),
                                                        capture_scratch.size());
    if (produced == 0) {
      return;
    }
    const std::size_t written = ring.write(capture_scratch.data(), produced);
    if (written < produced) {
      overruns.fetch_add(1, std::memory_order_relaxed);
    }
    captured.fetch_add(written, std::memory_order_relaxed);
  });
  if (error.failed()) {
    state.store(EngineState::Failed);
    return error;
  }

  // 预灌：等水位接近目标再开闸，避免开流瞬间的一串欠载。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
  while (std::chrono::steady_clock::now() < deadline && !warmed_up) {
    if (static_cast<double>(ring.available_read()) >= target_fill_frames) {
      warmed_up = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  error = render.start([this](std::uint8_t* data, std::uint32_t frames,
                              const audio_io::AudioFormat&) {
    std::uint64_t fill = ring.available_read();

    // 未达目标水位前只输出静音：避免开流瞬间的欠载，也防止首块把缓冲抽干。
    if (!warmed_up) {
      warmed_up = static_cast<double>(fill) >= target_fill_frames;
      if (!warmed_up) {
        std::fill(render_scratch.begin(), render_scratch.begin() + frames, 0.0f);
        render_bridge.write(render_scratch.data(), frames, data);
        rendered.fetch_add(frames, std::memory_order_relaxed);
        fill_frames.store(static_cast<std::uint32_t>(fill), std::memory_order_relaxed);
        return;
      }
    }

    // 异常积压恢复：设备切换、插件初始化等一次性停顿会让水位永久偏高。
    // 超过目标 3 倍时直接丢弃多余输入，把延迟拉回正常范围（仅在异常时触发）。
    const auto max_fill = static_cast<std::uint64_t>(target_fill_frames * 3.0);
    if (fill > max_fill) {
      std::uint64_t drop = fill - static_cast<std::uint64_t>(target_fill_frames);
      while (drop > 0) {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(drop, render_in.size()));
        const std::size_t got = ring.read(render_in.data(), chunk);
        if (got == 0) {
          break;
        }
        drop -= got;
      }
      fill = ring.available_read();
    }

    // 时钟漂移：连续微调比率，由重采样吸收，不丢样本。
    const double step = drift.update(fill);
    converter.set_step(step);
    applied_ppm.store(drift.last_ppm(), std::memory_order_relaxed);

    // 按 FIFO 水位决定本块取多少输入，使 FIFO 稳定在插值所需的最小值。
    // 用小数累加器而不是直接 ceil：否则 480 帧的整块会把 ppm 级修正四舍五入掉，
    // 控制器会一直顶在限幅上而实际比率仍是 1.0。
    const double fifo = static_cast<double>(converter.fifo_size());
    const double want_in =
        std::max(0.0, (static_cast<double>(frames) + kConverterFifoFrames - fifo) * step);
    pull_accumulator += want_in;
    std::size_t pull_in = static_cast<std::size_t>(pull_accumulator);
    pull_accumulator -= static_cast<double>(pull_in);
    pull_in = std::min(pull_in, render_in.size());

    const std::size_t got = ring.read(render_in.data(), pull_in);
    if (got == 0 && fifo < kConverterFifoFrames) {
      underruns.fetch_add(1, std::memory_order_relaxed);
    }

    if (got > 0) {
      raw_tap.write(render_in.data(), got);
      noise_in.process(render_in.data(), static_cast<int>(got));
      if (!bypass.load(std::memory_order_relaxed)) {
        chain.process(render_in.data(), static_cast<int>(got));
      }
      processed_tap.write(render_in.data(), got);
      noise_out.process(render_in.data(), static_cast<int>(got));
      // 输出增益放在测量之后：底噪对比只反映降噪效果，不随监听音量变化。
      out_gain->process(render_in.data(), static_cast<int>(got));
      converter.feed(render_in.data(), got);
    }

    std::size_t produced = converter.pull(render_scratch.data(), frames);
    if (produced < frames) {
      std::fill(render_scratch.begin() + produced, render_scratch.begin() + frames, 0.0f);
    }

    render_bridge.write(render_scratch.data(), frames, data);
    rendered.fetch_add(frames, std::memory_order_relaxed);
    fill_frames.store(static_cast<std::uint32_t>(ring.available_read()),
                      std::memory_order_relaxed);
  });
  if (error.failed()) {
    capture.stop();
    state.store(EngineState::Failed);
    return error;
  }

  state.store(EngineState::Running);
  return {};
}

void AudioEngine::Impl::stop_pipeline() {
  std::lock_guard<std::recursive_mutex> lock(pipeline_mutex);
  state.store(EngineState::Stopped);
  render.stop();
  capture.stop();
  ring.reset();
}

void AudioEngine::Impl::start_watchdog() {
  watchdog_stop.store(false);
  watchdog = std::thread([this] {
    while (!watchdog_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      // 流意外停止（设备拔出、驱动重置）也当作需要恢复
      if (state.load() == EngineState::Running && (!capture.running() || !render.running())) {
        recovery_requested.store(true);
      }
      if (!recovery_requested.exchange(false)) {
        continue;
      }

      state.store(EngineState::Recovering);
      stop_pipeline();
      std::this_thread::sleep_for(std::chrono::milliseconds(200));

      const auto error = start_pipeline(options);
      state.store(error.ok() ? EngineState::Running : EngineState::Failed);
      if (error.ok()) {
        recoveries.fetch_add(1);
      }
    }
  });
}

void AudioEngine::Impl::stop_watchdog() {
  watchdog_stop.store(true);
  if (watchdog.joinable()) {
    watchdog.join();
  }
}

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() { stop(); }

platform::Error AudioEngine::start(const EngineOptions& options) {
  impl_->stop_watchdog();
  impl_->watcher.stop();
  impl_->stop_pipeline();

  impl_->options = options;
  impl_->active_input_id = options.input_endpoint_id;
  impl_->active_output_id = options.output_endpoint_id;

  auto error = impl_->start_pipeline(options);
  if (error.failed()) {
    return error;
  }

  // 只关心本链路正在使用的端点；默认设备变化不影响已打开的流。
  impl_->watcher.start([impl = impl_.get()](audio_io::DeviceWatcher::Event, const std::wstring& id) {
    if (id == impl->active_input_id || id == impl->active_output_id) {
      impl->recovery_requested.store(true);
    }
  });
  impl_->start_watchdog();
  return {};
}

void AudioEngine::stop() {
  impl_->stop_watchdog();
  impl_->watcher.stop();
  impl_->stop_pipeline();
}

void AudioEngine::set_input_gain_db(float db) {
  if (impl_->gain != nullptr) {
    impl_->gain->set_gain_db(db);
  }
}

void AudioEngine::set_output_gain_db(float db) {
  if (impl_->out_gain != nullptr) {
    impl_->out_gain->set_gain_db(db);
  }
}

void AudioEngine::set_denoise_amount(float amount) {
  if (impl_->denoise != nullptr) {
    impl_->denoise->set_amount(amount);
  }
}

void AudioEngine::set_effects(bool clarity, bool body, bool leveler, bool deesser) {
  if (impl_->clarity != nullptr) {
    impl_->clarity->set_enabled(clarity);
  }
  if (impl_->body != nullptr) {
    impl_->body->set_enabled(body);
  }
  if (impl_->leveler != nullptr) {
    impl_->leveler->set_enabled(leveler);
  }
  if (impl_->deesser != nullptr) {
    impl_->deesser->set_enabled(deesser);
  }
}

void AudioEngine::set_bypass(bool bypass) { impl_->bypass.store(bypass); }

std::vector<PluginParameterInfo> AudioEngine::plugin_parameters() const {
  return impl_->plugin_info;
}

void AudioEngine::set_plugin_parameter(int slot, int id, float value) {
  for (auto& info : impl_->plugin_info) {
    if (info.slot == slot && info.id == id) {
      info.value = value;
      break;
    }
  }

  for (const auto& loaded : impl_->plugins) {
    if (loaded.slot != slot) {
      continue;
    }
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      loaded.vst3->set_parameter(id, value);
    } else if (loaded.vst2 != nullptr) {
      loaded.vst2->set_parameter(id, value);
    }
    return;
  }
}

std::vector<PluginParameterChange> AudioEngine::take_plugin_parameter_changes() {
  std::vector<PluginParameterChange> result;

  auto record = [this, &result](int slot, int id, float value) {
    for (auto& info : impl_->plugin_info) {
      if (info.slot == slot && info.id == id) {
        info.value = value;
        break;
      }
    }
    PluginParameterChange change;
    change.slot = slot;
    change.id = id;
    change.value = value;
    result.push_back(change);
  };

  for (const auto& loaded : impl_->plugins) {
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      for (const auto& change : loaded.vst3->take_parameter_changes()) {
        record(loaded.slot, change.id, change.value);
      }
    } else if (loaded.vst2 != nullptr) {
      for (const auto& change : loaded.vst2->take_parameter_changes()) {
        record(loaded.slot, change.id, change.value);
      }
    }
  }
  return result;
}

std::vector<std::wstring> AudioEngine::plugin_slot_errors() const {
  return impl_->plugin_errors;
}

std::wstring AudioEngine::builtin_denoise_error() const {
  return impl_->builtin_error;
}

bool AudioEngine::plugin_has_editor(int slot) const {
  for (const auto& loaded : impl_->plugins) {
    if (loaded.slot != slot) {
      continue;
    }
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      return loaded.vst3->has_editor();
    }
    return loaded.vst2 != nullptr && loaded.vst2->has_editor();
  }
  return false;
}

bool AudioEngine::open_plugin_editor(int slot, void* parent, int& width, int& height) {
  for (const auto& loaded : impl_->plugins) {
    if (loaded.slot != slot) {
      continue;
    }
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      return loaded.vst3->open_editor(parent, width, height);
    }
    return loaded.vst2 != nullptr && loaded.vst2->open_editor(parent, width, height);
  }
  return false;
}

void AudioEngine::close_plugin_editor(int slot) {
  for (const auto& loaded : impl_->plugins) {
    if (loaded.slot != slot) {
      continue;
    }
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      loaded.vst3->close_editor();
    } else if (loaded.vst2 != nullptr) {
      loaded.vst2->close_editor();
    }
    return;
  }
}

void AudioEngine::plugin_editor_idle(int slot) {
  for (const auto& loaded : impl_->plugins) {
    if (loaded.slot != slot) {
      continue;
    }
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      loaded.vst3->editor_idle();
    } else if (loaded.vst2 != nullptr) {
      loaded.vst2->editor_idle();
    }
    return;
  }
}

bool AudioEngine::plugin_editor_size(int slot, int& width, int& height) const {
  for (const auto& loaded : impl_->plugins) {
    if (loaded.slot != slot) {
      continue;
    }
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      return loaded.vst3->editor_size(width, height);
    }
    return loaded.vst2 != nullptr && loaded.vst2->editor_size(width, height);
  }
  return false;
}

EngineState AudioEngine::state() const noexcept { return impl_->state.load(); }

const TapBuffer& AudioEngine::raw_tap() const noexcept { return impl_->raw_tap; }

const TapBuffer& AudioEngine::processed_tap() const noexcept { return impl_->processed_tap; }

AudioEngine::Stats AudioEngine::stats() const noexcept {
  Stats stats;
  stats.captured_frames = impl_->captured.load(std::memory_order_relaxed);
  stats.rendered_frames = impl_->rendered.load(std::memory_order_relaxed);
  stats.overruns = impl_->overruns.load(std::memory_order_relaxed);
  stats.underruns = impl_->underruns.load(std::memory_order_relaxed);
  stats.fill_frames = impl_->fill_frames.load(std::memory_order_relaxed);
  stats.fill_ms = stats.fill_frames * 1000.0 / impl_->engine_rate;
  stats.applied_ppm = impl_->applied_ppm.load(std::memory_order_relaxed);
  stats.latency_ms =
      (stats.fill_frames + impl_->capture_period_frames + impl_->render_period_frames) * 1000.0 /
      impl_->engine_rate;

  stats.deepfilter_active = impl_->deepfilter_active;
  if (impl_->deepfilter_active && impl_->deepfilter != nullptr) {
    stats.deepfilter_latency_ms =
        impl_->deepfilter->latency_frames() * 1000.0 / impl_->engine_rate;
    stats.latency_ms += stats.deepfilter_latency_ms;
  }
  for (const auto& loaded : impl_->plugins) {
    ++stats.plugins_loaded;
    double latency_ms = 0.0;
    if (loaded.vst3 && loaded.vst3 != nullptr) {
      latency_ms = loaded.vst3->latency_frames() * 1000.0 / impl_->engine_rate;
    } else if (loaded.vst2 != nullptr) {
      latency_ms = loaded.vst2->latency_frames() * 1000.0 / impl_->engine_rate;
    }
    stats.plugin_latency_ms += latency_ms;
    stats.latency_ms += latency_ms;
  }
  stats.recoveries = impl_->recoveries.load(std::memory_order_relaxed);
  stats.noise_floor_in_db = impl_->noise_in.floor_db();
  stats.noise_floor_out_db = impl_->noise_out.floor_db();
  stats.snr_gain_db = stats.noise_floor_in_db - stats.noise_floor_out_db;

  const auto capture_stats = impl_->capture.stats();
  const auto render_stats = impl_->render.stats();
  stats.capture_buffers = capture_stats.buffers;
  stats.render_buffers = render_stats.buffers;
  stats.capture_timeouts = capture_stats.timeouts;
  stats.render_timeouts = render_stats.timeouts;
  return stats;
}

}  // namespace ms::engine
