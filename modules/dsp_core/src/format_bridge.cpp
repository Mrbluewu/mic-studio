#include "dsp_core/format_bridge.h"

#include <algorithm>
#include <cmath>

namespace ms::dsp_core {
namespace {

constexpr std::uint32_t kEngineRate = 48000;

bool is_supported(const audio_io::AudioFormat& format) {
  if (format.channels == 0 || format.sampleRate == 0) {
    return false;
  }
  if (format.isFloat && format.bitsPerSample == 32) {
    return true;
  }
  return !format.isFloat && format.bitsPerSample == 16;
}

float read_sample(const std::uint8_t* data, std::size_t index, const audio_io::AudioFormat& format) {
  if (format.isFloat && format.bitsPerSample == 32) {
    return reinterpret_cast<const float*>(data)[index];
  }
  if (!format.isFloat && format.bitsPerSample == 16) {
    return static_cast<float>(reinterpret_cast<const std::int16_t*>(data)[index]) / 32768.0f;
  }
  return 0.0f;
}

void write_sample(std::uint8_t* data, std::size_t index, float value,
                  const audio_io::AudioFormat& format) {
  if (format.isFloat && format.bitsPerSample == 32) {
    reinterpret_cast<float*>(data)[index] = value;
    return;
  }
  if (!format.isFloat && format.bitsPerSample == 16) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    reinterpret_cast<std::int16_t*>(data)[index] = static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
  }
}

}  // namespace

void CaptureBridge::prepare(const audio_io::AudioFormat& device, std::uint32_t engine_rate) {
  device_ = device;
  supported_ = is_supported(device);
  resampler_.prepare(device.sampleRate, engine_rate == 0 ? kEngineRate : engine_rate);
}

std::size_t CaptureBridge::process(const std::uint8_t* data, std::uint32_t frames, float* out,
                                   std::size_t capacity) {
  if (!supported_ || data == nullptr || out == nullptr) {
    return 0;
  }

  std::size_t written = 0;
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    double sum = 0.0;
    for (std::uint16_t channel = 0; channel < device_.channels; ++channel) {
      const std::size_t index = static_cast<std::size_t>(frame) * device_.channels + channel;
      sum += read_sample(data, index, device_);
    }
    const float mono = static_cast<float>(sum / device_.channels);

    if (written >= capacity) {
      break;
    }
    written += resampler_.push(mono, out + written,
                               static_cast<std::uint32_t>(std::min<std::size_t>(
                                   capacity - written, 4096)));
  }
  return written;
}

void RenderBridge::prepare(const audio_io::AudioFormat& device, std::uint32_t engine_rate) {
  device_ = device;
  supported_ = is_supported(device);
  resampler_.prepare(engine_rate == 0 ? kEngineRate : engine_rate, device.sampleRate);
}

std::uint32_t RenderBridge::render(const float* mono, std::uint32_t in_frames, float* scratch,
                                   std::uint32_t capacity) {
  if (!supported_ || mono == nullptr || scratch == nullptr) {
    return 0;
  }
  std::uint32_t produced = 0;
  for (std::uint32_t i = 0; i < in_frames && produced < capacity; ++i) {
    produced += resampler_.push(mono[i], scratch + produced, capacity - produced);
  }
  return produced;
}

void RenderBridge::write(const float* mono, std::uint32_t frames, std::uint8_t* out) const {
  if (!supported_ || mono == nullptr || out == nullptr) {
    return;
  }
  const std::size_t total = static_cast<std::size_t>(frames) * device_.channels;
  for (std::size_t index = 0; index < total; ++index) {
    write_sample(out, index, mono[index / device_.channels], device_);
  }
}

}  // namespace ms::dsp_core
