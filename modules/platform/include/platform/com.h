#pragma once

#include <combaseapi.h>

namespace ms::platform {

// RAII 管理 COM 套间。音频线程与主线程都使用 MTA，避免跨套间封送。
class ComApartment {
 public:
  ComApartment() : hr_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
    // RPC_E_CHANGED_MODE 表示本线程已被初始化为 STA，接口仍可使用。
    usable_ = SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
  }

  ~ComApartment() {
    if (SUCCEEDED(hr_)) {
      ::CoUninitialize();
    }
  }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  bool usable() const noexcept { return usable_; }
  long hresult() const noexcept { return hr_; }

 private:
  long hr_;
  bool usable_{false};
};

}  // namespace ms::platform
