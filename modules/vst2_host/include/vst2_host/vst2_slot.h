#pragma once

#include "dsp_core/bounded_parameter_queue.h"
#include "dsp_core/processor.h"
#include "platform/error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ms::vst2_host {

struct AEffect;

// VST2 插件插槽：把 64 位 VST2 效果器当作链上的一级处理单元。
// 只接受 1~2 进 1~2 出的效果器，不支持乐器；实时线程内不分配内存，
// 输入按内部固定块长切片送给插件，单声道会复制到左右声道、处理后下混。
class Vst2Slot : public dsp_core::IProcessor {
 public:
  struct ParameterInfo {
    int id = 0;              // VST2 参数序号
    std::wstring name;
    float value = 0.0f;      // 归一化 0..1
  };

  // 插件界面 → 宿主的参数变化通知（来自 audioMasterAutomate）。
  struct ParameterChange {
    int id = 0;
    float value = 0.0f;
  };
  Vst2Slot();
  ~Vst2Slot() override;

  Vst2Slot(const Vst2Slot&) = delete;
  Vst2Slot& operator=(const Vst2Slot&) = delete;

  // 加载 DLL 并完成 effOpen；失败时返回错误，且不留下半加载状态。
  platform::Error load(const std::wstring& path);

  void prepare(const dsp_core::ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;

  bool loaded() const noexcept { return effect_ != nullptr; }

  // 插件参数表（需要在 load 之后、非实时线程调用）。
  std::vector<ParameterInfo> parameters() const;

  // 取走插件界面产生的参数变化（UI 线程调用）。
  std::vector<ParameterChange> take_parameter_changes();

  // 插件参数（归一化 0..1）。可在运行中调用，插件内部自行加锁。
  void set_parameter(int index, float value);

  // 插件自带编辑器：parent 为宿主窗口句柄（HWND），返回初始尺寸。
  // 必须在 UI 线程调用。
  bool has_editor() const;
  bool open_editor(void* parent, int& width, int& height);
  void close_editor();
  void editor_idle();
  bool editor_size(int& width, int& height) const;

  // 插件自报延迟（帧，按 prepare 传入的采样率）
  int latency_frames() const noexcept { return latency_frames_; }

 private:
  static intptr_t host_callback(AEffect* effect, std::int32_t opcode, std::int32_t index,
                                intptr_t value, void* ptr, float opt);
  void record_parameter_change(int index, float value);
  void close();

  void* module_ = nullptr;
  AEffect* effect_ = nullptr;
  int num_outputs_ = 0;
  int latency_frames_ = 0;
  double sample_rate_ = 48000.0;
  int block_size_ = 0;

  bool editor_open_ = false;
  int editor_width_ = 0;
  int editor_height_ = 0;

  ms::dsp_core::BoundedParameterQueue<ParameterChange, 2048> pending_changes_;

  // 每声道内部缓冲，长度等于插件块长；只在 load 时分配一次。
  std::vector<float> in_l_;
  std::vector<float> in_r_;
  std::vector<float> out_l_;
  std::vector<float> out_r_;
};

}  // namespace ms::vst2_host
