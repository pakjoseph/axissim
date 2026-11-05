#include "charts.hpp"
#include "theme.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>

#include <algorithm>
#include <numeric>

EnergyBreakdownWidget::EnergyBreakdownWidget(QWidget* parent) : QWidget(parent) {
  // The empty-state explanation needs the same deliberate breathing room as
  // the populated ledger.  A preferred-height widget could otherwise be
  // compressed until its text visually meets the containing panel border.
  setMinimumHeight(184);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  setAccessibleName("Energy composition per frame chart");
  setToolTip("Modeled per-frame energy split by vector, arithmetic, memory movement, DMA, and static energy");
}

void EnergyBreakdownWidget::setSegments(QVector<EnergySegment> segments) {
  segments_ = std::move(segments);
  update();
}

void EnergyBreakdownWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  constexpr int kHorizontalInset = 10;
  const QRect content(kHorizontalInset, 0, std::max(0, width() - 2 * kHorizontalInset), height());
  painter.setPen(AxisTheme::textPrimary());
  QFont title_font = font();
  title_font.setPointSize(13);
  title_font.setWeight(QFont::DemiBold);
  painter.setFont(title_font);
  painter.drawText(QRect(content.left(), 0, content.width(), 24), Qt::AlignLeft | Qt::AlignVCenter, "Energy composition per frame");

  const double total = std::accumulate(segments_.cbegin(), segments_.cend(), 0.0,
                                       [](double sum, const EnergySegment& segment) { return sum + std::max(0.0, segment.value); });
  painter.setPen(AxisTheme::textSecondary());
  QFont total_font = font();
  total_font.setPointSize(11);
  painter.setFont(total_font);
  painter.drawText(QRect(content.left(), 0, content.width(), 24), Qt::AlignRight | Qt::AlignVCenter,
                   total > 0.0 ? QString("%1 µJ / frame").arg(total, 0, 'f', 2) : QString());
  if (total <= 0.0) {
    painter.setPen(AxisTheme::textSecondary());
    // Do not draw a chart track before there is data: this is an explanatory
    // empty state, not a zero-valued result.  Keep the copy inset and centered
    // in the available panel space so it cannot compete with a boundary.
    painter.drawText(QRect(content.left(), 42, content.width(), std::max(32, height() - 54)),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                     "Run a scenario to populate the modeled energy ledger.");
    return;
  }

  const QRectF track(content.left(), 42, content.width(), 22);
  painter.setPen(Qt::NoPen);
  painter.setBrush(AxisTheme::chartBackground());
  painter.drawRoundedRect(track, 7, 7);

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
  int legend_x = content.left();
  int legend_y = 88;
  const QFontMetrics metrics(legend_font);
  for (const auto& segment : segments_) {
    const QString text = QString("%1  %2 µJ  (%3%)")
                             .arg(segment.label)
                             .arg(segment.value, 0, 'f', 2)
                             .arg(100.0 * segment.value / total, 0, 'f', 1);
    const int item_width = 20 + metrics.horizontalAdvance(text) + 28;
    if (legend_x > content.left() && legend_x + item_width > content.right() + 1) {
      legend_x = content.left();
      legend_y += 28;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(segment.color);
    painter.drawRoundedRect(QRectF(legend_x, legend_y + 5, 10, 10), 3, 3);
    painter.setPen(AxisTheme::textSecondary());
    painter.drawText(QRect(legend_x + 18, legend_y, item_width - 18, 22), Qt::AlignLeft | Qt::AlignVCenter, text);
    legend_x += item_width;
  }
}

SweepChartWidget::SweepChartWidget(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(220);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setAccessibleName("RVV lane scaling chart");
  setToolTip("Modeled mean latency for each emitted RVV lane sweep point");
}

ResourceTimelineWidget::ResourceTimelineWidget(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(230);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  setAccessibleName("Frame zero resource timeline");
  setToolTip("Real modeled task trace on DMA and DRAM, RVV, and NPU resource lanes");
}

void ResourceTimelineWidget::setEvents(QVector<TimelineEvent> events) {
  events_ = std::move(events);
  update();
}

void ResourceTimelineWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  constexpr int kHorizontalInset = 10;
  const QRect content(kHorizontalInset, 0, std::max(0, width() - 2 * kHorizontalInset), height());
  QFont title_font = font();
  title_font.setPointSize(13);
  title_font.setWeight(QFont::DemiBold);
  painter.setFont(title_font);
  painter.setPen(AxisTheme::textPrimary());
  painter.drawText(QRect(content.left(), 0, content.width(), 26), Qt::AlignLeft | Qt::AlignVCenter, "Frame 0 resource pipeline");
  if (events_.isEmpty()) {
    painter.setPen(AxisTheme::textSecondary());
    painter.drawText(QRect(content.left(), 38, content.width(), height() - 38), Qt::AlignCenter,
                     "Run a scenario to trace DMA, RVV, and NPU work across the frame.");
    return;
  }
  const QStringList resources = {"DMA+DRAM", "RVV", "NPU"};
  const QStringList resource_labels = {"DMA / DRAM", "RVV", "NPU"};
  double end_us = 0.0;
  for (const auto& item : events_) end_us = std::max(end_us, item.end_us);
  painter.setPen(AxisTheme::textSecondary());
  QFont end_font = font();
  end_font.setPointSize(11);
  painter.setFont(end_font);
  painter.drawText(QRect(content.left(), 0, content.width(), 26), Qt::AlignRight | Qt::AlignVCenter,
                   QString("0 → %1 µs").arg(end_us, 0, 'f', 2));
  const QRectF plot(content.left() + 102, 38, std::max(10, content.width() - 118), std::max(10, height() - 76));
  const double lane_height = plot.height() / resources.size();
  QFont label_font = font();
  label_font.setPointSize(10);
  painter.setFont(label_font);
  for (int lane = 0; lane < resources.size(); ++lane) {
    const double y = plot.top() + lane * lane_height;
    painter.setPen(QPen(AxisTheme::border(), 1));
    painter.drawLine(QPointF(plot.left(), y + lane_height), QPointF(plot.right(), y + lane_height));
    painter.setPen(AxisTheme::textSecondary());
    painter.drawText(QRectF(content.left(), y, 92, lane_height), Qt::AlignRight | Qt::AlignVCenter, resource_labels[lane]);
  }
  for (const auto& item : events_) {
    const int lane = resources.indexOf(item.resource);
    if (lane < 0) continue;
    const double x = plot.left() + plot.width() * item.start_us / end_us;
    const double right = plot.left() + plot.width() * item.end_us / end_us;
    const QRectF bar(x, plot.top() + lane * lane_height + 10, std::max(2.0, right - x), lane_height - 20);
    painter.setPen(Qt::NoPen);
    painter.setBrush(item.color);
    painter.drawRoundedRect(bar, 3, 3);
    if (bar.width() > 110) {
      painter.setPen(AxisTheme::canvas());
      painter.drawText(bar.adjusted(6, 0, -6, 0), Qt::AlignCenter | Qt::TextSingleLine,
                       QString(item.task).replace('_', ' ') + QString(" · %1 µs").arg(item.end_us - item.start_us, 0, 'f', 1));
    }
  }
  painter.setPen(QPen(AxisTheme::border(), 1));
  painter.drawLine(QPointF(plot.left(), plot.bottom()), QPointF(plot.right(), plot.bottom()));
  for (int tick = 0; tick <= 4; ++tick) {
    const double ratio = static_cast<double>(tick) / 4.0;
    const double x = plot.left() + ratio * plot.width();
    painter.setPen(QPen(AxisTheme::border(), 1));
    painter.drawLine(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + 5));
    painter.setPen(AxisTheme::textSecondary());
    painter.drawText(QRectF(x - 30, plot.bottom() + 6, 60, 18), Qt::AlignCenter,
                     QString::number(ratio * end_us, 'f', 0));
  }
  painter.setPen(AxisTheme::textSecondary());
  painter.drawText(QRectF(plot.left(), plot.bottom() + 25, plot.width(), 18), Qt::AlignCenter, "Modeled frame time (µs)");
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
  painter.setPen(AxisTheme::textPrimary());
  QFont title_font = font();
  title_font.setPointSize(13);
  title_font.setWeight(QFont::DemiBold);
  painter.setFont(title_font);
  painter.drawText(QRect(0, 0, width(), 26), Qt::AlignLeft | Qt::AlignVCenter, "RVV lanes vs. modeled mean latency");

  if (points_.isEmpty()) {
    painter.setPen(AxisTheme::textSecondary());
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

  painter.setPen(QPen(AxisTheme::border(), 1));
  QFont axis_font = font();
  axis_font.setPointSize(9);
  painter.setFont(axis_font);
  for (int line = 0; line <= 4; ++line) {
    const double ratio = static_cast<double>(line) / 4.0;
    const double y = plot.bottom() - ratio * plot.height();
    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    painter.setPen(AxisTheme::textSecondary());
    painter.drawText(QRectF(0, y - 10, 50, 20), Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(y_min + ratio * (y_max - y_min), 'f', 0));
    painter.setPen(QPen(AxisTheme::border(), 1));
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

  painter.setPen(QPen(AxisTheme::accent(), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(path);
  for (qsizetype index = 0; index < plotted.size(); ++index) {
    painter.setPen(QPen(AxisTheme::canvas(), 2));
    painter.setBrush(AxisTheme::accent());
    painter.drawEllipse(plotted[index], 6, 6);
    painter.setPen(AxisTheme::textSecondary());
    painter.drawText(QRectF(plotted[index].x() - 28, plot.bottom() + 8, 56, 20), Qt::AlignCenter,
                     QString::number(points_[index].lanes, 'f', 0));
  }

  painter.save();
  painter.translate(13, plot.center().y());
  painter.rotate(-90);
  painter.setPen(AxisTheme::textSecondary());
  painter.drawText(QRectF(-70, -10, 140, 20), Qt::AlignCenter, "Latency (µs)");
  painter.restore();
  painter.setPen(AxisTheme::textSecondary());
  painter.drawText(QRectF(plot.left(), plot.bottom() + 28, plot.width(), 20), Qt::AlignCenter, "RVV lanes");
}
