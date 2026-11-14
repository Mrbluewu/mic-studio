#pragma once

// 全项目唯一的图标绘制入口。
//
// 之前麦克风/扬声器/箭头/电源/品牌标分散在 ui_theme.cpp 与 ui_controls.cpp 里，
// 各自定义线宽（1.0 / 1.6 / 4.0）和灰色（124,143,157 / 84,104,122 / 150,170,184），
// 视觉上明显不是一套。这里统一成：
//   · 一个中性灰      kIconGray
//   · 两档线宽        kStrokeSmall（16px 栅格）/ kStrokeLarge（26px 及以上）
//   · 所有图标都按 16 单位栅格设计，靠 size 等比缩放，尺寸不再随手写
//   · 圆角填充/描边也只剩这一份实现，两个 UI 源文件不再各写一套

#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>

#include <memory>

namespace ui {
namespace icons {

// 图标统一中性灰：麦克风、扬声器、折叠箭头、关闭态电源都用它。
constexpr COLORREF kIconGray = RGB(124, 143, 157);

// 线宽按 16 单位栅格定义，随 size 等比缩放：
//   kStrokeSmall → 常规描边（16px 时约 1.6px）
//   kStrokeLarge → 大号电源环（26px 时约 4px）
constexpr float kStrokeSmall = 1.6f;
constexpr float kStrokeLarge = 2.5f;

inline Gdiplus::Color to_color(COLORREF color, BYTE alpha = 255) {
  return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

// GDI+ 的 Pen 复制构造是私有的，无法按值返回；统一用 unique_ptr 交给调用方持有。
inline std::unique_ptr<Gdiplus::Pen> make_pen(const Gdiplus::Color& color, float width) {
  auto pen = std::make_unique<Gdiplus::Pen>(color, width);
  pen->SetStartCap(Gdiplus::LineCapRound);
  pen->SetEndCap(Gdiplus::LineCapRound);
  pen->SetLineJoin(Gdiplus::LineJoinRound);
  return pen;
}

// ---------------------------------------------------------------------------
// 圆角几何：填充与描边共用同一套路径
// ---------------------------------------------------------------------------

inline void add_round_rect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect,
                           float radius) {
  const float diameter = radius * 2.0f;
  path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
  path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
  path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0.0f,
              90.0f);
  path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
  path.CloseFigure();
}

inline void fill_round(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect, float radius,
                       const Gdiplus::Color& color) {
  Gdiplus::GraphicsPath path;
  add_round_rect(path, rect, radius);
  Gdiplus::SolidBrush brush(color);
  graphics.FillPath(&brush, &path);
}

// 描边严格沿 rect 边界绘制。整数 RECT 的调用方用整数矩形版，
// 由它内缩 0.5px 让 1px 线正好落在像素中心，避免半像素发虚。
inline void stroke_round(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect, float radius,
                         const Gdiplus::Color& color, float width) {
  Gdiplus::GraphicsPath path;
  add_round_rect(path, rect, radius);
  Gdiplus::Pen pen(color, width);
  graphics.DrawPath(&pen, &path);
}

inline void fill_round(Gdiplus::Graphics& graphics, const RECT& rect, float radius,
                       const Gdiplus::Color& color) {
  const Gdiplus::RectF box(static_cast<float>(rect.left), static_cast<float>(rect.top),
                           static_cast<float>(rect.right - rect.left),
                           static_cast<float>(rect.bottom - rect.top));
  fill_round(graphics, box, radius, color);
}

inline void stroke_round(Gdiplus::Graphics& graphics, const RECT& rect, float radius,
                         const Gdiplus::Color& color, float width) {
  const Gdiplus::RectF box(static_cast<float>(rect.left) + 0.5f,
                           static_cast<float>(rect.top) + 0.5f,
                           static_cast<float>(rect.right - rect.left) - 1.0f,
                           static_cast<float>(rect.bottom - rect.top) - 1.0f);
  stroke_round(graphics, box, radius, color, width);
}

inline void fill_dot(Gdiplus::Graphics& graphics, float cx, float cy, float diameter,
                     const Gdiplus::Color& color) {
  Gdiplus::SolidBrush brush(color);
  graphics.FillEllipse(&brush, cx - diameter / 2.0f, cy - diameter / 2.0f, diameter, diameter);
}

// ---------------------------------------------------------------------------
// 图标：全部以 (cx, cy) 为中心，size 是 16 单位栅格的缩放尺寸
// ---------------------------------------------------------------------------

// 麦克风：胶囊 + 半圆托架 + 短杆。
inline void draw_mic(Gdiplus::Graphics& graphics, float cx, float cy, float size,
                     const Gdiplus::Color& color) {
  const float u = size / 16.0f;
  const auto pen = make_pen(color, kStrokeSmall * u);

  Gdiplus::GraphicsPath capsule;
  add_round_rect(capsule, Gdiplus::RectF(cx - 3.5f * u, cy - 7.0f * u, 7.0f * u, 10.0f * u),
                 3.5f * u);
  graphics.DrawPath(pen.get(), &capsule);
  graphics.DrawArc(pen.get(), Gdiplus::RectF(cx - 6.0f * u, cy - 5.0f * u, 12.0f * u, 11.0f * u),
                   0.0f, 180.0f);
  graphics.DrawLine(pen.get(), cx, cy + 6.0f * u, cx, cy + 8.0f * u);
}

// 扬声器：梯形箱体 + 右侧两道声波弧。整体在 16 单位栅格内左右对称（-7..+7）。
inline void draw_speaker(Gdiplus::Graphics& graphics, float cx, float cy, float size,
                         const Gdiplus::Color& color) {
  const float u = size / 16.0f;
  const auto pen = make_pen(color, kStrokeSmall * u);

  Gdiplus::GraphicsPath body;
  body.AddLine(cx - 7.0f * u, cy - 2.5f * u, cx - 4.0f * u, cy - 2.5f * u);
  body.AddLine(cx - 4.0f * u, cy - 2.5f * u, cx - 0.5f * u, cy - 6.0f * u);
  body.AddLine(cx - 0.5f * u, cy - 6.0f * u, cx - 0.5f * u, cy + 6.0f * u);
  body.AddLine(cx - 0.5f * u, cy + 6.0f * u, cx - 4.0f * u, cy + 2.5f * u);
  body.AddLine(cx - 4.0f * u, cy + 2.5f * u, cx - 7.0f * u, cy + 2.5f * u);
  body.CloseFigure();
  graphics.DrawPath(pen.get(), &body);
  // 单道声波弧：以图标中心为圆心的右侧弧，最右点 cx + 5.5，不会越出 16 单位框。
  graphics.DrawArc(pen.get(), Gdiplus::RectF(cx - 5.5f * u, cy - 5.5f * u, 11.0f * u, 11.0f * u),
                   -55.0f, 110.0f);
}

// 折叠/展开箭头：down=true 朝下，false 朝上。
inline void draw_chevron(Gdiplus::Graphics& graphics, float cx, float cy, float size,
                         const Gdiplus::Color& color, bool down) {
  const float u = size / 16.0f;
  const auto pen = make_pen(color, kStrokeSmall * u);
  const float half = 4.0f * u;
  const float rise = 2.5f * u;
  if (down) {
    graphics.DrawLine(pen.get(), cx - half, cy - rise, cx, cy + rise);
    graphics.DrawLine(pen.get(), cx, cy + rise, cx + half, cy - rise);
  } else {
    graphics.DrawLine(pen.get(), cx - half, cy + rise, cx, cy - rise);
    graphics.DrawLine(pen.get(), cx, cy - rise, cx + half, cy + rise);
  }
}

// 电源：顶部留 60° 缺口的圆环 + 竖线。
inline void draw_power(Gdiplus::Graphics& graphics, float cx, float cy, float size,
                       const Gdiplus::Color& color) {
  const float u = size / 16.0f;
  const auto pen = make_pen(color, kStrokeLarge * u);
  const float radius = 8.0f * u;
  graphics.DrawArc(pen.get(), Gdiplus::RectF(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f),
                   -60.0f, 300.0f);
  graphics.DrawLine(pen.get(), cx, cy - radius, cx, cy + 2.0f * u);
}

// 品牌标：强调色实心圆角方块。
inline void draw_logo(Gdiplus::Graphics& graphics, const RECT& rect,
                      const Gdiplus::Color& color) {
  fill_round(graphics, rect, 5.0f, color);
}

}  // namespace icons
}  // namespace ui
