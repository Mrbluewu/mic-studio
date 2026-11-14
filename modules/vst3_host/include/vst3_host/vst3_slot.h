#pragma once

#include "dsp_core/processor.h"
#include "platform/error.h"

#include <memory>
#include <string>
#include <vector>

namespace ms::vst3_host {

// VST3 插件插槽：加载 64 位 VST3 模块（单文件 DLL 或 .vst3 包），
// 作为链上的一级处理单元。只接受 1~2 进 1~2 出的音频效果器；
// 实时线程内不分配内存，输入按内部固定块长切片送给插件。
class Vst3Slot : public dsp_core::IProcessor {
 public:
  struct ParameterInfo {
    int id = 0;              // VST3 ParamID
    std::wstring name;
    float value = 0.0f;      // 当前归一化值 0..1
  };

  // 插件界面 → 宿主的参数变化通知（来自 IComponentHandler::performEdit）。
  struct ParameterChange {
    int id = 0;
    float value = 0.0f;
  };
  Vst3Slot();
  ~Vst3Slot() override;

  Vst3Slot(const Vst3Slot&) = delete;
  Vst3Slot& operator=(const Vst3Slot&) = delete;

  // 加载模块并实例化音频效果器；失败时返回错误，且不留下半加载状态。
  platform::Error load(const std::wstring& path);

  void prepare(const dsp_core::ProcessContext& context) override;
  void process(float* data, int frames) override;
  void reset() override;

  bool loaded() const noexcept;

  // 插件参数表（需要在 load 之后、非实时线程调用）。
  std::vector<ParameterInfo> parameters() const;

  // 取走插件界面产生的参数变化（UI 线程调用）。
  std::vector<ParameterChange> take_parameter_changes();

  // 插件参数（归一化 0..1）。id 为 VST3 的 ParamID。
  void set_parameter(int id, float value);

  // 插件自带编辑器：parent 为宿主窗口句柄（HWND），返回初始尺寸。
  // 必须在 UI 线程调用。
  bool has_editor() const;
  bool open_editor(void* parent, int& width, int& height);
  void close_editor();
  void editor_idle();
  bool editor_size(int& width, int& height) const;

  // 插件自报延迟（帧，按 prepare 传入的采样率）
  int latency_frames() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ms::vst3_host
