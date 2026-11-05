#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

class QApplication;

namespace AxisTheme {

enum class Mode { System, Light, Dark };

void installFonts();
void apply(QApplication& application, Mode mode);
bool systemUsesDarkMode();
Mode modeFromKey(const QString& key);
QString modeKey(Mode mode);
QPalette palette(bool dark);
QString styleSheet(bool dark);

QColor chartDma();
QColor chartRvv();
QColor chartNpu();
QColor chartControl();
QColor chartBackground();
QColor textPrimary();
QColor textSecondary();
QColor canvas();
QColor border();
QColor accent();

}  // namespace AxisTheme
