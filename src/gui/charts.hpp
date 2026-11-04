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
