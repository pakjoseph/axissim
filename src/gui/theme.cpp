#include "theme.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStyleHints>
#include <QtGlobal>

namespace AxisTheme {
namespace {

constexpr auto kCanvas = "#F2F1EC";
constexpr auto kSurface = "#FAFAF7";
constexpr auto kSurfaceMuted = "#E7E5DF";
constexpr auto kTextPrimary = "#202526";
constexpr auto kTextSecondary = "#5D6465";
constexpr auto kTextMuted = "#747B79";
constexpr auto kBorder = "#C9CDCA";
constexpr auto kBorderStrong = "#858D8D";
constexpr auto kAccent = "#C45132";
constexpr auto kAccentHover = "#9A3D25";
constexpr auto kAccentSoft = "#F2DFD7";
constexpr auto kChartDma = "#3F6F77";
constexpr auto kChartRvv = "#A94B31";
constexpr auto kChartNpu = "#465D83";
constexpr auto kChartControl = "#775B79";
constexpr auto kChartBackground = "#DEDFD9";
bool g_dark = false;

}  // namespace

void installFonts() {
  const QStringList font_resources = {
      ":/fonts/IBMPlexSans-Regular.ttf",
      ":/fonts/IBMPlexSans-Medium.ttf",
      ":/fonts/IBMPlexSans-SemiBold.ttf",
      ":/fonts/IBMPlexMono-Regular.ttf",
      ":/fonts/IBMPlexMono-Medium.ttf",
      ":/fonts/IBMPlexMono-SemiBold.ttf",
  };
  for (const auto& resource : font_resources) QFontDatabase::addApplicationFont(resource);
}

bool systemUsesDarkMode() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
  return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
#endif
}

Mode modeFromKey(const QString& key) {
  if (key.compare("dark", Qt::CaseInsensitive) == 0) return Mode::Dark;
  if (key.compare("light", Qt::CaseInsensitive) == 0) return Mode::Light;
  return Mode::System;
}

QString modeKey(Mode mode) {
  if (mode == Mode::Dark) return "dark";
  if (mode == Mode::Light) return "light";
  return "system";
}

void apply(QApplication& application, Mode mode) {
  const bool dark = mode == Mode::Dark || (mode == Mode::System && systemUsesDarkMode());
  g_dark = dark;
  application.setFont(QFont("IBM Plex Sans", 10));
  application.setPalette(palette(dark));
  application.setStyleSheet(styleSheet(dark));
}

QPalette palette(bool dark) {
  const QColor canvas(dark ? "#171A1D" : kCanvas);
  const QColor surface(dark ? "#22262A" : kSurface);
  const QColor surface_muted(dark ? "#2B3035" : kSurfaceMuted);
  const QColor text_primary(dark ? "#F1F3F4" : kTextPrimary);
  const QColor text_secondary(dark ? "#C0C8CE" : kTextSecondary);
  const QColor text_muted(dark ? "#9FAAB2" : kTextMuted);
  const QColor accent(dark ? "#46AAB0" : kAccent);
  QPalette result;
  result.setColor(QPalette::Window, canvas);
  result.setColor(QPalette::WindowText, text_primary);
  result.setColor(QPalette::Base, surface);
  result.setColor(QPalette::AlternateBase, surface_muted);
  result.setColor(QPalette::Text, text_primary);
  result.setColor(QPalette::Button, surface);
  result.setColor(QPalette::ButtonText, text_primary);
  result.setColor(QPalette::Highlight, accent);
  result.setColor(QPalette::HighlightedText, dark ? QColor("#0F1214") : QColor("#FFFFFF"));
  result.setColor(QPalette::PlaceholderText, text_muted);
  result.setColor(QPalette::Disabled, QPalette::Text, text_muted);
  result.setColor(QPalette::Disabled, QPalette::ButtonText, text_muted);
  return result;
}

QString styleSheet(bool dark) {
  QString sheet = QString::fromUtf8(R"QSS(
QWidget {
  background: @CANVAS@;
  color: @TEXT@;
  font-family: "IBM Plex Sans";
  font-size: 14px;
}
QMainWindow, QDialog { background: @CANVAS@; }
QLabel { background: transparent; }
QLabel[muted="true"] { color: @SECONDARY@; }
QLabel#appTitle { color: #FFFFFF; font-size: 24px; font-weight: 600; }
QLabel#appSubtitle { color: #C6D1D6; font-size: 12px; }
QLabel#pageTitle { color: @TEXT@; font-size: 24px; font-weight: 600; }
QLabel#sectionTitle { color: @TEXT@; font-size: 18px; font-weight: 600; }
QLabel#panelTitle { color: @TEXT@; font-size: 16px; font-weight: 600; }
QLabel#modeledLabel {
  background: @ACCENT_SOFT@;
  color: @ACCENT_TEXT@;
  border: 1px solid @ACCENT@;
  border-radius: 4px;
  padding: 4px 8px;
  font-size: 12px;
  font-weight: 600;
}
QLabel#statusText { color: #D6DEE3; font-size: 12px; font-weight: 600; }
QLabel#statusText[state="busy"] { color: #77CCD1; }
QLabel#statusText[state="success"] { color: #7DD7AF; }
QLabel#statusText[state="warning"] { color: #F0BF74; }
QLabel#statusText[state="error"] { color: #F0969A; }
QFrame#navRail { background: #151D26; border: none; border-right: 2px solid @BORDER_STRONG@; }
QFrame#headerPanel { background: #151D26; border: 2px solid @BORDER_STRONG@; border-radius: 8px; }
QFrame#commandPanel, QFrame#editorPanel, QFrame#contentPanel {
  background: @SURFACE@;
  border: 2px solid @BORDER_STRONG@;
  border-radius: 8px;
}
QFrame#metricStrip {
  background: @CANVAS@;
  border: 2px solid @BORDER_STRONG@;
  border-radius: 8px;
}
QFrame#metricPanel { background: @SURFACE@; border: 1px solid @BORDER@; border-radius: 5px; }
QFrame#metricDivider { background: @BORDER@; border: none; max-width: 1px; }
QFrame#tutorialPanel {
  background: @ACCENT_SOFT@;
  border: 2px solid @BORDER_STRONG@;
  border-radius: 8px;
}
QLabel#tutorialTitle { color: @TEXT@; font-size: 14px; font-weight: 600; }
QLabel#tutorialText { color: @SECONDARY@; font-size: 13px; }
QWidget[tutorialTarget="true"] { border: 2px solid @ACCENT@; border-radius: 6px; }
QLabel#metricTitle { color: @SECONDARY@; font-size: 12px; font-weight: 600; }
QLabel#metricValue {
  color: @TEXT@;
  font-family: "IBM Plex Mono";
  font-size: 26px;
  font-weight: 600;
}
QLabel#metricValue[compact="true"] { font-size: 18px; }
QLabel#metricTitle[compact="true"], QLabel#metricCaption[compact="true"] { font-size: 11px; }
QLabel#metricCaption { color: @MUTED@; font-size: 12px; }
QPushButton {
  background: @SURFACE@;
  border: 1px solid @BORDER_STRONG@;
  border-radius: 6px;
  color: @TEXT@;
  min-height: 20px;
  padding: 7px 12px;
  font-weight: 600;
}
QPushButton:hover { background: @HOVER@; border-color: @ACCENT@; }
QPushButton:pressed { background: @ACCENT_SOFT@; }
QPushButton:focus { border: 2px solid @ACCENT@; padding: 6px 11px; }
QPushButton:disabled { color: @MUTED@; background: @MUTED_SURFACE@; border-color: @BORDER@; }
QPushButton#primaryButton { background: @ACCENT@; border-color: @ACCENT@; color: @PRIMARY_TEXT@; }
QPushButton#primaryButton:hover { background: @ACCENT_HOVER@; border-color: @ACCENT_HOVER@; }
QPushButton#primaryButton:pressed { background: #0E555B; }
QPushButton[nav="true"] {
  background: transparent;
  border: none;
  border-radius: 6px;
  color: #C6D1D6;
  text-align: left;
  padding: 10px 12px;
  font-weight: 500;
}
QPushButton[nav="true"]:hover { background: #24313D; color: #FFFFFF; }
QPushButton[nav="true"]:checked { background: @ACCENT@; color: @PRIMARY_TEXT@; border: 1px solid #8BD2D6; font-weight: 600; }
QPushButton[nav="true"]:focus { border: 2px solid #5DB2B7; padding: 8px 10px; }
QComboBox, QDoubleSpinBox, QSpinBox, QLineEdit {
  background: @SURFACE@;
  border: 1px solid @BORDER_STRONG@;
  border-radius: 6px;
  color: @TEXT@;
  min-height: 20px;
  padding: 7px 10px;
}
QComboBox:hover, QDoubleSpinBox:hover, QSpinBox:hover, QLineEdit:hover { background: @HOVER@; border-color: @ACCENT@; }
QComboBox:focus, QDoubleSpinBox:focus, QSpinBox:focus, QLineEdit:focus { border: 2px solid @ACCENT@; padding: 6px 9px; }
QComboBox::drop-down { border: none; width: 26px; }
QComboBox QAbstractItemView { background: @SURFACE@; color: @TEXT@; border: 1px solid @BORDER_STRONG@; selection-background-color: @ACCENT@; selection-color: @PRIMARY_TEXT@; }
QPlainTextEdit {
  background: @SURFACE@;
  border: 1px solid @BORDER_STRONG@;
  border-radius: 6px;
  color: @TEXT@;
  selection-background-color: @ACCENT@;
  selection-color: @PRIMARY_TEXT@;
  font-family: "IBM Plex Mono";
  font-size: 13px;
  padding: 8px;
}
QPlainTextEdit:focus { border: 2px solid @ACCENT@; padding: 7px; }
QTabWidget::pane { border: 1px solid @BORDER_STRONG@; background: @SURFACE@; top: -1px; }
QTabBar::tab {
  background: @MUTED_SURFACE@;
  color: @SECONDARY@;
  padding: 10px 14px;
  border: 1px solid @BORDER@;
  border-bottom: 2px solid @BORDER_STRONG@;
  font-weight: 500;
}
QTabBar::tab:selected { background: @SURFACE@; color: @ACCENT_TEXT@; border: 2px solid @ACCENT@; border-bottom-color: @SURFACE@; font-weight: 600; }
QTabBar::tab:hover { color: @TEXT@; background: @HOVER@; }
QTabBar::tab:focus { border: 2px solid @ACCENT@; }
QTableWidget {
  background: @SURFACE@;
  alternate-background-color: @MUTED_SURFACE@;
  border: 1px solid @BORDER_STRONG@;
  border-radius: 6px;
  gridline-color: @BORDER@;
  selection-background-color: @ACCENT@;
  selection-color: @PRIMARY_TEXT@;
  font-family: "IBM Plex Mono";
  font-size: 12px;
}
QHeaderView::section {
  background: @MUTED_SURFACE@;
  color: @SECONDARY@;
  border: 1px solid @BORDER@;
  padding: 8px;
  font-family: "IBM Plex Sans";
  font-size: 12px;
  font-weight: 600;
}
QProgressBar { background: @CHART_BG@; border: 1px solid @BORDER@; border-radius: 2px; max-height: 5px; }
QProgressBar::chunk { background: @ACCENT@; border-radius: 2px; }
QSplitter::handle { background: @BORDER@; width: 10px; margin: 0 3px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @BORDER_STRONG@; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @ACCENT@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QStatusBar { background: @CANVAS@; color: @SECONDARY@; border-top: 1px solid @BORDER@; padding: 3px 8px; min-height: 22px; }
QToolTip { background: #151D26; color: #FFFFFF; border: 1px solid @BORDER_STRONG@; padding: 5px; }
QMenuBar { background: @CANVAS@; color: @TEXT@; border-bottom: 1px solid @BORDER@; }
QMenuBar::item:selected { background: @ACCENT@; color: @PRIMARY_TEXT@; }
QMenu { background: @SURFACE@; color: @TEXT@; border: 1px solid @BORDER_STRONG@; }
QMenu::item:selected { background: @ACCENT@; color: @PRIMARY_TEXT@; }
QFrame#workbenchCommandBar { background: #101211; border: none; border-bottom: 1px solid @BORDER@; }
QLabel#workbenchBrandMark {
  min-width: 26px; max-width: 26px; min-height: 26px; max-height: 26px;
  border: 1px solid @ACCENT@; border-radius: 3px; color: @ACCENT@;
  font-family: "IBM Plex Mono"; font-size: 11px; font-weight: 500;
  qproperty-alignment: AlignCenter;
}
QLabel#workbenchBrandName { color: #F4F3ED; font-size: 16px; font-weight: 600; }
QFrame#navRail { background: #1C1F1D; border: none; border-right: 1px solid @BORDER@; }
QLabel#appTitle, QLabel#appSubtitle { min-height: 0px; max-height: 0px; }
QPushButton[nav="true"] { color: @SECONDARY@; border-radius: 3px; padding: 9px 10px; }
QPushButton[nav="true"]:hover { background: @HOVER@; color: @TEXT@; }
QPushButton[nav="true"]:checked { background: @SURFACE@; color: @TEXT@; border: none; border-left: 3px solid @ACCENT@; padding-left: 7px; }
QFrame#contentPanel { background: transparent; border: 1px solid @BORDER_STRONG@; border-radius: 4px; }
QFrame#metricStrip { background: @MUTED_SURFACE@; border: 1px solid @BORDER_STRONG@; border-radius: 4px; }
QFrame#metricPanel { background: transparent; border: none; border-radius: 0; }
QFrame#metricPanel[measurement="true"][separated="true"] { border-left: 1px solid @BORDER@; }
QLabel#metricValue { font-size: 22px; }
QFrame#timelinePanel, QFrame#energyPanel { background: @SURFACE@; border: 1px solid @BORDER_STRONG@; border-radius: 4px; }
QFrame#findingsMargin { background: @SURFACE@; border: 1px solid @BORDER_STRONG@; border-radius: 4px; }
QFrame#scopePanel { background: @MUTED_SURFACE@; border: 1px solid @BORDER@; border-radius: 4px; }
QFrame#findingDivider { background: @BORDER_STRONG@; border: none; min-height: 1px; max-height: 1px; }
QLabel#findingText { color: @TEXT@; font-size: 14px; font-weight: 600; }
QLabel#evidenceLedger { color: @SECONDARY@; font-family: "IBM Plex Mono"; font-size: 12px; }
QFrame#editorPanel { border-radius: 0; border-left: none; }
QFrame#experimentLab { background: transparent; border: none; }
QFrame#experimentHeader { background: transparent; border: none; border-bottom: 1px solid @BORDER_STRONG@; }
QLabel#pageTitle { color: @TEXT@; font-size: 20px; font-weight: 600; }
QLabel#experimentKicker { color: @SECONDARY@; font-size: 13px; }
QLabel#experimentState { color: @SECONDARY@; font-family: "IBM Plex Mono"; font-size: 12px; }
QFrame#parameterNavigator { background: transparent; border: none; border-right: 1px solid @BORDER_STRONG@; }
QLabel#parameterGroup { color: @SECONDARY@; padding: 0 8px; font-size: 14px; }
QLabel#activeParameterGroup { background: @MUTED_SURFACE@; color: @TEXT@; border-left: 3px solid @ACCENT@; padding: 0 5px; font-size: 14px; font-weight: 600; }
QFrame#parameterEditor { background: transparent; border: none; }
QFrame#experimentDelta { background: transparent; border: none; border-left: 1px solid @BORDER_STRONG@; }
QFrame#experimentDivider { background: @BORDER_STRONG@; border: none; min-height: 1px; max-height: 1px; }
QFrame#experimentField { background: transparent; border: none; border-bottom: 1px solid @BORDER@; }
QLabel#fieldLabel { color: @TEXT@; font-size: 14px; font-weight: 600; }
QFrame#capacityCheck { background: transparent; border: none; border-bottom: 1px solid @BORDER@; }
QLabel#capacityValue, QLabel#deltaLedger { color: @TEXT@; font-family: "IBM Plex Mono"; font-size: 13px; }
QFrame#interpretationNote { background: transparent; border: none; border-top: 2px solid @ACCENT@; }
QPushButton#advancedManifestButton { text-align: left; font-weight: 500; }
)QSS");
  const QString canvas = dark ? "#171918" : kCanvas;
  const QString surface = dark ? "#202321" : kSurface;
  const QString muted_surface = dark ? "#292D2B" : kSurfaceMuted;
  const QString hover = dark ? "#2B302D" : "#E4EBEE";
  const QString text = dark ? "#F1F0EA" : kTextPrimary;
  const QString secondary = dark ? "#AEB4AF" : kTextSecondary;
  const QString muted = dark ? "#8A938E" : kTextMuted;
  const QString border = dark ? "#3E4441" : kBorder;
  const QString border_strong = dark ? "#535B56" : kBorderStrong;
  const QString accent = dark ? "#E0704E" : kAccent;
  const QString accent_hover = dark ? "#EE8A6B" : kAccentHover;
  const QString accent_soft = dark ? "#382A25" : kAccentSoft;
  const QString accent_text = dark ? "#F0A58D" : kAccentHover;
  const QString primary_text = dark ? "#171918" : "#FFFFFF";
  const QString chart_background = dark ? "#343A40" : kChartBackground;
  return sheet.replace("@CANVAS@", canvas)
      .replace("@SURFACE@", surface)
      .replace("@MUTED_SURFACE@", muted_surface)
      .replace("@HOVER@", hover)
      .replace("@TEXT@", text)
      .replace("@SECONDARY@", secondary)
      .replace("@MUTED@", muted)
      .replace("@BORDER_STRONG@", border_strong)
      .replace("@BORDER@", border)
      .replace("@ACCENT_HOVER@", accent_hover)
      .replace("@ACCENT_SOFT@", accent_soft)
      .replace("@ACCENT_TEXT@", accent_text)
      .replace("@ACCENT@", accent)
      .replace("@PRIMARY_TEXT@", primary_text)
      .replace("@CHART_BG@", chart_background);
}

QColor chartDma() { return QColor(g_dark ? "#46AAB0" : kChartDma); }
QColor chartRvv() { return QColor(g_dark ? "#E2A85B" : kChartRvv); }
QColor chartNpu() { return QColor(g_dark ? "#839BE2" : kChartNpu); }
QColor chartControl() { return QColor(g_dark ? "#C08AD1" : kChartControl); }
QColor chartBackground() { return QColor(g_dark ? "#343A40" : kChartBackground); }
QColor textPrimary() { return QColor(g_dark ? "#F1F3F4" : kTextPrimary); }
QColor textSecondary() { return QColor(g_dark ? "#C0C8CE" : kTextSecondary); }
QColor canvas() { return QColor(g_dark ? "#171A1D" : kCanvas); }
QColor border() { return QColor(g_dark ? "#59636B" : kBorder); }
QColor accent() { return QColor(g_dark ? "#46AAB0" : kAccent); }

}  // namespace AxisTheme
