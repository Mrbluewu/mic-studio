#pragma once

#include <memory>
#include <vector>

namespace ms::dsp_core {

struct ProcessContext {
  double sampleRate = 48000.0;
  int maxBlockSize = 1024;
};

// 所有处理单元的统一形态。输入输出都是 48 kHz mono float，原地处理。
// 实时线程只允许调用 process()，禁止分配、加锁、日志。
class IProcessor {
 public:
  virtual ~IProcessor() = default;

  virtual void prepare(const ProcessContext& context) = 0;
  virtual void process(float* data, int frames) = 0;
  virtual void reset() = 0;
};

// 串行链：按添加顺序处理，空链即直通。
class Chain : public IProcessor {
 public:
  void add(std::unique_ptr<IProcessor> processor) { stages_.push_back(std::move(processor)); }

  std::size_t size() const noexcept { return stages_.size(); }

  void prepare(const ProcessContext& context) override {
    for (auto& stage : stages_) {
      stage->prepare(context);
    }
  }

  void process(float* data, int frames) override {
    for (auto& stage : stages_) {
      stage->process(data, frames);
    }
  }

  void reset() override {
    for (auto& stage : stages_) {
      stage->reset();
    }
  }

 private:
  std::vector<std::unique_ptr<IProcessor>> stages_;
};

}  // namespace ms::dsp_core
