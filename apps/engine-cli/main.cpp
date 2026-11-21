#include <windows.h>

#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_io/capture_stream.h"
#include "audio_io/device_enumerator.h"
#include "audio_io/endpoint_meter.h"
#include "audio_io/render_stream.h"
#include "dsp_core/format_bridge.h"
#include "engine/audio_engine.h"
#include "config/settings.h"
#include "platform/com.h"
#include "platform/default_endpoint.h"
#include "platform/utf8.h"
#include "presets/preset_library.h"
#ifdef MS_ENABLE_VST3
#include "vst3_host/vst3_slot.h"
#endif

namespace {

using ms::audio_io::AudioFormat;
using ms::audio_io::CaptureStream;
using ms::audio_io::EndpointInfo;
using ms::audio_io::Flow;

double amplitude_to_db(double amplitude) {
  if (amplitude <= 1e-7) {
    return -120.0;
  }
  return 20.0 * std::log10(amplitude);
}

// 解析 --vst2-param / --vst3-param 的 <键>=<0..1>，例如 0=0.6 表示把参数设为 60%。
bool parse_plugin_param(const std::wstring& text, ms::engine::PluginParameter& parameter) {
  const auto separator = text.find(L'=');
  if (separator == std::wstring::npos) {
    return false;
  }
  try {
    parameter.index = std::stoi(text.substr(0, separator));
    parameter.value = static_cast<float>(std::stod(text.substr(separator + 1)));
  } catch (const std::exception&) {
    return false;
  }
  return parameter.index >= 0;
}

// 把 CLI 的 VST2/VST3 路径整理成插件槽列表：VST2 在前，VST3 在后。
void apply_plugins(ms::engine::EngineOptions& options, const std::wstring& vst2_path,
                   const std::vector<ms::engine::PluginParameter>& vst2_params,
                   const std::wstring& vst3_path,
                   const std::vector<ms::engine::PluginParameter>& vst3_params) {
  if (!vst2_path.empty()) {
    options.plugins.push_back(ms::engine::PluginSlot{false, vst2_path, vst2_params});
  }
  if (!vst3_path.empty()) {
    options.plugins.push_back(ms::engine::PluginSlot{true, vst3_path, vst3_params});
  }
}

void print_error(const ms::platform::Error& error) {
  std::cout << "错误：" << ms::platform::to_utf8(error.message) << "（"
            << ms::platform::to_utf8(ms::platform::to_string(error.code)) << "，0x" << std::hex
            << error.hresult << std::dec << "）\n";
}

void print_usage() {
  std::cout << "用法：\n"
            << "  engine-cli list                         列出采集与渲染端点\n"
            << "  engine-cli capture --device <序号> [--seconds <秒>] [--loopback]\n"
            << "                                          采集并实时显示电平（--loopback 抓渲染端点）\n"
            << "  engine-cli passthrough --input <序号> --output <序号> [--seconds <秒>] [--gain <dB>] [--denoise 0..1] [--preset <场景>] [--effects <列表>] [--vst2 <DLL路径>] [--vst3 <路径>]\n"
            << "                                          场景：meeting / noisy / stream / raw\n"
            << "                                          效果：clarity,body,leveler（逗号分隔）\n"
            << "                                          --vst2 挂载 VST2 插件（如 DeepFilterNet）\n"
            << "                                          --vst2-param <序号=0..1> 调整插件参数（可重复）\n"
            << "                                          --vst3 挂载 VST3 插件（.vst3 包或 DLL）\n"
            << "                                          --vst3-param <参数ID=0..1> 调整插件参数（可重复）\n"
            << "                                          麦克风直通到指定渲染端点\n"
            << "  engine-cli tone --output <序号> [--seconds <秒>]\n"
            << "                                          向渲染端点输出 440 Hz 测试音\n"
            << "  engine-cli monitor --device <序号> [--seconds <秒>]\n"
            << "                                          读取端点峰值表（不经采集流）\n"
            << "  engine-cli ab --input <序号> --output <序号> [--seconds <秒>] [--device <播放序号>]\n"
            << "                                          采集后循环回放 原始/处理 各 2 秒\n"
            << "  engine-cli run --input <序号> --output <序号> [--preset <场景>] [--seconds <秒>]\n"
            << "                                          一键就绪：启动链路并把虚拟麦克风设为默认\n"
            << "  engine-cli vst3-info --vst3 <路径>\n"
            << "                                          列出 VST3 插件的参数 ID / 名称 / 当前值\n";
}

void print_format(const AudioFormat& format) {
  std::cout << format.sampleRate << " Hz · " << format.channels << " ch · " << format.bitsPerSample
            << "-bit " << (format.isFloat ? "float" : "int");
}

// MME（waveIn）枚举：验证老一代应用也能看见同一个虚拟麦克风。
void print_mme_capture_devices() {
  const UINT count = ::waveInGetNumDevs();
  std::cout << "MME 采集设备（" << count << " 个）\n";
  for (UINT i = 0; i < count; ++i) {
    WAVEINCAPSW caps{};
    if (::waveInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
      std::cout << "[" << i << "] " << ms::platform::to_utf8(caps.szPname) << " · " << caps.wChannels
                << " ch · " << caps.dwFormats << "\n";
    }
  }
  std::cout << "\n";
}

void print_devices(Flow flow, const char* title) {
  const auto list = ms::audio_io::enumerate_devices(flow);
  if (!list.error.ok()) {
    print_error(list.error);
    return;
  }

  std::cout << title << "（" << list.items.size() << " 个）\n";
  for (const auto& device : list.items) {
    std::cout << "[" << device.index << "] " << ms::platform::to_utf8(device.friendlyName);
    if (device.isDefaultCommunications) {
      std::cout << "   *默认通信";
    } else if (device.isDefaultConsole) {
      std::cout << "   *默认";
    }
    std::cout << "\n    " << ms::platform::to_utf8(device.id) << "\n    ";
    print_format(device.format);
    std::cout << " · 周期 " << device.defaultPeriodFrames << " 帧（最小 " << device.minPeriodFrames
              << "）";
    if (device.muted) {
      std::cout << " · 已静音";
    }
    std::cout << " · 音量 " << static_cast<int>(device.volumeScalar * 100.0f + 0.5f) << "%\n";
  }
  std::cout << "\n";
}

// 仅用于命令行演示的统计器：回调线程会加锁，正式引擎中绝不这样做。
class LevelAccumulator {
 public:
  void add(const std::uint8_t* data, std::uint32_t frames, const AudioFormat& format) {
    double local_sum = 0.0;
    double local_peak = 0.0;
    std::uint64_t count = 0;

    if (format.isFloat && format.bitsPerSample == 32) {
      const auto* samples = reinterpret_cast<const float*>(data);
      count = static_cast<std::uint64_t>(frames) * format.channels;
      for (std::uint64_t i = 0; i < count; ++i) {
        const double value = samples[i];
        local_sum += value * value;
        local_peak = std::max(local_peak, std::fabs(value));
      }
    } else if (!format.isFloat && format.bitsPerSample == 16) {
      const auto* samples = reinterpret_cast<const std::int16_t*>(data);
      count = static_cast<std::uint64_t>(frames) * format.channels;
      for (std::uint64_t i = 0; i < count; ++i) {
        const double value = samples[i] / 32768.0;
        local_sum += value * value;
        local_peak = std::max(local_peak, std::fabs(value));
      }
    } else {
      return;  // 其他格式暂不统计
    }

    std::lock_guard<std::mutex> lock(mutex_);
    sum_squares_ += local_sum;
    samples_ += count;
    peak_ = std::max(peak_, local_peak);
  }

  bool snapshot(double& rms_db, double& peak_db) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_ == 0) {
      rms_db = -120.0;
      peak_db = -120.0;
      return false;
    }
    rms_db = amplitude_to_db(std::sqrt(sum_squares_ / static_cast<double>(samples_)));
    peak_db = amplitude_to_db(peak_);
    sum_squares_ = 0.0;
    samples_ = 0;
    peak_ = 0.0;
    return true;
  }

 private:
  std::mutex mutex_;
  double sum_squares_ = 0.0;
  std::uint64_t samples_ = 0;
  double peak_ = 0.0;
};

int run_capture(const std::vector<std::wstring>& args) {
  int device_index = -1;
  double seconds = 5.0;
  bool loopback = false;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--device" && i + 1 < args.size()) {
      device_index = std::stoi(args[++i]);
    } else if (args[i] == L"--seconds" && i + 1 < args.size()) {
      seconds = std::stod(args[++i]);
    } else if (args[i] == L"--loopback") {
      loopback = true;
    }
  }

  const auto list = ms::audio_io::enumerate_devices(loopback ? Flow::Render : Flow::Capture);
  if (!list.error.ok()) {
    print_error(list.error);
    return 2;
  }
  if (list.items.empty()) {
    std::cout << "没有可用的采集端点。\n";
    return 2;
  }
  if (device_index < 0 || static_cast<std::size_t>(device_index) >= list.items.size()) {
    std::cout << "设备序号无效。可用设备：\n";
    for (const auto& device : list.items) {
      std::cout << "  [" << device.index << "] " << ms::platform::to_utf8(device.friendlyName) << "\n";
    }
    return 2;
  }

  const EndpointInfo& device = list.items[static_cast<std::size_t>(device_index)];
  std::cout << (loopback ? "回环采集：" : "采集：") << ms::platform::to_utf8(device.friendlyName)
            << "\n    ";
  print_format(device.format);
  std::cout << "\n\n";

  CaptureStream stream;
  auto error = stream.open(device.id, loopback);
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  LevelAccumulator accumulator;
  std::cout << "流格式：";
  print_format(stream.format());
  std::cout << "\n";
  error = stream.start([&accumulator](const std::uint8_t* data, std::uint32_t frames,
                                      const AudioFormat& format) {
    accumulator.add(data, frames, format);
  });
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  const auto start_time = std::chrono::steady_clock::now();
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();

    double rms_db = -120.0;
    double peak_db = -120.0;
    const bool has_data = accumulator.snapshot(rms_db, peak_db);
    const auto stats = stream.stats();

    std::cout << "[" << std::setw(5) << std::fixed << std::setprecision(1) << elapsed << "s] "
              << "RMS " << std::setw(7) << (has_data ? rms_db : -120.0) << " dBFS"
              << "   峰值 " << std::setw(7) << (has_data ? peak_db : -120.0) << " dBFS"
              << "   缓冲 " << stats.buffers << "   静音 " << stats.silentBuffers << "   断流 "
              << stats.discontinuities << std::endl;

    if (elapsed >= seconds) {
      break;
    }
  }

  stream.stop();

  const auto stats = stream.stats();
  const auto& format = stream.format();
  const double period_ms =
      format.sampleRate != 0 ? 1000.0 * device.defaultPeriodFrames / format.sampleRate : 0.0;

  std::cout << "\n已停止："
            << static_cast<double>(stats.frames) / (format.sampleRate != 0 ? format.sampleRate : 1)
            << " 秒音频，" << stats.buffers << " 个缓冲；静音 " << stats.silentBuffers << "，断流 "
            << stats.discontinuities << "，等待超时 " << stats.timeouts << "\n"
            << "端点周期约 " << std::fixed << std::setprecision(1) << period_ms << " ms\n";
  return 0;
}

int run_passthrough(const std::vector<std::wstring>& args) {
  int input_index = -1;
  int output_index = -1;
  double seconds = 10.0;
  double gain_db = 0.0;
  double denoise = 0.0;
  bool denoise_set = false;
  std::wstring preset_id;
  std::wstring effects;
  std::wstring vst2_path;
  std::vector<ms::engine::PluginParameter> vst2_params;
  std::wstring vst3_path;
  std::vector<ms::engine::PluginParameter> vst3_params;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--input" && i + 1 < args.size()) {
      input_index = std::stoi(args[++i]);
    } else if (args[i] == L"--output" && i + 1 < args.size()) {
      output_index = std::stoi(args[++i]);
    } else if (args[i] == L"--seconds" && i + 1 < args.size()) {
      seconds = std::stod(args[++i]);
    } else if (args[i] == L"--gain" && i + 1 < args.size()) {
      gain_db = std::stod(args[++i]);
    } else if (args[i] == L"--denoise" && i + 1 < args.size()) {
      denoise = std::stod(args[++i]);
      denoise_set = true;
    } else if (args[i] == L"--vst2" && i + 1 < args.size()) {
      vst2_path = args[++i];
    } else if (args[i] == L"--vst2-param" && i + 1 < args.size()) {
      ms::engine::PluginParameter parameter;
      if (parse_plugin_param(args[++i], parameter)) {
        vst2_params.push_back(parameter);
      } else {
        std::cout << "忽略无效的 VST2 参数（应为 序号=值）：" << ms::platform::to_utf8(args[i])
                  << "\n";
      }
    } else if (args[i] == L"--vst3" && i + 1 < args.size()) {
      vst3_path = args[++i];
    } else if (args[i] == L"--vst3-param" && i + 1 < args.size()) {
      ms::engine::PluginParameter parameter;
      if (parse_plugin_param(args[++i], parameter)) {
        vst3_params.push_back(parameter);
      } else {
        std::cout << "忽略无效的 VST3 参数（应为 参数ID=值）：" << ms::platform::to_utf8(args[i])
                  << "\n";
      }
    } else if (args[i] == L"--preset" && i + 1 < args.size()) {
      preset_id = args[++i];
    } else if (args[i] == L"--effects" && i + 1 < args.size()) {
      effects = args[++i];
    }
  }

  const ms::presets::PresetTemplate* preset = nullptr;
  if (!preset_id.empty()) {
    preset = ms::presets::PresetLibrary::find(ms::platform::to_utf8(preset_id));
    if (preset == nullptr) {
      std::cout << "未知场景。可用：";
      for (const auto& item : ms::presets::PresetLibrary::all()) {
        std::cout << item.id << "  ";
      }
      std::cout << "\n";
      return 2;
    }
    // 显式参数优先于场景默认值；场景只决定降噪强度，不影响输入增益。
    if (!denoise_set) {
      denoise = preset->denoise_amount;
    }
  }

  const auto inputs = ms::audio_io::enumerate_devices(Flow::Capture);
  const auto outputs = ms::audio_io::enumerate_devices(Flow::Render);
  if (!inputs.error.ok()) {
    print_error(inputs.error);
    return 2;
  }
  if (!outputs.error.ok()) {
    print_error(outputs.error);
    return 2;
  }
  if (input_index < 0 || static_cast<std::size_t>(input_index) >= inputs.items.size()) {
    std::cout << "--input 无效。\n";
    return 2;
  }
  if (output_index < 0 || static_cast<std::size_t>(output_index) >= outputs.items.size()) {
    std::cout << "--output 无效。\n";
    return 2;
  }

  const auto& input = inputs.items[static_cast<std::size_t>(input_index)];
  const auto& output = outputs.items[static_cast<std::size_t>(output_index)];

  std::cout << "输入：" << ms::platform::to_utf8(input.friendlyName) << "\n输出："
            << ms::platform::to_utf8(output.friendlyName);
  if (preset != nullptr) {
    std::cout << "\n场景：" << preset->name << "（" << preset->scenario << "）";
  }
  std::cout << "\n增益：" << std::showpos << std::fixed << std::setprecision(1) << gain_db
            << std::noshowpos << " dB\n降噪：" << static_cast<int>(denoise * 100) << "%";
  const std::string effect_list = ms::platform::to_utf8(effects);
  if (!effect_list.empty()) {
    std::cout << "\n效果：" << effect_list;
  }
  if (!vst2_path.empty()) {
    std::cout << "\nVST2：" << ms::platform::to_utf8(vst2_path);
  }
  if (!vst3_path.empty()) {
    std::cout << "\nVST3：" << ms::platform::to_utf8(vst3_path);
  }
  std::cout << "\n提示：建议戴耳机，外放可能产生啸叫。\n\n";

  ms::engine::EngineOptions options;
  options.input_endpoint_id = input.id;
  options.output_endpoint_id = output.id;
  options.input_gain_db = static_cast<float>(gain_db);
  options.denoise_amount = static_cast<float>(denoise);
  options.clarity = effect_list.find("clarity") != std::string::npos;
  options.body = effect_list.find("body") != std::string::npos;
  options.leveler = effect_list.find("leveler") != std::string::npos;
  options.deesser = effect_list.find("deesser") != std::string::npos;
  apply_plugins(options, vst2_path, vst2_params, vst3_path, vst3_params);

  ms::engine::AudioEngine engine;
  auto error = engine.start(options);
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  const auto initial_stats = engine.stats();
  if (initial_stats.deepfilter_active) {
    std::cout << "内置降噪：DeepFilterNet，延迟 " << std::fixed << std::setprecision(1)
              << initial_stats.deepfilter_latency_ms << " ms\n";
  } else {
    std::cout << "内置降噪：DeepFilterNet 加载失败（"
              << ms::platform::to_utf8(engine.builtin_denoise_error()) << "）\n";
  }
  if (initial_stats.plugins_loaded > 0) {
    std::cout << "插件已加载 " << initial_stats.plugins_loaded << " 个，总延迟 " << std::fixed
              << std::setprecision(1) << initial_stats.plugin_latency_ms << " ms\n";
  }

  const auto start_time = std::chrono::steady_clock::now();
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    const auto stats = engine.stats();

    std::cout << "[" << std::setw(5) << std::fixed << std::setprecision(1) << elapsed << "s] 水位 "
              << std::setw(5) << stats.fill_ms << " ms   引擎延迟 " << std::setw(5)
              << stats.latency_ms << " ms   修正 " << std::showpos << std::setprecision(0)
              << stats.applied_ppm << std::noshowpos << " ppm   溢出 " << stats.overruns
              << "   欠载 " << stats.underruns << "   采/渲 " << stats.capture_buffers << "/"
              << stats.render_buffers << "   超时 " << stats.capture_timeouts << "/"
              << stats.render_timeouts << "   恢复 " << stats.recoveries << "   噪声底 "
              << std::setprecision(1) << stats.noise_floor_in_db << " → "
              << stats.noise_floor_out_db << " dB" << std::endl;

    if (elapsed >= seconds) {
      break;
    }
  }

  engine.stop();

  const auto stats = engine.stats();
  const double captured_seconds = stats.captured_frames / 48000.0;
  const double rendered_seconds = stats.rendered_frames / 48000.0;
  std::cout << "\n已停止：采集 " << std::fixed << std::setprecision(2) << captured_seconds
            << " 秒，渲染 " << rendered_seconds << " 秒；溢出 " << stats.overruns << "，欠载 "
            << stats.underruns << "\n";
  return 0;
}

int run_tone(const std::vector<std::wstring>& args) {
  int output_index = -1;
  double seconds = 2.0;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--output" && i + 1 < args.size()) {
      output_index = std::stoi(args[++i]);
    } else if (args[i] == L"--seconds" && i + 1 < args.size()) {
      seconds = std::stod(args[++i]);
    }
  }

  const auto outputs = ms::audio_io::enumerate_devices(Flow::Render);
  if (!outputs.error.ok()) {
    print_error(outputs.error);
    return 2;
  }
  if (output_index < 0 || static_cast<std::size_t>(output_index) >= outputs.items.size()) {
    std::cout << "--output 无效。可用设备：\n";
    for (const auto& device : outputs.items) {
      std::cout << "  [" << device.index << "] " << ms::platform::to_utf8(device.friendlyName) << "\n";
    }
    return 2;
  }

  const auto& output = outputs.items[static_cast<std::size_t>(output_index)];
  std::cout << "测试音输出：" << ms::platform::to_utf8(output.friendlyName) << "\n";

  ms::audio_io::RenderStream stream;
  auto error = stream.open(output.id);
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  ms::dsp_core::RenderBridge bridge;
  bridge.prepare(stream.format(), 48000);
  if (!bridge.supported()) {
    std::cout << "该端点格式不受支持。\n";
    return 3;
  }

  std::vector<float> mono(16384, 0.0f);
  std::vector<float> scratch(16384, 0.0f);
  double phase = 0.0;
  const double increment = 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;

  error = stream.start([&](std::uint8_t* data, std::uint32_t frames,
                           const ms::audio_io::AudioFormat&) {
    const std::uint32_t input_frames = std::min<std::uint32_t>(frames, 16384);
    for (std::uint32_t i = 0; i < input_frames; ++i) {
      mono[i] = static_cast<float>(0.2 * std::sin(phase));
      phase += increment;
      if (phase > 6.283185307179586) {
        phase -= 6.283185307179586;
      }
    }

    std::uint32_t produced = bridge.render(mono.data(), input_frames, scratch.data(), frames);
    produced = std::min(produced, frames);
    if (produced < frames) {
      std::fill(scratch.begin() + produced, scratch.begin() + frames, 0.0f);
    }
    bridge.write(scratch.data(), frames, data);
  });
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
  stream.stop();
  std::cout << "已结束。\n";
  return 0;
}

int run_monitor(const std::vector<std::wstring>& args) {
  int device_index = -1;
  double seconds = 5.0;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--device" && i + 1 < args.size()) {
      device_index = std::stoi(args[++i]);
    } else if (args[i] == L"--seconds" && i + 1 < args.size()) {
      seconds = std::stod(args[++i]);
    }
  }

  const auto devices = ms::audio_io::enumerate_devices(Flow::Capture);
  if (!devices.error.ok()) {
    print_error(devices.error);
    return 2;
  }
  if (device_index < 0 || static_cast<std::size_t>(device_index) >= devices.items.size()) {
    std::cout << "--device 无效。\n";
    return 2;
  }

  const auto& device = devices.items[static_cast<std::size_t>(device_index)];
  std::cout << "监听：" << ms::platform::to_utf8(device.friendlyName) << "\n";

  ms::audio_io::EndpointMeter meter;
  auto error = meter.open(device.id);
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  float max_peak = 0.0f;
  const auto start_time = std::chrono::steady_clock::now();
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    const float peak = meter.peak();
    max_peak = std::max(max_peak, peak);

    std::cout << "[" << std::setw(5) << std::fixed << std::setprecision(1) << elapsed << "s] 峰值 "
              << std::setw(7) << amplitude_to_db(peak) << " dBFS" << std::endl;

    if (elapsed >= seconds) {
      break;
    }
  }

  std::cout << "\n最大峰值 " << std::fixed << std::setprecision(1) << amplitude_to_db(max_peak)
            << " dBFS" << (max_peak <= 0.0f ? "（端点无信号）" : "") << "\n";
  return 0;
}

// 采集一段，然后通过默认播放设备交替回放"原始 / 处理"，用于验证效果是否可闻。
int run_ab(const std::vector<std::wstring>& args) {
  int input_index = -1;
  int output_index = -1;
  int play_index = -1;
  double seconds = 8.0;
  double denoise_amount = 0.8;
  std::wstring preset_id;
  std::wstring vst2_path;
  std::vector<ms::engine::PluginParameter> vst2_params;
  std::wstring vst3_path;
  std::vector<ms::engine::PluginParameter> vst3_params;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--input" && i + 1 < args.size()) {
      input_index = std::stoi(args[++i]);
    } else if (args[i] == L"--output" && i + 1 < args.size()) {
      output_index = std::stoi(args[++i]);
    } else if (args[i] == L"--device" && i + 1 < args.size()) {
      play_index = std::stoi(args[++i]);
    } else if (args[i] == L"--seconds" && i + 1 < args.size()) {
      seconds = std::stod(args[++i]);
    } else if (args[i] == L"--denoise" && i + 1 < args.size()) {
      denoise_amount = std::stod(args[++i]);
    } else if (args[i] == L"--preset" && i + 1 < args.size()) {
      preset_id = args[++i];
    } else if (args[i] == L"--vst2" && i + 1 < args.size()) {
      vst2_path = args[++i];
    } else if (args[i] == L"--vst2-param" && i + 1 < args.size()) {
      ms::engine::PluginParameter parameter;
      if (parse_plugin_param(args[++i], parameter)) {
        vst2_params.push_back(parameter);
      } else {
        std::cout << "忽略无效的 VST2 参数（应为 序号=值）：" << ms::platform::to_utf8(args[i])
                  << "\n";
      }
    } else if (args[i] == L"--vst3" && i + 1 < args.size()) {
      vst3_path = args[++i];
    } else if (args[i] == L"--vst3-param" && i + 1 < args.size()) {
      ms::engine::PluginParameter parameter;
      if (parse_plugin_param(args[++i], parameter)) {
        vst3_params.push_back(parameter);
      } else {
        std::cout << "忽略无效的 VST3 参数（应为 参数ID=值）：" << ms::platform::to_utf8(args[i])
                  << "\n";
      }
    }
  }

  if (!preset_id.empty()) {
    const auto* preset = ms::presets::PresetLibrary::find(ms::platform::to_utf8(preset_id));
    if (preset == nullptr) {
      std::cout << "未知场景。可用：";
      for (const auto& item : ms::presets::PresetLibrary::all()) {
        std::cout << item.id << "  ";
      }
      std::cout << "\n";
      return 2;
    }
    denoise_amount = preset->denoise_amount;
  }

  const auto inputs = ms::audio_io::enumerate_devices(Flow::Capture);
  const auto outputs = ms::audio_io::enumerate_devices(Flow::Render);
  if (!inputs.error.ok() || !outputs.error.ok()) {
    std::cout << "枚举设备失败。\n";
    return 2;
  }
  if (input_index < 0 || static_cast<std::size_t>(input_index) >= inputs.items.size() ||
      output_index < 0 || static_cast<std::size_t>(output_index) >= outputs.items.size()) {
    std::cout << "--input / --output 无效。\n";
    return 2;
  }

  if (play_index < 0) {
    for (const auto& device : outputs.items) {
      if (device.isDefaultConsole) {
        play_index = static_cast<int>(device.index);
        break;
      }
    }
  }
  if (play_index < 0 || static_cast<std::size_t>(play_index) >= outputs.items.size()) {
    std::cout << "找不到可用于回放的扬声器，请用 --device 指定。\n";
    return 2;
  }

  const auto& input = inputs.items[static_cast<std::size_t>(input_index)];
  const auto& sink = outputs.items[static_cast<std::size_t>(output_index)];
  const auto& playback = outputs.items[static_cast<std::size_t>(play_index)];

  std::cout << "采集：" << ms::platform::to_utf8(input.friendlyName) << " → 虚拟输出："
            << ms::platform::to_utf8(sink.friendlyName) << "\n回放："
            << ms::platform::to_utf8(playback.friendlyName) << "\n请正常说话 "
            << static_cast<int>(seconds) << " 秒...\n";

  ms::engine::EngineOptions options;
  options.input_endpoint_id = input.id;
  options.output_endpoint_id = sink.id;
  options.denoise_amount = static_cast<float>(denoise_amount);
  apply_plugins(options, vst2_path, vst2_params, vst3_path, vst3_params);

  ms::engine::AudioEngine engine;
  auto error = engine.start(options);
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
  const auto stats = engine.stats();
  engine.stop();

  std::cout << "\n噪声底：处理前 " << std::fixed << std::setprecision(1) << stats.noise_floor_in_db
            << " dBFS → 处理后 " << stats.noise_floor_out_db << " dBFS（差 "
            << stats.snr_gain_db << " dB）\n";

  // 取最近 2 秒，拼接成 [原始, 处理, 原始, 处理]
  constexpr std::size_t kSegment = 48000 * 2;
  std::vector<float> raw(kSegment, 0.0f);
  std::vector<float> processed(kSegment, 0.0f);
  // 快照在读的过程中可能被渲染线程覆盖（返回 false）。导出是诊断用途，
  // 重试几次拿一份一致快照；仍失败则保留最后一次结果继续。
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (engine.raw_tap().read_latest(raw.data(), kSegment)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (engine.processed_tap().read_latest(processed.data(), kSegment)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  std::vector<float> sequence;
  sequence.reserve(kSegment * 4);
  for (int round = 0; round < 2; ++round) {
    sequence.insert(sequence.end(), raw.begin(), raw.end());
    sequence.insert(sequence.end(), processed.begin(), processed.end());
  }

  ms::audio_io::RenderStream stream;
  error = stream.open(playback.id);
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  ms::dsp_core::RenderBridge bridge;
  bridge.prepare(stream.format(), 48000);
  if (!bridge.supported()) {
    std::cout << "回放端点格式不受支持。\n";
    return 3;
  }

  std::vector<float> mono(16384, 0.0f);
  std::vector<float> scratch(16384, 0.0f);
  std::size_t cursor = 0;

  error = stream.start([&](std::uint8_t* data, std::uint32_t frames,
                           const ms::audio_io::AudioFormat&) {
    const std::uint32_t count = std::min<std::uint32_t>(frames, 16384);
    for (std::uint32_t i = 0; i < count; ++i) {
      mono[i] = cursor < sequence.size() ? sequence[cursor] : 0.0f;
      ++cursor;
    }
    std::uint32_t produced = bridge.render(mono.data(), count, scratch.data(), frames);
    produced = std::min(produced, frames);
    if (produced < frames) {
      std::fill(scratch.begin() + produced, scratch.begin() + frames, 0.0f);
    }
    bridge.write(scratch.data(), frames, data);
  });
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  std::cout << "回放中：原始 2 秒 → 处理 2 秒 → 原始 2 秒 → 处理 2 秒\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(8600));
  stream.stop();
  std::cout << "完成。\n";
  return 0;
}

// 设备解析：序号优先 → 设置里的 id → 设置里的名字。
int resolve_endpoint(const std::vector<ms::audio_io::EndpointInfo>& items, int index,
                     const std::wstring& id, const std::wstring& name) {
  if (index >= 0 && static_cast<std::size_t>(index) < items.size()) {
    return index;
  }
  if (!id.empty()) {
    for (const auto& item : items) {
      if (item.id == id) {
        return static_cast<int>(item.index);
      }
    }
  }
  if (!name.empty()) {
    for (const auto& item : items) {
      if (item.friendlyName == name) {
        return static_cast<int>(item.index);
      }
    }
  }
  return -1;
}

std::string effects_from_settings(const ms::config::Settings& settings) {
  std::string list;
  if (settings.clarity) list += "clarity,";
  if (settings.body) list += "body,";
  if (settings.leveler) list += "leveler,";
  if (settings.deesser) list += "deesser,";
  return list;
}

// 一键运行：优先使用保存的设置，命令行参数可覆盖；退出时还原默认麦克风。
int run_oneshot(const std::vector<std::wstring>& args) {
  int input_index = -1;
  int output_index = -1;
  int mic_index = -1;
  double seconds = 30.0;
  double gain_db = 0.0;
  double denoise = -1.0;
  bool gain_set = false;
  bool save_settings = false;
  bool set_default = true;
  std::wstring preset_id;
  std::wstring effects;
  std::wstring vst2_path;
  std::vector<ms::engine::PluginParameter> vst2_params;
  std::wstring vst3_path;
  std::vector<ms::engine::PluginParameter> vst3_params;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--input" && i + 1 < args.size()) {
      input_index = std::stoi(args[++i]);
    } else if (args[i] == L"--output" && i + 1 < args.size()) {
      output_index = std::stoi(args[++i]);
    } else if (args[i] == L"--mic" && i + 1 < args.size()) {
      mic_index = std::stoi(args[++i]);
    } else if (args[i] == L"--seconds" && i + 1 < args.size()) {
      seconds = std::stod(args[++i]);
    } else if (args[i] == L"--gain" && i + 1 < args.size()) {
      gain_db = std::stod(args[++i]);
      gain_set = true;
    } else if (args[i] == L"--denoise" && i + 1 < args.size()) {
      denoise = std::stod(args[++i]);
    } else if (args[i] == L"--preset" && i + 1 < args.size()) {
      preset_id = args[++i];
    } else if (args[i] == L"--effects" && i + 1 < args.size()) {
      effects = args[++i];
    } else if (args[i] == L"--vst2" && i + 1 < args.size()) {
      vst2_path = args[++i];
    } else if (args[i] == L"--vst2-param" && i + 1 < args.size()) {
      ms::engine::PluginParameter parameter;
      if (parse_plugin_param(args[++i], parameter)) {
        vst2_params.push_back(parameter);
      } else {
        std::cout << "忽略无效的 VST2 参数（应为 序号=值）：" << ms::platform::to_utf8(args[i])
                  << "\n";
      }
    } else if (args[i] == L"--vst3" && i + 1 < args.size()) {
      vst3_path = args[++i];
    } else if (args[i] == L"--vst3-param" && i + 1 < args.size()) {
      ms::engine::PluginParameter parameter;
      if (parse_plugin_param(args[++i], parameter)) {
        vst3_params.push_back(parameter);
      } else {
        std::cout << "忽略无效的 VST3 参数（应为 参数ID=值）：" << ms::platform::to_utf8(args[i])
                  << "\n";
      }
    } else if (args[i] == L"--save") {
      save_settings = true;
    } else if (args[i] == L"--no-default") {
      set_default = false;
    }
  }

  const ms::config::Settings saved = ms::config::Settings::load();
  if (preset_id.empty()) {
    preset_id = ms::platform::from_utf8(saved.preset_id);
  }
  const auto* preset = ms::presets::PresetLibrary::find(ms::platform::to_utf8(preset_id));
  if (preset == nullptr) {
    std::cout << "未知场景。可用：meeting / noisy / stream / raw\n";
    return 2;
  }
  if (!gain_set) {
    gain_db = saved.gain_db;
  }
  if (denoise < 0.0) {
    denoise = saved.denoise_amount;
  }
  std::string effect_list = ms::platform::to_utf8(effects);
  if (effect_list.empty()) {
    effect_list = effects_from_settings(saved);
  }

  const auto inputs = ms::audio_io::enumerate_devices(Flow::Capture);
  const auto outputs = ms::audio_io::enumerate_devices(Flow::Render);
  if (!inputs.error.ok() || !outputs.error.ok()) {
    std::cout << "枚举设备失败。\n";
    return 2;
  }

  input_index = resolve_endpoint(inputs.items, input_index, saved.input_id, saved.input_name);
  output_index = resolve_endpoint(outputs.items, output_index, saved.output_id, saved.output_name);
  mic_index = resolve_endpoint(inputs.items, mic_index, saved.mic_id, saved.mic_name);

  if (input_index < 0 || output_index < 0) {
    std::cout << "没有可用的设备配置。请先运行一次：\n"
              << "  engine-cli run --input <麦克风序号> --output <虚拟声卡序号> --mic <虚拟麦克风序号> --save\n";
    return 2;
  }

  // 首次使用且没指定 --mic 时，尝试自动找 VB-CABLE 的 CABLE Output
  if (mic_index < 0) {
    for (const auto& device : inputs.items) {
      if (device.friendlyName.find(L"CABLE Output") != std::wstring::npos) {
        mic_index = static_cast<int>(device.index);
        break;
      }
    }
  }
  if (mic_index < 0) {
    std::cout << "找不到虚拟麦克风端点，请用 --mic 指定。\n";
    return 2;
  }

  const auto& input = inputs.items[static_cast<std::size_t>(input_index)];
  const auto& sink = outputs.items[static_cast<std::size_t>(output_index)];
  const auto& mic = inputs.items[static_cast<std::size_t>(mic_index)];

  ms::engine::EngineOptions options;
  options.input_endpoint_id = input.id;
  options.output_endpoint_id = sink.id;
  options.input_gain_db = static_cast<float>(gain_db);
  options.denoise_amount = static_cast<float>(denoise);
  options.clarity = effect_list.find("clarity") != std::string::npos;
  options.body = effect_list.find("body") != std::string::npos;
  options.leveler = effect_list.find("leveler") != std::string::npos;
  options.deesser = effect_list.find("deesser") != std::string::npos;
  apply_plugins(options, vst2_path, vst2_params, vst3_path, vst3_params);

  ms::engine::AudioEngine engine;
  auto error = engine.start(options);
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  const auto initial_stats = engine.stats();
  if (initial_stats.deepfilter_active) {
    std::cout << "内置降噪：DeepFilterNet，延迟 " << std::fixed << std::setprecision(1)
              << initial_stats.deepfilter_latency_ms << " ms\n";
  } else {
    std::cout << "内置降噪：DeepFilterNet 加载失败（"
              << ms::platform::to_utf8(engine.builtin_denoise_error()) << "）\n";
  }
  if (initial_stats.plugins_loaded > 0) {
    std::cout << "插件已加载 " << initial_stats.plugins_loaded << " 个，总延迟 " << std::fixed
              << std::setprecision(1) << initial_stats.plugin_latency_ms << " ms\n";
  }

  if (save_settings) {
    ms::config::Settings updated = saved;
    updated.input_id = input.id;
    updated.input_name = input.friendlyName;
    updated.output_id = sink.id;
    updated.output_name = sink.friendlyName;
    updated.mic_id = mic.id;
    updated.mic_name = mic.friendlyName;
    updated.preset_id = ms::platform::to_utf8(preset_id);
    updated.gain_db = static_cast<float>(gain_db);
    updated.denoise_amount = static_cast<float>(denoise);
    updated.clarity = options.clarity;
    updated.body = options.body;
    updated.leveler = options.leveler;
    updated.deesser = options.deesser;
    updated.set_default_mic = set_default;

    if (updated.save()) {
      std::cout << "配置已保存：" << ms::platform::to_utf8(ms::config::Settings::file_path())
                << "\n";
    } else {
      std::cout << "警告：配置保存失败。\n";
    }
  }

  std::wstring previous_default;
  if (set_default) {
    previous_default = ms::platform::set_default_capture(mic.id);
    if (previous_default.empty()) {
      std::cout << "警告：切换默认麦克风失败，请在系统设置中手动选择 "
                << ms::platform::to_utf8(mic.friendlyName) << "\n";
    }
  }

  std::cout << "已就绪：" << preset->name << " 场景｜输入 "
            << ms::platform::to_utf8(input.friendlyName) << " → "
            << ms::platform::to_utf8(mic.friendlyName) << "\n";
  if (set_default) {
    std::cout << "已把默认麦克风切到虚拟设备，打开任意会议软件即可使用。\n";
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
  engine.stop();

  if (set_default && !previous_default.empty()) {
    if (ms::platform::restore_default_capture(previous_default)) {
      std::cout << "已还原原来的默认麦克风。\n";
    } else {
      std::cout << "警告：还原默认麦克风失败，请手动检查系统设置。\n";
    }
  }
  return 0;
}

// 列出 VST3 插件的参数 ID / 名称 / 默认值，便于用 --vst3-param 调节。
#ifdef MS_ENABLE_VST3
int run_vst3_info(const std::vector<std::wstring>& args) {
  std::wstring path;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == L"--vst3" && i + 1 < args.size()) {
      path = args[++i];
    }
  }
  if (path.empty()) {
    std::cout << "用法：engine-cli vst3-info --vst3 <路径>\n";
    return 2;
  }

  ms::vst3_host::Vst3Slot slot;
  auto error = slot.load(path);
  if (!error.ok()) {
    print_error(error);
    return 3;
  }

  const auto parameters = slot.parameters();
  std::cout << "VST3：" << ms::platform::to_utf8(path) << "\n参数（" << parameters.size()
            << " 个）：\n";
  for (const auto& parameter : parameters) {
    std::cout << "  id=" << parameter.id << "  当前 " << std::fixed << std::setprecision(2)
              << parameter.value << "  " << ms::platform::to_utf8(parameter.name) << "\n";
  }
  std::cout << "\n用法：engine-cli passthrough ... --vst3-param <id>=<0..1>\n";
  return 0;
}
#endif  // MS_ENABLE_VST3

}  // namespace

int wmain(int argc, wchar_t** argv) {
  ::SetConsoleOutputCP(CP_UTF8);

  ms::platform::ComApartment com;
  if (!com.usable()) {
    std::cout << "COM 初始化失败：0x" << std::hex << com.hresult() << std::dec << "\n";
    return 1;
  }

  std::vector<std::wstring> args(argv + 1, argv + argc);
  if (args.empty()) {
    print_usage();
    return 1;
  }

  if (args[0] == L"list") {
    print_devices(Flow::Capture, "采集端点");
    print_devices(Flow::Render, "渲染端点");
    print_mme_capture_devices();
    return 0;
  }
  if (args[0] == L"capture") {
    return run_capture(args);
  }
  if (args[0] == L"passthrough") {
    return run_passthrough(args);
  }
  if (args[0] == L"tone") {
    return run_tone(args);
  }
  if (args[0] == L"monitor") {
    return run_monitor(args);
  }
  if (args[0] == L"ab") {
    return run_ab(args);
  }
  if (args[0] == L"run") {
    return run_oneshot(args);
  }
  if (args[0] == L"vst3-info") {
#ifdef MS_ENABLE_VST3
    return run_vst3_info(args);
#else
    std::cout << "本次构建未包含 VST3 支持（配置时用了 -DMS_ENABLE_VST3=OFF）。\n";
    return 2;
#endif
  }

  print_usage();
  return 1;
}
