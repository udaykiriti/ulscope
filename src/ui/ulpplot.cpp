#include "ui/ulpplot.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QWheelEvent>

#include "ui/formatting.h"

using namespace ulpscope;

namespace {

constexpr double kLogBase = 10.0;

// Errors span zero to hundreds of thousands, so a linear axis buries the
// interesting low end. log10(1 + err) is monotonic, maps zero to zero, and
// still reads as "ULP" on the tick labels.
double mapErr(double err, bool log) {
  return log ? std::log10(1.0 + std::min(err, 1e18)) : std::min(err, 1e18);
}

double unmapErr(double v, bool log) {
  if (!log)
    return v;
  return std::pow(10.0, v) - 1.0;
}

} // namespace

UlpPlot::UlpPlot(QWidget *parent) : QWidget(parent) {
  setMinimumHeight(220);
  setMouseTracking(true);
  setCursor(Qt::CrossCursor);
  setAutoFillBackground(true);
}

void UlpPlot::setResult(const ScanResult *result) {
  result_ = result;
  selected_ = -1;
  hovered_ = -1;
  // A new scan invalidates the old window; keeping it would show an empty
  // plot and look like the new scan found nothing.
  zoomed_ = false;
  update();
}

void UlpPlot::setLogScale(bool on) {
  if (log_ == on)
    return;
  log_ = on;
  update();
}

void UlpPlot::setColorMode(ColorMode mode) {
  if (colorMode_ == mode)
    return;
  colorMode_ = mode;
  update();
}

void UlpPlot::setSelectedIndex(int index) {
  if (selected_ == index)
    return;
  selected_ = index;
  update();
}

uint64_t UlpPlot::maxUlp() const {
  uint64_t m = 0;
  if (result_)
    for (const Sample &s : result_->samples)
      if (!s.nanDisagreement)
        m = std::max(m, s.ulpError);
  return m;
}

UlpPlot::Box UlpPlot::plotBox() const {
  Box b;
  const QFontMetricsF fm(font());
  b.rect = QRectF(QPointF(58.0, 10.0),
                 QPointF(width() - 14.0, height() - 26.0 - fm.height()));
  if (b.rect.width() < 10 || b.rect.height() < 10)
    return b;

  double dataMin = 0.0;
  double dataMax = 1.0;
  double maxErr = 0.0;
  if (result_) {
    dataMin = std::numeric_limits<double>::max();
    dataMax = std::numeric_limits<double>::lowest();
    for (const Sample &s : result_->samples) {
      dataMin = std::min(dataMin, static_cast<double>(s.inputBits));
      dataMax = std::max(dataMax, static_cast<double>(s.inputBits));
    }
  }
  if (dataMax <= dataMin) {
    dataMin = 0.0;
    dataMax = 1.0;
  }

  if (zoomed_) {
    b.xMin = viewMin_;
    b.xMax = viewMax_;
  } else {
    b.xMin = dataMin;
    b.xMax = dataMax;
  }
  if (b.xMax <= b.xMin)
    b.xMax = b.xMin + 1.0;

  // Scale the y axis to what is actually on screen, so zooming into a quiet
  // region does not leave it squashed against the axis by an outlier elsewhere.
  if (result_) {
    for (const Sample &s : result_->samples) {
      const double x = static_cast<double>(s.inputBits);
      if (x < b.xMin || x > b.xMax)
        continue;
      if (!s.nanDisagreement)
        maxErr = std::max(maxErr, static_cast<double>(s.ulpError));
    }
  }

  // Leave headroom above the data so points are not glued to the frame, but no
  // more: with a 1 ULP worst case the axis should show 0 and 1, not 0 to 9.
  const double mapped = mapErr(maxErr, log_);
  // Always allow at least 1 ULP of range, so an all-correct scan still draws a
  // meaningful "everything is on the zero line" picture.
  b.yMax = std::max(mapped, std::log10(2.0)) * 1.3;
  return b;
}

std::pair<double, double> UlpPlot::viewRange() const {
  return {viewMin_, viewMax_};
}

void UlpPlot::resetView() {
  zoomed_ = false;
  update();
}

double UlpPlot::xToPx(double x, const Box &b) const {
  return b.rect.left() + (x - b.xMin) / (b.xMax - b.xMin) * b.rect.width();
}

double UlpPlot::pxToX(double px, const Box &b) const {
  return b.xMin + (px - b.rect.left()) / b.rect.width() * (b.xMax - b.xMin);
}

double UlpPlot::errToPx(double err, const Box &b) const {
  const double m = mapErr(err, log_);
  return b.rect.bottom() - m / b.yMax * b.rect.height();
}

double UlpPlot::pxToErr(double py, const Box &b) const {
  const double m = (b.rect.bottom() - py) / b.rect.height() * b.yMax;
  return unmapErr(m, log_);
}

int UlpPlot::nearestSample(double xPx, double yPx, const Box &b,
                           double tolPixels) const {
  if (!result_ || result_->samples.empty())
    return -1;
  const double targetX = pxToX(xPx, b);

  int best = -1;
  double bestDist = 0.0;
  for (size_t i = 0; i < result_->samples.size(); ++i) {
    const Sample &s = result_->samples[i];
    const double dx = std::fabs(static_cast<double>(s.inputBits) - targetX);
    // Compare in pixels so the tolerance means the same thing across the axis.
    const double distPx =
        std::fabs(xToPx(static_cast<double>(s.inputBits), b) - xPx);
    if (distPx <= tolPixels && (best < 0 || dx < bestDist)) {
      best = static_cast<int>(i);
      bestDist = dx;
    }
  }
  (void)yPx;
  return best;
}

void UlpPlot::drawAxes(QPainter &p, const Box &b) const {
  const QFontMetricsF fm(font());
  p.setPen(palette().color(QPalette::Mid));

  p.drawRect(b.rect);

  // --- x axis: input bit pattern -----------------------------------------
  const int tickCount = std::max(2, static_cast<int>(b.rect.width() / 90.0));
  for (int i = 0; i <= tickCount; ++i) {
    const double t = static_cast<double>(i) / tickCount;
    const double x = b.xMin + t * (b.xMax - b.xMin);
    const double px = xToPx(x, b);
    p.drawLine(QPointF(px, b.rect.bottom()), QPointF(px, b.rect.bottom() + 4));
    const QString label = formatBitPattern(x);
    p.drawText(QRectF(px - 46, b.rect.bottom() + 6, 92, fm.height()),
               Qt::AlignCenter, label);
  }
  p.drawText(QRectF(b.rect.left(), height() - fm.height() - 2, b.rect.width(),
                    fm.height()),
             Qt::AlignCenter, QStringLiteral("input (bit pattern)"));

  // --- y axis --------------------------------------------------------------
  const double topMapped = b.yMax;
  // Ticks at 0, 1, 2, ... 10, then decades.
  std::vector<double> ticks{0.0};
  for (int i = 1; i <= 10; ++i)
    ticks.push_back(i);
  double decade = 10.0;
  while (mapErr(decade, log_) <= topMapped) {
    for (int m = 1; m < 10; ++m)
      ticks.push_back(decade * m);
    decade *= 10.0;
  }

  const QColor labelColour = palette().color(QPalette::Text);
  for (double t : ticks) {
    const double mapped = mapErr(t, log_);
    if (mapped > topMapped)
      break;
    const double py = b.rect.bottom() - mapped / topMapped * b.rect.height();
    p.setPen(palette().color(QPalette::Mid));
    p.drawLine(QPointF(b.rect.left() - 4, py), QPointF(b.rect.left(), py));
    p.drawLine(QPointF(b.rect.left(), py), QPointF(b.rect.right(), py));
    p.setPen(labelColour);
    p.drawText(QRectF(2, py - fm.height() / 2, b.rect.left() - 6, fm.height()),
               Qt::AlignRight | Qt::AlignVCenter, formatUlpCompact(t));
  }
  p.save();
  p.translate(12, b.rect.center().y());
  p.rotate(-90);
  p.drawText(QRectF(-b.rect.height() / 2, -fm.height() / 2, b.rect.height(),
                    fm.height()),
             Qt::AlignCenter, QStringLiteral("ULP error"));
  p.restore();
}

void UlpPlot::drawPoints(QPainter &p, const Box &b) const {
  if (!result_)
    return;

  static const QColor good(0x2e, 0x8b, 0x57); // sea green
  static const QColor bad(0xd6, 0x27, 0x28);  // red
  static const QColor nan(0x7b, 0x3f, 0xbf);  // purple
  static const QColor byClass[5] = {
      QColor(0x60, 0x60, 0x60), // zero
      QColor(0x1f, 0x77, 0xb4), // normal
      QColor(0xff, 0x7f, 0x0e), // denormal
      QColor(0x94, 0x67, 0xbd), // infinity
      QColor(0x8c, 0x56, 0x4b)}; // NaN

  // Collect into polygons and draw each in one call.
  //
  // This matters more than it looks. Measured over 65,536 points:
  //   65536 individual drawEllipse calls   151 ms per repaint
  //   one QPainterPath per bucket          388 ms  (worse: path building)
  //   QPolygonF + drawPoints                1.0 ms
  // At 70 ms a repaint, dragging the zoomed view is unusable, so this is the
  // difference between an interactive plot and a frozen one.
  QPolygonF buckets[5];
  for (QPolygonF &poly : buckets)
    poly.reserve(result_->samples.size());

  const QRectF visible = b.rect.adjusted(-2, -2, 2, 2);
  for (const Sample &s : result_->samples) {
    const QPointF pt(xToPx(static_cast<double>(s.inputBits), b),
                     errToPx(s.nanDisagreement
                                 ? static_cast<double>(result_->maxUlp) + 1.0
                                 : static_cast<double>(s.ulpError),
                             b));
    if (!visible.contains(pt))
      continue;

    int bucket;
    if (colorMode_ == ColorMode::InputClass) {
      const int cls = static_cast<int>(result_->config.fmt.classify(s.inputBits));
      bucket = cls >= 0 && cls < 5 ? cls : 0;
    } else {
      // Correct points go in an earlier bucket than mismatches so they are
      // drawn first and the failures land on top of them.
      bucket = s.nanDisagreement ? 3 : (s.mismatch ? 2 : 1);
    }
    buckets[bucket].append(pt);
  }

  // Antialiasing a hundred thousand one-pixel shapes costs far more than it
  // shows at this size.
  //
  // The pen carries the colour here, not the brush: drawPoints sizes its
  // squares from the pen, and with NoPen set it draws *nothing at all* - which
  // is fast, correct-looking in a benchmark, and an empty plot.
  p.setRenderHint(QPainter::Antialiasing, false);
  p.setBrush(Qt::NoBrush);
  const int first = colorMode_ == ColorMode::InputClass ? 0 : 1;
  const int count = colorMode_ == ColorMode::InputClass ? 5 : 3;
  for (int i = first; i < count; ++i) {
    if (buckets[i].isEmpty())
      continue;
    p.setPen(QPen(colorMode_ == ColorMode::InputClass
                      ? byClass[i]
                      : (i == 1 ? good : (i == 2 ? bad : nan)),
                  1.0));
    p.drawPoints(buckets[i]);
  }
  p.setPen(Qt::NoPen);
  p.setRenderHint(QPainter::Antialiasing, true);

  static const char *resultNames[] = {nullptr, "correct", "mismatch",
                                      "NaN disagreement"};
  static const char *classNames[] = {"zero", "normal", "denormal", "inf", "nan"};
  const QFontMetricsF fm(font());
  double y = b.rect.top() + 4;
  for (int i = first; i < count; ++i) {
    const QColor c = colorMode_ == ColorMode::InputClass
                         ? byClass[i]
                         : (i == 1 ? good : (i == 2 ? bad : nan));
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QPointF(b.rect.right() - 150, y + fm.height() / 2), 3.5, 3.5);
    p.setPen(palette().color(QPalette::Text));
    p.drawText(QRectF(b.rect.right() - 140, y, 138, fm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               QString::fromLatin1(colorMode_ == ColorMode::InputClass
                                       ? classNames[i]
                                       : resultNames[i]));
    y += fm.height() + 2;
  }
}

void UlpPlot::drawMarker(QPainter &p, const Box &b) const {
  if (!result_)
    return;
  const int idx = hovered_ >= 0 ? hovered_ : selected_;
  if (idx < 0 || idx >= static_cast<int>(result_->samples.size()))
    return;
  const Sample &s = result_->samples[idx];

  const QPointF pt(
      xToPx(static_cast<double>(s.inputBits), b),
      errToPx(s.nanDisagreement ? static_cast<double>(result_->maxUlp) + 1.0
                                : static_cast<double>(s.ulpError),
              b));

  p.setPen(QPen(palette().color(QPalette::Highlight), 1.0, Qt::DashLine));
  p.drawLine(QPointF(pt.x(), b.rect.top()), QPointF(pt.x(), b.rect.bottom()));
  p.drawLine(QPointF(b.rect.left(), pt.y()), QPointF(b.rect.right(), pt.y()));

  p.setPen(QPen(QColor(Qt::black), 1.2));
  p.setBrush(QColor(0xff, 0xff, 0xc0, 220));
  p.drawEllipse(pt, 4, 4);

  const QFontMetricsF fm(font());
  const QString tip =
      QStringLiteral("in %1 = %2\nexpected %3\nactual   %4\n%5 ULP")
          .arg(formatBitPattern(s.inputBits), formatExact(s.input),
               formatExact(s.expected), formatExact(s.actual),
               s.nanDisagreement ? QStringLiteral("NaN") : QString::number(s.ulpError));
  const QRectF tipRect = QRectF(pt.x() + 8, pt.y() - 8 - fm.height() * 4.4,
                                std::max(fm.horizontalAdvance(tip) + 12.0, 150.0),
                                fm.height() * 4.4 + 8);
  // Keep the tooltip inside the widget.
  QRectF box = tipRect.translated(0, 0);
  if (box.right() > width() - 4)
    box.moveLeft(pt.x() - 8 - box.width());
  if (box.top() < 2)
    box.moveTop(pt.y() + 12);
  p.setPen(QPen(QColor(Qt::black), 1.0));
  p.setBrush(QColor(0xff, 0xff, 0xd0, 235));
  p.drawRect(box);
  p.setPen(Qt::black);
  p.drawText(box.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft, tip);
}

void UlpPlot::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.fillRect(rect(), palette().base());

  const Box b = plotBox();
  if (b.rect.width() < 10 || b.rect.height() < 10)
    return;

  drawAxes(p, b);
  drawPoints(p, b);
  drawMarker(p, b);

  if (!result_) {
    p.setPen(palette().color(QPalette::Text));
    p.drawText(rect(), Qt::AlignCenter,
               QStringLiteral("Load a library and run a scan to see ULP error."));
  }
}

void UlpPlot::mousePressEvent(QMouseEvent *e) {
  const Box b = plotBox();
  if (e->button() == Qt::RightButton) {
    // Right-click backs out one level, so a deep zoom is escapable without
    // having to find the edge of the image and drag all the way back.
    resetView();
    return;
  }
  if (e->button() != Qt::LeftButton)
    return;

  dragging_ = true;
  dragStartPx_ = e->position().x();
  dragAnchorX_ = pxToX(e->position().x(), b);
  dragViewMin_ = b.xMin;
  dragViewMax_ = b.xMax;
  setCursor(Qt::ClosedHandCursor);
}

void UlpPlot::mouseReleaseEvent(QMouseEvent *e) {
  if (!dragging_)
    return;
  dragging_ = false;
  setCursor(Qt::CrossCursor);

  // Left-drag pans and plain-click selects, so the release only counts as a
  // click when the pointer barely moved.
  if (std::fabs(e->position().x() - dragStartPx_) < 3.0) {
    const Box b = plotBox();
    const int idx = nearestSample(e->position().x(), e->position().y(), b, 6.0);
    setSelectedIndex(idx);
    if (idx >= 0)
      emit samplePicked(idx);
  }
  update();
}

void UlpPlot::mouseDoubleClickEvent(QMouseEvent *e) {
  Q_UNUSED(e);
  resetView();
}

void UlpPlot::wheelEvent(QWheelEvent *e) {
  const Box b = plotBox();
  if (e->angleDelta().y() == 0)
    return;
  e->accept();

  // Zoom about the pointer, so the value under the cursor stays put.
  const double anchor = pxToX(e->position().x(), b);
  const double span = b.xMax - b.xMin;
  const double factor = e->angleDelta().y() > 0 ? 1.0 / 1.25 : 1.25;
  double newSpan = span * factor;

  // Do not zoom past a single input on either side, and never past the whole
  // input space.
  const double floorSpan = 4.0;
  const double dataSpan = result_ ? static_cast<double>(result_->config.fmt.inputCount())
                                  : 65536.0;
  if (newSpan < floorSpan)
    newSpan = floorSpan;
  if (newSpan > dataSpan * 1.05)
    newSpan = dataSpan * 1.05;

  const double frac = span > 0 ? (anchor - b.xMin) / span : 0.5;
  viewMin_ = anchor - frac * newSpan;
  viewMax_ = viewMin_ + newSpan;
  zoomed_ = true;
  update();
}

void UlpPlot::mouseMoveEvent(QMouseEvent *e) {
  const Box b = plotBox();

  if (dragging_) {
    // Move the whole window by however far the pointer travelled.
    const double now = pxToX(e->position().x(), b);
    const double delta = now - dragAnchorX_;
    if (delta != 0.0) {
      viewMin_ = dragViewMin_ - delta;
      viewMax_ = dragViewMax_ - delta;
      zoomed_ = true;
      update();
    }
    return;
  }

  const int idx = nearestSample(e->position().x(), e->position().y(), b, 6.0);
  if (idx != hovered_) {
    hovered_ = idx;
    update();
  }
  emit sampleHovered(idx);
}

void UlpPlot::leaveEvent(QEvent *e) {
  if (hovered_ != -1) {
    hovered_ = -1;
    update();
  }
  QWidget::leaveEvent(e);
}