#pragma once

#include "dsp_core/bounded_parameter_queue.h"
#include "engine/tap_buffer.h"
#include "platform/error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ms::engine {

enum class EngineState { Stopped, Starting, Running, Recovering, Failed };

struct PluginParameter {
  int index = 0;       // VST2 为参数序号，VST3 为 ParamID
  float value = 0.0f;  // 归一化 0..1
};

// 运行时参数快照：宿主用来持久化插件编辑器里改过的值。
struct PluginParameterInfo {
  int slot = 0;  // 插件槽序号，0 起
  int id = 0;
  std::wstring name;
  float value = 0.0f;  // 当前归一化值 0..1
};

// 插件界面产生的参数变化，宿主用来标记参数快照需要落盘。
struct PluginParameterChange {
  int slot = 0;
  int id = 0;
  float value = 0.0f;
};

// UI 线程投递到音频线程的插件参数命令：定容量、无锁、满载丢弃。
// 打包成 8 字节以适配队列单元：slot 8 位 | id 24 位 | 归一化值 32 位（float 位模式）。
struct PluginParameterCommand {
  std::uint64_t packed = 0;
};

// 一个插件槽：槽内只能是 VST2 或 VST3 之一。
struct PluginSlot {
  bool vst3 = false;
  std::wstring path;
  std::vector<PluginParameter> params;
};

struct EngineOptions {
  std::wstring input_endpoint_id;
  std::wstring output_endpoint_id;
  std::uint32_t engine_rate = 48000;
  // 环形缓冲目标水位。12 ms 在 60 s 压测里出现个别掉帧（最低水位仅 3.7 ms），
  // 取 15 ms：仍能吸收 10 ms 采集包节拍与线程调度抖动，又比原来的 20 ms 少 5 ms。
  double target_fill_ms = 15.0;
  float input_gain_db = 0.0f;
  float output_gain_db = 0.0f;
  float denoise_amount = 0.0f;
  bool clarity = false;
  bool body = false;
  bool leveler = false;
  bool deesser = false;

  // 内置降噪：固定使用 DeepFilterNet（VST2 DLL）；缺失或加载失败时不做其它后端回退，
  // 而是把错误通过 builtin_denoise_error() 报给界面。
  bool builtin_denoise = true;
  std::wstring deepfilter_path;

  // 可选插件槽（最多 3 个，按顺序串接在内置降噪之后）。
  // 单个槽加载失败会被跳过并记录错误，不影响引擎启动。
  std::vector<PluginSlot> plugins;
};

class AudioEngine {
 public:
  struct Stats {
    std::uint64_t captured_frames = 0;
    std::uint64_t rendered_frames = 0;
    std::uint64_t overruns = 0;
    std::uint64_t underruns = 0;
    std::uint32_t fill_frames = 0;
    double fill_ms = 0.0;
    double applied_ppm = 0.0;
    double latency_ms = 0.0;

    // 线程活性诊断：两个计数应随时间线性增长。
    std::uint32_t capture_buffers = 0;
    std::uint32_t render_buffers = 0;
    std::uint32_t capture_timeouts = 0;
    std::uint32_t render_timeouts = 0;
    std::uint32_t recoveries = 0;

    // 噪声底：处理前 / 处理后（dBFS），以及由此得到的信噪比提升
    float noise_floor_in_db = -120.0f;
    float noise_floor_out_db = -120.0f;
    float snr_gain_db = 0.0f;

    // 内置降噪后端：DeepFilterNet 是否生效，以及其算法延迟
    bool deepfilter_active = false;
    double deepfilter_latency_ms = 0.0;

    // 可选插件槽：成功加载的数量与总延迟
    int plugins_loaded = 0;
    double plugin_latency_ms = 0.0;
  };

  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // 打开采集与渲染端点并启动链路。失败时返回明确错误码，且不留下半启动状态。
  platform::Error start(const EngineOptions& options);
  void stop();

  // 运行中调整输入增益，内部走 20 ms 斜坡
  void set_input_gain_db(float db);

  // 运行中调整输出增益（处理链末端），内部走 20 ms 斜坡
  void set_output_gain_db(float db);

  // 0 = 关闭降噪，1 = 最强
  void set_denoise_amount(float amount);

  // 四张效果卡的开关
  void set_effects(bool clarity, bool body, bool leveler, bool deesser);

  // 旁路：跳过整条处理链，音频直通
  void set_bypass(bool bypass);

  // 当前已加载插件的参数表；非实时线程调用。
  std::vector<PluginParameterInfo> plugin_parameters() const;

  // 运行中调整插件参数（归一化 0..1），slot 为插件槽序号
  void set_plugin_parameter(int slot, int id, float value);

  // 取走插件界面产生的参数变化，并同步内部参数缓存。
  std::vector<PluginParameterChange> take_plugin_parameter_changes();

  // 每个插件槽的加载错误（与 options.plugins 对齐，空串表示成功）
  std::vector<std::wstring> plugin_slot_errors() const;

  // 内置降噪（DeepFilterNet）加载失败的原因；空串表示加载成功或未启用。
  std::wstring builtin_denoise_error() const;

  // 插件自带编辑器（必须在 UI 线程调用）。slot 指定操作哪一个插件槽。
  bool plugin_has_editor(int slot) const;
  bool open_plugin_editor(int slot, void* parent, int& width, int& height);
  void close_plugin_editor(int slot);
  void plugin_editor_idle(int slot);
  bool plugin_editor_size(int slot, int& width, int& height) const;

  EngineState state() const noexcept;
  // 全部字段来自 atomic 快照，可在任意线程高频调用。
  Stats stats() const noexcept;

  // A/B 对比用的最近 4 秒缓存（非实时读取）
  const TapBuffer& raw_tap() const noexcept;
  const TapBuffer& processed_tap() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ms::engine
