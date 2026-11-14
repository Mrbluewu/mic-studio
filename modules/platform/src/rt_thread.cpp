#include "platform/rt_thread.h"

#include <windows.h>

#include <avrt.h>

namespace ms::platform {

RtScope::RtScope() {
  DWORD index = 0;
  HANDLE handle = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
  if (handle != nullptr) {
    handle_ = handle;
    task_index_ = index;
    ::AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL);
  }
}

RtScope::~RtScope() {
  if (handle_ != nullptr) {
    ::AvRevertMmThreadCharacteristics(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

}  // namespace ms::platform
