#pragma once

#include "audio_io/endpoint.h"

#include <windows.h>

#include <ksmedia.h>

namespace ms::audio_io::detail {

inline AudioFormat parse_format(const WAVEFORMATEX* wfx) {
  AudioFormat format;
  if (wfx == nullptr) {
    return format;
  }
  format.sampleRate = wfx->nSamplesPerSec;
  format.channels = wfx->nChannels;
  format.bitsPerSample = wfx->wBitsPerSample;
  format.blockAlign = wfx->nBlockAlign;
  format.isFloat = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);

  if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
    format.isFloat = ::IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
  }
  return format;
}

}  // namespace ms::audio_io::detail
