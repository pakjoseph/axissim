#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

struct EnergySegment {
  QString label;
  double value = 0.0;
  QColor color;
};

class EnergyBreakdownWidget final : public QWidget {
 public:
  explicit EnergyBreakdownWidget(QWidget* parent = nullptr);
  void setSegments(QVector<EnergySegment> segments);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QVector<EnergySegment> segments_;
};

struct SweepPoint {
  double lanes = 0.0;
  double latency_us = 0.0;
  double p99_latency_us = 0.0;
  double dram_traffic_mb = 0.0;
  double energy_per_frame_uj = 0.0;
};

struct TimelineEvent {
  QString task;
  QString resource;
  double start_us = 0.0;
  double end_us = 0.0;
  QColor color;
};

class ResourceTimelineWidget final : public QWidget {
 public:
  explicit ResourceTimelineWidget(QWidget* parent = nullptr);
  void setEvents(QVector<TimelineEvent> events);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QVector<TimelineEvent> events_;
};

class SweepChartWidget final : public QWidget {
 public:
  explicit SweepChartWidget(QWidget* parent = nullptr);
  void setPoints(QVector<SweepPoint> points);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QVector<SweepPoint> points_;
};
