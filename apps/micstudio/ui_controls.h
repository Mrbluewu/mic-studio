#pragma once

#include <windows.h>

namespace ui {

const wchar_t* slider_class();
const wchar_t* meter_class();

void register_control_classes(HINSTANCE instance);

// 更新底噪对比条；同一线程内调用，内部做数值平滑。
void set_meter(HWND meter, float raw_db, float processed_db, bool bypass);

#ifdef MICSTUDIO_UI_TESTING
// 仅供单元测试：读取底噪条内部平滑状态，生产构建不暴露。
struct MeterSnapshot {
  float target_raw = 0.0f;
  float target_out = 0.0f;
  float shown_raw = 0.0f;
  float shown_out = 0.0f;
  bool bypass = false;
  bool animating = false;
};

bool meter_snapshot(HWND meter, MeterSnapshot& out);

// 仅供单元测试：把底噪条绘制到指定 DC，不依赖 WM_PAINT。
void meter_paint_to(HWND meter, HDC dc);

// 仅供单元测试：把滑杆绘制到指定 DC，不依赖 WM_PAINT。
void slider_paint_to(HWND slider, HDC dc);
#endif

}  // namespace ui
