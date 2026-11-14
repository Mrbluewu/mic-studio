#pragma once

namespace ms::platform {

// 把当前线程挂到 MMCSS "Pro Audio" 任务下，并在析构时还原。
// 只应在音频循环真正开始时构造，避免空闲线程长期占用高优先级。
class RtScope {
 public:
  RtScope();
  ~RtScope();

  RtScope(const RtScope&) = delete;
  RtScope& operator=(const RtScope&) = delete;

  bool active() const noexcept { return handle_ != nullptr; }

 private:
  void* handle_{nullptr};
  unsigned long task_index_{0};
};

}  // namespace ms::platform
