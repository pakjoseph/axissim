#include "charts.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>

#include <algorithm>
#include <numeric>

namespace {

const QColor kText("#e6edf7");
const QColor kMuted("#8fa3bf");
const QColor kGrid("#27344d");
const QColor kTrack("#1a2438");
const QColor kAccent("#38bdf8");

}  // namespace

EnergyBreakdownWidget::EnergyBreakdownWidget(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(160);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void EnergyBreakdownWidget::setSegments(QVector<EnergySegment> segments) {
  segments_ = std::move(segments);
  update();
}

void EnergyBreakdownWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(kText);
  QFont title_font = font();
  title_font.setPointSize(13);
  title_font.setWeight(QFont::DemiBold);
  painter.setFont(title_font);
  painter.drawText(QRect(0, 0, width(), 24), Qt::AlignLeft | Qt::AlignVCenter, "Energy composition per frame");

  const double total = std::accumulate(segments_.cbegin(), segments_.cend(), 0.0,
                                       [](double sum, const EnergySegment& segment) { return sum + std::max(0.0, segment.value); });
  const QRectF track(0, 42, width(), 22);
  painter.setPen(Qt::NoPen);
  painter.setBrush(kTrack);
  painter.drawRoundedRect(track, 7, 7);
  if (total <= 0.0) {
    painter.setPen(kMuted);
    painter.drawText(QRect(0, 78, width(), 28), Qt::AlignLeft | Qt::AlignVCenter, "Run a scenario to populate the modeled energy ledger.");
    return;
  }

  double x = track.x();
  for (const auto& segment : segments_) {
    const double segment_width = track.width() * std::max(0.0, segment.value) / total;
    if (segment_width > 0.0) {
      painter.setBrush(segment.color);
      painter.drawRoundedRect(QRectF(x, track.y(), segment_width, track.height()), 5, 5);
    }
    x += segment_width;
  }

  QFont legend_font = font();
  legend_font.setPointSize(10);
  painter.setFont(legend_font);
  int legend_x = 0;
  int legend_y = 88;
  const QFontMetrics metrics(legend_font);
  for (const auto& segment : segments_) {
    const QString text = QString("%1  %2 µJ  (%3%)")
                             .arg(segment.label)
                             .arg(segment.value, 0, 'f', 2)
                             .arg(100.0 * segment.value / total, 0, 'f', 1);
    const int item_width = 20 + metrics.horizontalAdvance(text) + 28;
    if (legend_x > 0 && legend_x + item_width > width()) {
      legend_x = 0;
      legend_y += 28;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(segment.color);
    painter.drawRoundedRect(QRectF(legend_x, legend_y + 5, 10, 10), 3, 3);
    painter.setPen(kMuted);
    painter.drawText(QRect(legend_x + 18, legend_y, item_width - 18, 22), Qt::AlignLeft | Qt::AlignVCenter, text);
    legend_x += item_width;
  }
}

SweepChartWidget::SweepChartWidget(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(220);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SweepChartWidget::setPoints(QVector<SweepPoint> points) {
  points_ = std::move(points);
  std::sort(points_.begin(), points_.end(), [](const SweepPoint& left, const SweepPoint& right) { return left.lanes < right.lanes; });
  update();
}

void SweepChartWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(kText);
  QFont title_font = font();
  title_font.setPointSize(13);
  title_font.setWeight(QFont::DemiBold);
  painter.setFont(title_font);
  painter.drawText(QRect(0, 0, width(), 26), Qt::AlignLeft | Qt::AlignVCenter, "RVV lanes vs. modeled mean latency");

  if (points_.isEmpty()) {
    painter.setPen(kMuted);
    painter.drawText(QRect(0, 42, width(), height() - 42), Qt::AlignCenter, "Run the RVV lane sweep to compare scaling.");
    return;
  }

  const QRectF plot(58, 42, std::max(10, width() - 76), std::max(10, height() - 82));
  const auto [min_latency_it, max_latency_it] = std::minmax_element(
      points_.cbegin(), points_.cend(), [](const SweepPoint& left, const SweepPoint& right) { return left.latency_us < right.latency_us; });
  const double min_latency = min_latency_it->latency_us;
  const double max_latency = max_latency_it->latency_us;
  const double range = std::max(1.0, max_latency - min_latency);
  const double y_min = std::max(0.0, min_latency - range * 0.15);
  const double y_max = max_latency + range * 0.15;

  painter.setPen(QPen(kGrid, 1));
  QFont axis_font = font();
  axis_font.setPointSize(9);
  painter.setFont(axis_font);
  for (int line = 0; line <= 4; ++line) {
    const double ratio = static_cast<double>(line) / 4.0;
    const double y = plot.bottom() - ratio * plot.height();
    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    painter.setPen(kMuted);
    painter.drawText(QRectF(0, y - 10, 50, 20), Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(y_min + ratio * (y_max - y_min), 'f', 0));
    painter.setPen(QPen(kGrid, 1));
  }

  QPainterPath path;
  QVector<QPointF> plotted;
  for (qsizetype index = 0; index < points_.size(); ++index) {
    const double x_ratio = points_.size() == 1 ? 0.5 : static_cast<double>(index) / static_cast<double>(points_.size() - 1);
    const double y_ratio = (points_[index].latency_us - y_min) / (y_max - y_min);
    const QPointF point(plot.left() + x_ratio * plot.width(), plot.bottom() - y_ratio * plot.height());
    plotted.push_back(point);
    if (index == 0) path.moveTo(point); else path.lineTo(point);
  }

  painter.setPen(QPen(kAccent, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(path);
  for (qsizetype index = 0; index < plotted.size(); ++index) {
    painter.setPen(QPen(QColor("#0b1020"), 2));
    painter.setBrush(kAccent);
    painter.drawEllipse(plotted[index], 6, 6);
    painter.setPen(kMuted);
    painter.drawText(QRectF(plotted[index].x() - 28, plot.bottom() + 8, 56, 20), Qt::AlignCenter,
                     QString::number(points_[index].lanes, 'f', 0));
  }

  painter.save();
  painter.translate(13, plot.center().y());
  painter.rotate(-90);
  painter.setPen(kMuted);
  painter.drawText(QRectF(-70, -10, 140, 20), Qt::AlignCenter, "Latency (µs)");
  painter.restore();
  painter.setPen(kMuted);
  painter.drawText(QRectF(plot.left(), plot.bottom() + 28, plot.width(), 20), Qt::AlignCenter, "RVV lanes");
}
