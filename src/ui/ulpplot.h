// ULP-error plot: input on x, error in ULP on y.
//
// Qt Charts is not available in this environment and there is no root access to
// install it, so this is a self-contained QPainter widget. That turns out to be
// a good fit: the data is one point per input, we only need a linear x axis and
// one selectable series, and drawing 65k points costs nothing.
#pragma once

#include <cstdint>
#include <vector>

#include <QPoint>
#include <QString>
#include <QWidget>

#include "core/scan.h"

using ulpscope::Sample;
using ulpscope::ScanResult;

class UlpPlot : public QWidget {
  Q_OBJECT

public:
  // What the points are coloured by. Error magnitude is the default; class is
  // useful for seeing whether a defect is confined to denormals or to the
  // extremes of the exponent range.
  enum class ColorMode { Result, InputClass };

  explicit UlpPlot(QWidget *parent = nullptr);

  void setResult(const ScanResult *result);
  void setLogScale(bool on);
  bool logScale() const { return log_; }
  void setColorMode(ColorMode mode);
  ColorMode colorMode() const { return colorMode_; }

  // Index into ScanResult::samples, or -1.
  void setSelectedIndex(int index);
  int selectedIndex() const { return selected_; }

  // Size of the plotted error range, for the axis label.
  uint64_t maxUlp() const;

  // Horizontal view window, in input bit-pattern units. Defaults to the whole
  // input space; the mouse wheel and drag narrow it.
  bool isZoomed() const { return zoomed_; }
  void resetView();
  std::pair<double, double> viewRange() const;

signals:
  // Emitted when the user picks a point, with the sample index.
  void samplePicked(int index);
  // Emitted when the hover moves over a point.
  void sampleHovered(int index);

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void leaveEvent(QEvent *) override;

private:
  struct Box {
    QRectF rect;    // plot area in widget coordinates
    double xMin, xMax;
    double yMax;    // in ULP
  };

  void drawAxes(QPainter &p, const Box &b) const;
  void drawPoints(QPainter &p, const Box &b) const;
  void drawMarker(QPainter &p, const Box &b) const;

  Box plotBox() const;

  double xToPx(double x, const Box &b) const;
  double pxToX(double px, const Box &b) const;
  double errToPx(double err, const Box &b) const;
  double pxToErr(double py, const Box &b) const;

  // Samples whose input bit pattern is nearest to the given x. Prefers an
  // exact match, then the closest within `tolPixels`.
  int nearestSample(double xPx, double yPx, const Box &b, double tolPixels) const;

  const ScanResult *result_ = nullptr;
  bool log_ = true;
  ColorMode colorMode_ = ColorMode::Result;
  int selected_ = -1;
  int hovered_ = -1;

  // Visible x window. Empty while !zoomed_, in which case the whole input
  // space is shown.
  double viewMin_ = 0.0;
  double viewMax_ = 0.0;
  bool zoomed_ = false;
  // Drag state for panning: the x that the press started at, and where the
  // pointer is now, both in data coordinates.
  bool dragging_ = false;
  double dragStartPx_ = 0.0;
  double dragAnchorX_ = 0.0;
  double dragViewMin_ = 0.0;
  double dragViewMax_ = 0.0;
};