#include "charts.hpp"
#include "theme.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyleHints>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QVector>
#include <QVBoxLayout>
#include <QWhatsThis>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

namespace {

QString requestedAppearanceKey() {
  const QString override = qEnvironmentVariable("AXISSIM_APPEARANCE_OVERRIDE").trimmed().toLower();
  if (override == "light" || override == "dark" || override == "system") return override;
  return QSettings().value("appearance/mode", "system").toString();
}

class KpiCard final : public QFrame {
 public:
  KpiCard(const QString& title, const QString& caption, QWidget* parent = nullptr) : QFrame(parent) {
    setObjectName("metricPanel");
    setAccessibleName(title + " modeled metric");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(4);
    auto* title_label = new QLabel(title, this);
    title_label->setObjectName("metricTitle");
    value_ = new QLabel("—", this);
    value_->setObjectName("metricValue");
    caption_ = new QLabel(caption, this);
    caption_->setObjectName("metricCaption");
    layout->addWidget(title_label);
    layout->addWidget(value_);
    layout->addWidget(caption_);
  }

  void setValue(const QString& value, const QString& caption = {}) {
    value_->setText(value);
    if (!caption.isEmpty()) caption_->setText(caption);
    setAccessibleDescription(value + ". " + caption_->text());
  }

 private:
  QLabel* value_ = nullptr;
  QLabel* caption_ = nullptr;
};

class AxisSimWindow final : public QMainWindow {
 public:
  explicit AxisSimWindow(bool exit_after_run = false, QString snapshot_path = {}, QSize snapshot_size = {},
                         bool tutorial_smoke_run = false, bool tutorial_preview = false)
      : snapshot_path_(std::move(snapshot_path)), exit_after_run_(exit_after_run), tutorial_smoke_run_(tutorial_smoke_run), tutorial_preview_(tutorial_preview) {
    project_root_ = findProjectRoot();
    setWindowTitle("AxisSim — AI Architecture Explorer");
    setWindowIcon(QIcon(":/icons/axissim.png"));
    resize(snapshot_size.isValid() ? snapshot_size : QSize(1'440, 920));
    setMinimumSize(1'100, 720);
    buildInterface();
    buildMenus();
    if (snapshot_path_.isEmpty()) restoreWindowState();
    loadScenario(scenario_->currentData().toString());
    refreshTutorialEntry();
    if (tutorial_preview_) {
      QTimer::singleShot(0, this, [this] {
        startTutorial(false);
        QTimer::singleShot(120, this, [this] {
          QFileInfo output(snapshot_path_);
          output.dir().mkpath(".");
          QCoreApplication::exit(grab().save(snapshot_path_) ? 0 : 1);
        });
      });
    } else if (tutorial_smoke_run_) {
      QTimer::singleShot(0, this, [this] {
        startTutorial();
        runPublicMnistProfile();
      });
    } else if (exit_after_run_) {
      QTimer::singleShot(0, this, [this] { runScenario(); });
    }
  }

  QStringList accessibilityIssues() const {
    QStringList issues;
    const auto widgets = findChildren<QWidget*>();
    for (auto* widget : widgets) {
      if (!widget->isVisible() || !widget->isEnabled() || widget->focusPolicy() == Qt::NoFocus) continue;
      const bool interactive = qobject_cast<QAbstractButton*>(widget) || qobject_cast<QComboBox*>(widget) ||
                               qobject_cast<QPlainTextEdit*>(widget) || qobject_cast<QTableWidget*>(widget) ||
                               qobject_cast<QTabWidget*>(widget);
      if (!interactive) continue;
      QString name = widget->accessibleName().trimmed();
      if (name.isEmpty()) {
        if (const auto* button = qobject_cast<QAbstractButton*>(widget)) name = button->text().remove('&').trimmed();
      }
      if (name.isEmpty()) issues << QString("%1 has no accessible name").arg(widget->metaObject()->className());
    }
    return issues;
  }

 protected:
  void closeEvent(QCloseEvent* event) override {
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/splitter", splitter_->saveState());
    QMainWindow::closeEvent(event);
  }

  void resizeEvent(QResizeEvent* event) override {
    QMainWindow::resizeEvent(event);
    if (!nav_rail_ || !header_title_) return;
    const bool compact = event->size().width() < 1'200;
    nav_rail_->setFixedWidth(compact ? 88 : 200);
    nav_layout_->setContentsMargins(compact ? 8 : 16, 20, compact ? 8 : 16, 16);
    nav_title_->setText(compact ? "AS" : "AxisSim");
    nav_subtitle_->setVisible(!compact);
    nav_scope_->setVisible(!compact);
    appearance_label_->setVisible(false);
    appearance_combo_->setVisible(false);
    header_subtitle_->setVisible(!compact);
    header_title_->setVisible(!compact);
    header_title_->setText("Architecture overview");
    scenario_->setMinimumWidth(compact ? 190 : 270);
    browse_button_->setText(compact ? "Open…" : "Open scenario…");
    run_button_->setText(compact ? "Run" : "Run baseline");
    mnist_button_->setText(compact ? "MNIST-12" : "Run public MNIST-12");
    for (qsizetype index = 0; index < nav_buttons_.size(); ++index) {
      nav_buttons_[index]->setText(compact ? nav_short_labels_[index] : nav_labels_[index]);
      nav_buttons_[index]->setToolTip(nav_labels_[index]);
    }
    for (auto* metric : metric_cards_) {
      for (auto* label : metric->findChildren<QLabel*>()) {
        label->setProperty("compact", compact);
        label->style()->unpolish(label);
        label->style()->polish(label);
      }
    }
  }

 private:
  enum class TutorialStep {
    Inactive,
    AwaitingPublicRun,
    ExplainingBaseline,
    AwaitingSensitivityRun,
    ExplainingTradeoff,
  };

  struct TutorialSnapshot {
    bool valid = false;
    double mean_latency_us = 0.0;
    double energy_per_frame_uj = 0.0;
    double arithmetic_energy_per_frame_uj = 0.0;
    QString dominant_component;
    double dominant_share_percent = 0.0;
  };

  static QLabel* mutedLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("muted", true);
    return label;
  }

  static void configureTable(QTableWidget* table) {
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setShowGrid(false);
    table->verticalHeader()->setVisible(false);
  }

  void buildInterface() {
    auto* root = new QWidget(this);
    auto* root_layout = new QVBoxLayout(root);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);
    auto* command_bar = new QFrame(root);
    command_bar->setObjectName("workbenchCommandBar");
    auto* command_layout = new QHBoxLayout(command_bar);
    command_layout->setContentsMargins(12, 8, 12, 8);
    command_layout->setSpacing(12);
    auto* brand = new QLabel("AX", command_bar);
    brand->setObjectName("workbenchBrandMark");
    brand->setAccessibleName("AxisSim");
    auto* brand_name = new QLabel("AxisSim", command_bar);
    brand_name->setObjectName("workbenchBrandName");
    command_layout->addWidget(brand);
    command_layout->addWidget(brand_name);
    command_layout->addStretch();
    root_layout->addWidget(command_bar);

    auto* shell = new QWidget(root);
    auto* shell_layout = new QHBoxLayout(shell);
    shell_layout->setContentsMargins(0, 0, 0, 0);
    shell_layout->setSpacing(0);

    nav_rail_ = new QFrame(root);
    nav_rail_->setObjectName("navRail");
    nav_rail_->setFixedWidth(200);
    nav_layout_ = new QVBoxLayout(nav_rail_);
    nav_layout_->setContentsMargins(16, 20, 16, 16);
    nav_layout_->setSpacing(6);
    nav_title_ = new QLabel("", nav_rail_);
    nav_title_->setObjectName("appTitle");
    nav_subtitle_ = new QLabel("", nav_rail_);
    nav_subtitle_->setObjectName("appSubtitle");
    nav_layout_->addWidget(nav_title_);
    nav_layout_->addWidget(nav_subtitle_);
    nav_layout_->addSpacing(8);
    auto* nav_group = new QButtonGroup(nav_rail_);
    nav_group->setExclusive(true);
    nav_labels_ = {"Overview", "Experiment Lab", "Compare", "Evidence"};
    nav_short_labels_ = {"OV", "EX", "CP", "EV"};
    for (qsizetype index = 0; index < nav_labels_.size(); ++index) {
      auto* button = new QPushButton(nav_labels_[index], nav_rail_);
      button->setProperty("nav", true);
      button->setCheckable(true);
      button->setAccessibleName(nav_labels_[index] + " view");
      nav_group->addButton(button, static_cast<int>(index));
      nav_buttons_.push_back(button);
      nav_layout_->addWidget(button);
    }
    nav_buttons_.front()->setChecked(true);
    nav_layout_->addStretch();
    appearance_label_ = new QLabel("Appearance", nav_rail_);
    appearance_label_->setObjectName("appSubtitle");
    appearance_combo_ = new QComboBox(nav_rail_);
    appearance_combo_->addItem("System", "system");
    appearance_combo_->addItem("Light", "light");
    appearance_combo_->addItem("Dark", "dark");
    appearance_combo_->setAccessibleName("Application appearance");
    appearance_combo_->setToolTip("Follow the operating system or choose an AxisSim theme");
    const QString saved_appearance = requestedAppearanceKey();
    const int saved_appearance_index = appearance_combo_->findData(saved_appearance);
    appearance_combo_->setCurrentIndex(saved_appearance_index < 0 ? 0 : saved_appearance_index);
    nav_layout_->addWidget(appearance_label_);
    nav_layout_->addWidget(appearance_combo_);
    nav_layout_->addSpacing(12);
    nav_scope_ = new QLabel("modeled estimate\nschema 1.2", nav_rail_);
    nav_scope_->setObjectName("appSubtitle");
    nav_scope_->setWordWrap(true);
    nav_scope_->setMinimumHeight(36);
    nav_layout_->addWidget(nav_scope_);
    shell_layout->addWidget(nav_rail_);

    auto* workspace = new QWidget(root);
    auto* layout = new QVBoxLayout(workspace);
    layout->setContentsMargins(24, 20, 24, 14);
    layout->setSpacing(12);

    auto* header = new QFrame(workspace);
    header->setObjectName("headerPanel");
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(20, 12, 16, 12);
    header_layout->setSpacing(12);
    auto* title_layout = new QVBoxLayout();
    title_layout->setSpacing(2);
    header_title_ = new QLabel("Architecture overview", header);
    header_title_->setObjectName("appTitle");
    header_subtitle_ = new QLabel("Configure a scenario, run the model, and trace each conclusion to its assumptions.", header);
    header_subtitle_->setObjectName("appSubtitle");
    title_layout->addWidget(header_title_);
    title_layout->addWidget(header_subtitle_);
    header_layout->addLayout(title_layout);
    header_layout->addStretch();
    scenario_ = new QComboBox(header);
    scenario_->setMinimumWidth(270);
    scenario_->addItem("Baseline Physical AI · model 0.2", project_root_.filePath("configs/baseline/manifest.conf"));
    scenario_->addItem("Public MNIST-12 profile", project_root_.filePath("configs/workloads/mnist12_profiled.conf"));
    scenario_->addItem("Fixed power-budget study", project_root_.filePath("configs/power_budget/manifest.conf"));
    scenario_->addItem("Deployment & implementation proxies", project_root_.filePath("configs/implementation_proxy/manifest.conf"));
    scenario_->setToolTip("Choose a checked-in scenario or open your own manifest");
    scenario_->setAccessibleName("Scenario");
    header_layout->addWidget(scenario_);
    browse_button_ = new QPushButton("Open scenario…", header);
    browse_button_->setAccessibleName("Open scenario file");
    header_layout->addWidget(browse_button_);
    progress_ = new QProgressBar(header);
    progress_->setRange(0, 0);
    progress_->setFixedWidth(84);
    progress_->setTextVisible(false);
    progress_->hide();
    status_pill_ = new QLabel("Ready", header);
    status_pill_->setObjectName("statusText");
    status_pill_->setProperty("state", "idle");
    status_pill_->setToolTip("Current simulator activity");
    status_pill_->setAccessibleName("Simulator status");
    header_layout->addWidget(progress_);
    header_layout->addWidget(status_pill_);
    run_button_ = new QPushButton("Run baseline", header);
    run_button_->setObjectName("primaryButton");
    run_button_->setToolTip("Run the selected scenario with any explicit quick overrides (⌘R)");
    run_button_->setAccessibleName("Run model");
    header_layout->addWidget(run_button_);
    mnist_button_ = new QPushButton("Run public MNIST-12", header);
    mnist_button_->setToolTip("Load the checked-in public MNIST-12 operator profile and run the deterministic model with scenario defaults");
    mnist_button_->setAccessibleName("Run public MNIST-12 example");
    mnist_button_->setWhatsThis("Runs the checked-in MNIST-12 operator profile with no quick overrides. The output is a modeled estimate with public-workload evidence available in Model & Evidence.");
    header_layout->addWidget(mnist_button_);
    layout->addWidget(header);

    tutorial_panel_ = new QFrame(workspace);
    tutorial_panel_->setObjectName("tutorialPanel");
    tutorial_panel_->setAccessibleName("Guided first run");
    auto* tutorial_layout = new QHBoxLayout(tutorial_panel_);
    tutorial_layout->setContentsMargins(16, 10, 12, 10);
    tutorial_layout->setSpacing(12);
    auto* tutorial_copy = new QVBoxLayout();
    tutorial_copy->setSpacing(2);
    tutorial_title_ = new QLabel(tutorial_panel_);
    tutorial_title_->setObjectName("tutorialTitle");
    tutorial_text_ = new QLabel(tutorial_panel_);
    tutorial_text_->setObjectName("tutorialText");
    tutorial_text_->setWordWrap(true);
    tutorial_copy->addWidget(tutorial_title_);
    tutorial_copy->addWidget(tutorial_text_);
    tutorial_layout->addLayout(tutorial_copy, 1);
    tutorial_action_button_ = new QPushButton(tutorial_panel_);
    tutorial_action_button_->setObjectName("primaryButton");
    tutorial_action_button_->setAccessibleName("Continue guided first run");
    tutorial_layout->addWidget(tutorial_action_button_);
    tutorial_skip_button_ = new QPushButton("Skip", tutorial_panel_);
    tutorial_skip_button_->setAccessibleName("Skip guided first run");
    tutorial_layout->addWidget(tutorial_skip_button_);
    tutorial_panel_->hide();
    layout->addWidget(tutorial_panel_);

    auto* controls = new QFrame(workspace);
    quick_controls_ = controls;
    controls->setObjectName("commandPanel");
    auto* controls_layout = new QGridLayout(controls);
    controls_layout->setContentsMargins(16, 12, 16, 12);
    controls_layout->setSpacing(8);
    auto* experiment_label = mutedLabel("Quick experiment", controls);
    experiment_label->setAccessibleName("Quick experiment overrides");
    controls_layout->addWidget(experiment_label, 0, 0);
    rvv_lanes_ = makeOverrideCombo("RVV lanes", {"2", "4", "8", "16", "32"}, controls);
    npu_macs_ = makeOverrideCombo("NPU MACs", {"256", "512", "1024", "2048", "4096"}, controls);
    dram_bandwidth_ = makeOverrideCombo("DRAM GB/s", {"0.15", "0.5", "1", "5", "20"}, controls);
    arithmetic_scale_ = new QComboBox(controls);
    arithmetic_scale_->addItem("Arithmetic: scenario default");
    arithmetic_scale_->addItem("Arithmetic: baseline", 1.0);
    arithmetic_scale_->addItem("Arithmetic: 2× lower energy", 0.5);
    arithmetic_scale_->addItem("Arithmetic: 3× lower energy", 0.333333);
    arithmetic_scale_->addItem("Arithmetic: 4× lower energy", 0.25);
    arithmetic_scale_->setToolTip("Changes NPU arithmetic energy only; latency and frequency remain fixed");
    arithmetic_scale_->setAccessibleName("NPU arithmetic energy scale");
    controls_layout->addWidget(rvv_lanes_, 0, 1);
    controls_layout->addWidget(npu_macs_, 0, 2);
    controls_layout->addWidget(dram_bandwidth_, 0, 3);
    controls_layout->addWidget(arithmetic_scale_, 0, 4);
    controls_layout->setColumnStretch(5, 1);
    save_button_ = new QPushButton("Save As…", controls);
    export_button_ = new QPushButton("Export", controls);
    export_button_->setEnabled(false);
    sweep_button_ = new QPushButton("Lane sweep", controls);
    ara_button_ = new QPushButton("Ara2 check", controls);
    save_button_->setAccessibleName("Save scenario as");
    export_button_->setAccessibleName("Export result JSON");
    sweep_button_->setAccessibleName("Run RVV lane sweep");
    ara_button_->setAccessibleName("Check published Ara2 reference point");
    controls_layout->addWidget(save_button_, 1, 1);
    controls_layout->addWidget(export_button_, 1, 2);
    controls_layout->addWidget(sweep_button_, 1, 3);
    controls_layout->addWidget(ara_button_, 1, 4);
    scenario_->setParent(command_bar);
    export_button_->setParent(command_bar);
    run_button_->setParent(command_bar);
    command_layout->addWidget(scenario_);
    command_layout->addWidget(export_button_);
    command_layout->addWidget(run_button_);
    header->hide();
    controls->hide();
    layout->addWidget(controls);

    splitter_ = new QSplitter(Qt::Horizontal, workspace);
    auto* editor_panel = new QFrame(splitter_);
    experiment_panel_ = editor_panel;
    editor_panel->setObjectName("editorPanel");
    editor_panel->setMinimumWidth(280);
    editor_panel->setMaximumWidth(360);
    auto* editor_layout = new QVBoxLayout(editor_panel);
    editor_layout->setContentsMargins(16, 16, 16, 16);
    editor_layout->setSpacing(8);
    auto* editor_title = new QLabel("Experiment configuration", editor_panel);
    editor_title->setObjectName("panelTitle");
    scenario_path_label_ = mutedLabel("", editor_panel);
    editor_layout->addWidget(editor_title);
    editor_layout->addWidget(scenario_path_label_);
    auto* editor_help = mutedLabel("Expert manifest editor. Quick experiment controls apply only to the next run and never rewrite this file.", editor_panel);
    editor_help->setWordWrap(true);
    editor_layout->addWidget(editor_help);
    editor_ = new QPlainTextEdit(editor_panel);
    editor_->setPlaceholderText("Open a scenario manifest to edit it.");
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setAccessibleName("Raw scenario manifest editor");
    editor_->setAccessibleDescription("Advanced key-value configuration used as the base for the next simulation run");
    editor_layout->addWidget(editor_, 1);

    auto* content_panel = new QFrame(splitter_);
    content_panel->setObjectName("contentPanel");
    auto* content_layout = new QVBoxLayout(content_panel);
    content_layout->setContentsMargins(16, 16, 16, 16);
    content_layout->setSpacing(10);
    auto* results_header = new QHBoxLayout();
    auto* results_title = new QLabel("Architecture overview", content_panel);
    results_title->setObjectName("sectionTitle");
    result_context_ = mutedLabel("No result yet. Run the selected scenario to populate modeled metrics.", content_panel);
    result_context_->setWordWrap(true);
    tutorial_start_button_ = new QPushButton("Guided first run", content_panel);
    tutorial_start_button_->setToolTip("Run a public workload and inspect one real architecture tradeoff in four short steps");
    tutorial_start_button_->setAccessibleName("Start guided first run");
    auto* modeled_label = new QLabel("Modeled estimate", content_panel);
    modeled_label->setObjectName("modeledLabel");
    modeled_label->setToolTip("Outputs depend on explicit model assumptions and are not measured silicon values");
    auto* title_stack = new QVBoxLayout();
    title_stack->setSpacing(1);
    title_stack->addWidget(result_context_);
    title_stack->addWidget(results_title);
    results_header->addLayout(title_stack, 1);
    results_header->addWidget(tutorial_start_button_);
    results_header->addWidget(modeled_label);
    content_layout->addLayout(results_header);

    auto* metric_strip = new QFrame(content_panel);
    metric_strip->setObjectName("metricStrip");
    metric_layout_ = new QGridLayout(metric_strip);
    metric_layout_->setContentsMargins(12, 10, 12, 10);
    metric_layout_->setSpacing(0);
    latency_card_ = new KpiCard("Mean latency", "End-to-end per frame", content_panel);
    deadline_card_ = new KpiCard("Deadline margin", "Configured vision deadline", content_panel);
    energy_card_ = new KpiCard("IT energy / frame", "Modeled estimate", content_panel);
    traffic_card_ = new KpiCard("Dominant energy", "Largest modeled component", content_panel);
    const std::array<KpiCard*, 4> metrics = {latency_card_, deadline_card_, energy_card_, traffic_card_};
    for (std::size_t index = 0; index < metrics.size(); ++index) {
      metrics[index]->setProperty("measurement", true);
      metrics[index]->setProperty("separated", index > 0);
      metric_cards_.push_back(metrics[index]);
      metric_layout_->addWidget(metrics[index], 0, static_cast<int>(index));
      metric_layout_->setColumnStretch(static_cast<int>(index), 1);
    }
    content_layout->addWidget(metric_strip);

    result_views_ = new QTabWidget(content_panel);
    result_views_->setAccessibleName("Result views");
    result_views_->tabBar()->hide();
    auto* overview_page = new QWidget(result_views_);
    auto* overview_layout = new QHBoxLayout(overview_page);
    overview_layout->setContentsMargins(0, 4, 0, 0);
    overview_layout->setSpacing(22);
    auto* analysis = new QWidget(overview_page);
    auto* analysis_layout = new QVBoxLayout(analysis);
    analysis_layout->setContentsMargins(0, 0, 0, 0);
    analysis_layout->setSpacing(18);
    auto* timeline_panel = new QFrame(analysis);
    timeline_panel->setObjectName("timelinePanel");
    auto* timeline_panel_layout = new QVBoxLayout(timeline_panel);
    timeline_panel_layout->setContentsMargins(12, 10, 12, 10);
    timeline_chart_ = new ResourceTimelineWidget(timeline_panel);
    timeline_panel_layout->addWidget(timeline_chart_);
    analysis_layout->addWidget(timeline_panel, 3);
    auto* energy_panel = new QFrame(analysis);
    energy_panel->setObjectName("energyPanel");
    auto* energy_panel_layout = new QVBoxLayout(energy_panel);
    energy_panel_layout->setContentsMargins(12, 10, 12, 10);
    energy_chart_ = new EnergyBreakdownWidget(energy_panel);
    energy_panel_layout->addWidget(energy_chart_);
    analysis_layout->addWidget(energy_panel, 1);
    metrics_ = new QTableWidget(overview_page);
    metrics_->setColumnCount(3);
    metrics_->setHorizontalHeaderLabels({"Metric", "Modeled value", "Interpretation"});
    metrics_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    metrics_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    metrics_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    configureTable(metrics_);
    metrics_->setAccessibleName("Modeled result details");
    metrics_->hide();
    auto* scope_note = mutedLabel("Model outputs are scenario-dependent estimates. Power, area, thermal, deployment, and reliability values are only as valid as their explicit input assumptions.", overview_page);
    scope_note->setWordWrap(true);
    auto* scope_panel = new QFrame(analysis);
    scope_panel->setObjectName("scopePanel");
    auto* scope_layout = new QVBoxLayout(scope_panel);
    scope_layout->setContentsMargins(12, 8, 12, 8);
    scope_layout->addWidget(scope_note);
    analysis_layout->addWidget(scope_panel);
    overview_layout->addWidget(analysis, 3);
    auto* findings = new QFrame(overview_page);
    findings->setObjectName("findingsMargin");
    findings->setMinimumWidth(250);
    findings->setMaximumWidth(340);
    auto* findings_layout = new QVBoxLayout(findings);
    findings_layout->setContentsMargins(16, 12, 16, 12);
    auto* findings_title = new QLabel("What limits this run", findings);
    findings_title->setObjectName("panelTitle");
    findings_layout->addWidget(findings_title);
    latency_finding_ = new QLabel("Run a scenario to identify the longest serialized stage.", findings);
    latency_finding_->setObjectName("findingText");
    latency_finding_->setWordWrap(true);
    findings_layout->addWidget(latency_finding_);
    auto* latency_scope = mutedLabel("Timing is an analytical model under the selected workload and configuration assumptions.", findings);
    latency_scope->setWordWrap(true);
    findings_layout->addWidget(latency_scope);
    auto* divider = new QFrame(findings);
    divider->setObjectName("findingDivider");
    divider->setFrameShape(QFrame::HLine);
    findings_layout->addWidget(divider);
    energy_finding_ = new QLabel("Run a scenario to identify the modeled energy driver.", findings);
    energy_finding_->setObjectName("findingText");
    energy_finding_->setWordWrap(true);
    findings_layout->addWidget(energy_finding_);
    auto* energy_scope = mutedLabel("Energy is assumption-led. Component changes should be evaluated at whole-system scope.", findings);
    energy_scope->setWordWrap(true);
    findings_layout->addWidget(energy_scope);
    findings_layout->addStretch();
    auto* evidence = new QLabel("Timing\tanalytical\nEnergy\tassumption-led\nRVV envelope\tAra checked\nSilicon claim\tnone", findings);
    evidence->setObjectName("evidenceLedger");
    evidence->setWordWrap(true);
    findings_layout->addWidget(evidence);
    overview_layout->addWidget(findings, 1);
    result_views_->addTab(overview_page, "Overview");

    auto* timeline_page = new QWidget(result_views_);
    auto* timeline_layout = new QVBoxLayout(timeline_page);
    timeline_layout->setContentsMargins(12, 12, 12, 12);
    timeline_layout->setSpacing(10);
    trace_ = new QTableWidget(timeline_page);
    trace_->setColumnCount(5);
    trace_->setHorizontalHeaderLabels({"Frame", "Task", "Resource", "Start (µs)", "Duration (µs)"});
    trace_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    configureTable(trace_);
    trace_->setAccessibleName("Frame zero task trace");
    timeline_layout->addWidget(trace_, 1);
    result_views_->addTab(timeline_page, "Timeline");

    auto* sweep_page = new QWidget(result_views_);
    auto* sweep_layout = new QVBoxLayout(sweep_page);
    comparison_summary_ = mutedLabel("Run an RVV lane sweep to compare candidates against the selected baseline.", sweep_page);
    comparison_summary_->setWordWrap(true);
    sweep_layout->addWidget(comparison_summary_);
    sweep_chart_ = new SweepChartWidget(sweep_page);
    sweep_layout->addWidget(sweep_chart_, 2);
    sweep_ = new QTableWidget(sweep_page);
    sweep_->setColumnCount(6);
    sweep_->setHorizontalHeaderLabels({"Candidate", "RVV lanes", "Mean (µs)", "Δ vs. 8 lanes", "DRAM (MB)", "Energy/frame (µJ)"});
    sweep_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    configureTable(sweep_);
    sweep_->setAccessibleName("RVV lane sweep results");
    sweep_layout->addWidget(sweep_, 1);
    result_views_->addTab(sweep_page, "Candidate comparison");

    auto* proxies_page = new QWidget(result_views_);
    auto* proxies_layout = new QVBoxLayout(proxies_page);
    auto* proxies_note = mutedLabel("Bounded sensitivity proxies only—not floorplanning, thermal signoff, or predicted silicon reliability.", proxies_page);
    proxies_note->setWordWrap(true);
    proxies_layout->addWidget(proxies_note);
    profile_evidence_ = mutedLabel("No public workload profile is selected. Profile evidence appears here when a profiled-graph scenario runs.", proxies_page);
    profile_evidence_->setWordWrap(true);
    profile_evidence_->setObjectName("profileEvidence");
    proxies_layout->addWidget(profile_evidence_);
    proxies_ = new QTableWidget(proxies_page);
    proxies_->setColumnCount(3);
    proxies_->setHorizontalHeaderLabels({"Proxy", "Modeled value", "Scope / status"});
    proxies_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    proxies_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    proxies_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    configureTable(proxies_);
    proxies_->setAccessibleName("Implementation sensitivity proxies");
    proxies_layout->addWidget(proxies_);
    result_views_->addTab(proxies_page, "Implementation proxies");

    log_ = new QPlainTextEdit(result_views_);
    log_->setReadOnly(true);
    log_->setAccessibleName("Complete simulator run log");
    log_->setPlaceholderText("Run a scenario to inspect the complete CLI output and validation notes.");
    log_tab_index_ = result_views_->addTab(log_, "Run log");
    content_layout->addWidget(result_views_, 1);

    splitter_->addWidget(editor_panel);
    splitter_->addWidget(content_panel);
    splitter_->setStretchFactor(0, 2);
    splitter_->setStretchFactor(1, 5);
    splitter_->setSizes({0, 1140});
    layout->addWidget(splitter_, 1);
    shell_layout->addWidget(workspace, 1);
    root_layout->addWidget(shell, 1);
    setCentralWidget(root);

    connect(scenario_, &QComboBox::currentIndexChanged, this, [this](int index) { changeScenario(index); });
    connect(browse_button_, &QPushButton::clicked, this, [this] { openScenario(); });
    connect(save_button_, &QPushButton::clicked, this, [this] { saveAs(); });
    connect(export_button_, &QPushButton::clicked, this, [this] { exportResult(); });
    connect(run_button_, &QPushButton::clicked, this, [this] { runScenario(); });
    connect(mnist_button_, &QPushButton::clicked, this, [this] { runPublicMnistProfile(); });
    connect(tutorial_start_button_, &QPushButton::clicked, this, [this] { startTutorial(); });
    connect(tutorial_action_button_, &QPushButton::clicked, this, [this] { advanceTutorial(); });
    connect(tutorial_skip_button_, &QPushButton::clicked, this, [this] { dismissTutorial(); });
    connect(sweep_button_, &QPushButton::clicked, this, [this] { runLaneSweep(); });
    connect(ara_button_, &QPushButton::clicked, this, [this] { runAra2Check(); });
    connect(appearance_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
      const QString key = appearance_combo_->itemData(index).toString();
      QSettings().setValue("appearance/mode", key);
      AxisTheme::apply(*qApp, AxisTheme::modeFromKey(key));
      for (auto* chart : {static_cast<QWidget*>(energy_chart_), static_cast<QWidget*>(sweep_chart_)}) chart->update();
    });
    connect(nav_group, &QButtonGroup::idClicked, this, [this](int id) {
      if (id == 1) {
        quick_controls_->show();
        splitter_->setSizes({320, 820});
        editor_->setFocus();
        return;
      }
      quick_controls_->hide();
      splitter_->setSizes({0, 1140});
      const std::array<int, 4> tab_for_nav = {0, 0, 2, 3};
      if (id >= 0 && id < static_cast<int>(tab_for_nav.size())) {
        result_views_->setCurrentIndex(tab_for_nav[static_cast<std::size_t>(id)]);
      }
    });
    connect(result_views_, &QTabWidget::currentChanged, this, [this](int index) {
      const std::array<int, 5> nav_for_tab = {0, 0, 2, 3, 3};
      const int nav_index = index >= 0 && index < static_cast<int>(nav_for_tab.size())
                                ? nav_for_tab[static_cast<std::size_t>(index)]
                                : 0;
      if (nav_index >= 0 && nav_index < nav_buttons_.size()) nav_buttons_[nav_index]->setChecked(true);
    });

    setTabOrder(scenario_, browse_button_);
    setTabOrder(browse_button_, run_button_);
    setTabOrder(run_button_, mnist_button_);
    setTabOrder(mnist_button_, tutorial_start_button_);
    setTabOrder(tutorial_start_button_, tutorial_action_button_);
    setTabOrder(tutorial_action_button_, tutorial_skip_button_);
    setTabOrder(tutorial_skip_button_, rvv_lanes_);
    setTabOrder(rvv_lanes_, npu_macs_);
    setTabOrder(npu_macs_, dram_bandwidth_);
    setTabOrder(dram_bandwidth_, arithmetic_scale_);
    setTabOrder(arithmetic_scale_, save_button_);
    setTabOrder(save_button_, export_button_);
    setTabOrder(export_button_, sweep_button_);
    setTabOrder(sweep_button_, ara_button_);
    setTabOrder(ara_button_, appearance_combo_);
    setTabOrder(appearance_combo_, editor_);
    setTabOrder(editor_, result_views_);
  }

  void buildMenus() {
    auto* file_menu = menuBar()->addMenu("File");
    open_action_ = file_menu->addAction("Open Scenario…");
    open_action_->setShortcut(QKeySequence::Open);
    connect(open_action_, &QAction::triggered, this, [this] { openScenario(); });
    save_action_ = file_menu->addAction("Save Scenario As…");
    save_action_->setShortcut(QKeySequence::SaveAs);
    connect(save_action_, &QAction::triggered, this, [this] { saveAs(); });
    export_action_ = file_menu->addAction("Export Result JSON…");
    export_action_->setShortcut(QKeySequence("Ctrl+E"));
    export_action_->setEnabled(false);
    connect(export_action_, &QAction::triggered, this, [this] { exportResult(); });
    file_menu->addSeparator();
    auto* quit_action = file_menu->addAction("Quit AxisSim");
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered, this, &QWidget::close);

    auto* run_menu = menuBar()->addMenu("Run");
    run_action_ = run_menu->addAction("Run Model");
    run_action_->setShortcut(QKeySequence("Ctrl+R"));
    connect(run_action_, &QAction::triggered, this, [this] { runScenario(); });
    mnist_action_ = run_menu->addAction("Run Public MNIST-12 Example");
    connect(mnist_action_, &QAction::triggered, this, [this] { runPublicMnistProfile(); });
    sweep_action_ = run_menu->addAction("Sweep RVV Lanes");
    sweep_action_->setShortcut(QKeySequence("Ctrl+Shift+R"));
    connect(sweep_action_, &QAction::triggered, this, [this] { runLaneSweep(); });
    ara_action_ = run_menu->addAction("Check Published Ara2 Point");
    connect(ara_action_, &QAction::triggered, this, [this] { runAra2Check(); });

    auto* help_menu = menuBar()->addMenu("Help");
    auto* appearance_menu = help_menu->addMenu("Appearance");
    for (const auto& mode : {QString("System"), QString("Light"), QString("Dark")}) {
      auto* action = appearance_menu->addAction(mode);
      connect(action, &QAction::triggered, this, [this, mode] {
        const int index = appearance_combo_->findText(mode);
        if (index >= 0) appearance_combo_->setCurrentIndex(index);
      });
    }
    help_menu->addSeparator();
    tutorial_start_action_ = help_menu->addAction("Start Guided First Run");
    connect(tutorial_start_action_, &QAction::triggered, this, [this] { startTutorial(); });
    tutorial_reset_action_ = help_menu->addAction("Reset Guided First Run");
    connect(tutorial_reset_action_, &QAction::triggered, this, [this] { resetTutorial(); });
    auto* context_help_action = QWhatsThis::createAction(this);
    context_help_action->setText("Context Help");
    help_menu->addAction(context_help_action);
    help_menu->addSeparator();
    auto* about_action = help_menu->addAction("About AxisSim");
    connect(about_action, &QAction::triggered, this, [this] {
      QMessageBox::about(this, "About AxisSim",
                         "<h3>AxisSim</h3><p>A workload-driven AI architecture, power, and implementation sensitivity explorer.</p>"
                         "<p>AxisSim is an independent model. It does not model Titan Core, Velaura silicon, or physical-design signoff.</p>");
    });
  }

  QComboBox* makeOverrideCombo(const QString& name, const QStringList& values, QWidget* parent) {
    auto* combo = new QComboBox(parent);
    combo->addItem(name + ": scenario default");
    for (const auto& value : values) combo->addItem(name + ": " + value, value);
    combo->setToolTip("Optional run-only override. Leave at scenario default to use the manifest value.");
    combo->setAccessibleName(name + " override");
    return combo;
  }

  void restoreWindowState() {
    QSettings settings;
    if (settings.contains("window/geometry")) restoreGeometry(settings.value("window/geometry").toByteArray());
    if (settings.contains("window/splitter")) splitter_->restoreState(settings.value("window/splitter").toByteArray());
  }

  static QDir findProjectRoot() {
    const QDir application_dir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        application_dir.absoluteFilePath("../Resources/axissim-data"),
        application_dir.absoluteFilePath("../../../.."),
        application_dir.absoluteFilePath(".."),
        QDir::currentPath(),
    };
    for (const auto& candidate : candidates) {
      const QDir directory(candidate);
      if (QFileInfo::exists(directory.filePath("configs/baseline/manifest.conf"))) return directory;
    }
    return QDir(QDir::currentPath());
  }

  QString cliExecutable() const {
    const QDir application_dir(QCoreApplication::applicationDirPath());
    const QString application_path = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QStringList candidates = {
        application_dir.filePath("axissim-cli"),
        application_dir.absoluteFilePath("../../../axissim"),
        project_root_.filePath("build/axissim"),
        application_dir.filePath("axissim"),
    };
    for (const auto& candidate : candidates) {
      const QFileInfo info(candidate);
      if (info.isExecutable() && info.canonicalFilePath() != application_path) return info.absoluteFilePath();
    }
    return {};
  }

  void loadScenario(const QString& path) {
    if (path.isEmpty()) return;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
      showError("Unable to open scenario", "Could not read:\n" + path);
      return;
    }
    editor_->setPlainText(QString::fromUtf8(input.readAll()));
    scenario_path_ = QFileInfo(path).absoluteFilePath();
    editor_->document()->setModified(false);
    scenario_path_label_->setText(QFileInfo(path).fileName());
    scenario_path_label_->setToolTip(scenario_path_);
    statusBar()->showMessage("Loaded " + scenario_path_, 5'000);
  }

  QString publicMnistProfilePath() const {
    return project_root_.filePath("configs/workloads/mnist12_profiled.conf");
  }

  bool tutorialCompleted() const { return QSettings().value("tutorial/v1/completed", false).toBool(); }
  bool tutorialDismissed() const { return QSettings().value("tutorial/v1/dismissed", false).toBool(); }

  void refreshTutorialEntry() {
    if (!tutorial_start_button_) return;
    const bool show_entry = tutorial_step_ == TutorialStep::Inactive && last_result_json_.isEmpty() && !tutorialCompleted() && !tutorialDismissed();
    tutorial_start_button_->setVisible(show_entry);
    if (show_entry && result_context_ && last_result_json_.isEmpty()) {
      result_context_->setText("New to AxisSim? Take a two-minute guided run with a public workload.");
    }
  }

  void setTutorialTarget(QWidget* target) {
    if (tutorial_target_ == target) return;
    if (tutorial_target_) {
      tutorial_target_->setProperty("tutorialTarget", false);
      tutorial_target_->style()->unpolish(tutorial_target_);
      tutorial_target_->style()->polish(tutorial_target_);
    }
    tutorial_target_ = target;
    if (tutorial_target_) {
      tutorial_target_->setProperty("tutorialTarget", true);
      tutorial_target_->style()->unpolish(tutorial_target_);
      tutorial_target_->style()->polish(tutorial_target_);
      tutorial_target_->setFocus(Qt::OtherFocusReason);
    }
  }

  void updateTutorialPanel() {
    if (!tutorial_panel_) return;
    const bool active = tutorial_step_ != TutorialStep::Inactive;
    tutorial_panel_->setVisible(active);
    refreshTutorialEntry();
    if (!active) {
      setTutorialTarget(nullptr);
      return;
    }

    tutorial_action_button_->show();
    tutorial_skip_button_->show();
    switch (tutorial_step_) {
      case TutorialStep::AwaitingPublicRun:
        tutorial_title_->setText("Guided first run · 1 of 4 · Run a public workload");
        tutorial_text_->setText("Run the checked-in MNIST-12 operator profile. AxisSim will create real JSON and trace artifacts; its outputs remain modeled estimates.");
        tutorial_action_button_->setText("Run public MNIST-12");
        setTutorialTarget(mnist_button_);
        break;
      case TutorialStep::ExplainingBaseline:
        tutorial_title_->setText("Guided first run · 2 of 4 · Read the model result");
        tutorial_text_->setText(tutorial_notice_);
        tutorial_action_button_->setText("Prepare sensitivity");
        setTutorialTarget(energy_chart_);
        break;
      case TutorialStep::AwaitingSensitivityRun:
        tutorial_title_->setText("Guided first run · 3 of 4 · Test one assumption");
        tutorial_text_->setText(tutorial_notice_.isEmpty()
            ? "The visible quick override now models 3× lower NPU arithmetic energy at fixed performance. Run the model to compare it with the public-workload baseline."
            : tutorial_notice_);
        tutorial_action_button_->setText("Run sensitivity model");
        setTutorialTarget(run_button_);
        break;
      case TutorialStep::ExplainingTradeoff:
        tutorial_title_->setText("Guided first run · 4 of 4 · Interpret the tradeoff");
        tutorial_text_->setText(tutorial_notice_);
        tutorial_action_button_->setText("Open evidence & finish");
        setTutorialTarget(result_views_);
        break;
      case TutorialStep::Inactive:
        break;
    }
  }

  void startTutorial(bool persist = true) {
    if (process_ && process_->state() != QProcess::NotRunning) return;
    if (persist) QSettings().setValue("tutorial/v1/dismissed", false);
    tutorial_baseline_ = {};
    tutorial_notice_.clear();
    tutorial_step_ = TutorialStep::AwaitingPublicRun;
    updateTutorialPanel();
    statusBar()->showMessage("Guided first run started. Run the public MNIST-12 profile to begin.", 8'000);
  }

  void dismissTutorial() {
    QSettings().setValue("tutorial/v1/dismissed", true);
    tutorial_step_ = TutorialStep::Inactive;
    tutorial_notice_.clear();
    updateTutorialPanel();
    statusBar()->showMessage("Guided first run dismissed. Restart it any time from Help.", 8'000);
  }

  void resetTutorial() {
    QSettings settings;
    settings.remove("tutorial/v1/completed");
    settings.remove("tutorial/v1/dismissed");
    startTutorial();
  }

  int arithmeticScaleIndex(double scale) const {
    for (int index = 0; index < arithmetic_scale_->count(); ++index) {
      const QVariant value = arithmetic_scale_->itemData(index);
      if (value.isValid() && std::abs(value.toDouble() - scale) < 0.000001) return index;
    }
    return -1;
  }

  void prepareGuidedSensitivity() {
    const int scale_index = arithmeticScaleIndex(0.333333);
    if (scale_index < 0) {
      showError("Guided setting unavailable", "AxisSim could not find the 3× lower arithmetic-energy quick override.");
      return;
    }
    arithmetic_scale_->setCurrentIndex(scale_index);
    tutorial_notice_.clear();
    tutorial_step_ = TutorialStep::AwaitingSensitivityRun;
    updateTutorialPanel();
    statusBar()->showMessage("Prepared a 3× lower NPU arithmetic-energy sensitivity. Run the model to compare it with the captured baseline.", 8'000);
  }

  void finishTutorial() {
    QSettings settings;
    settings.setValue("tutorial/v1/completed", true);
    settings.setValue("tutorial/v1/dismissed", false);
    tutorial_step_ = TutorialStep::Inactive;
    updateTutorialPanel();
    if (nav_buttons_.size() > 4) nav_buttons_[4]->click();
    statusBar()->showMessage("Guided first run complete. Use Experiment Lab to make your own reproducible scenario changes.", 10'000);
  }

  void advanceTutorial() {
    switch (tutorial_step_) {
      case TutorialStep::AwaitingPublicRun:
        runPublicMnistProfile();
        break;
      case TutorialStep::ExplainingBaseline:
        prepareGuidedSensitivity();
        break;
      case TutorialStep::AwaitingSensitivityRun:
        runScenario();
        break;
      case TutorialStep::ExplainingTradeoff:
        finishTutorial();
        break;
      case TutorialStep::Inactive:
        break;
    }
  }

  TutorialSnapshot tutorialSnapshot(const QJsonObject& result) const {
    const QJsonObject energy = result.value("energy").toObject();
    const QJsonObject latency = result.value("latency").toObject();
    const int frames = std::max(1, result.value("deadlines").toObject().value("vision_jobs").toInt());
    const double total = energy.value("it_per_frame_uj").toDouble();
    const std::array<std::pair<QString, double>, 5> components = {{
        {"Vector", energy.value("vector_pj").toDouble() / frames / 1'000'000.0},
        {"NPU arithmetic", energy.value("npu_arithmetic_pj").toDouble() / frames / 1'000'000.0},
        {"DRAM", energy.value("dram_pj").toDouble() / frames / 1'000'000.0},
        {"DMA", energy.value("dma_pj").toDouble() / frames / 1'000'000.0},
        {"Static", energy.value("static_per_frame_uj").toDouble()},
    }};
    const auto dominant = std::max_element(components.cbegin(), components.cend(),
                                           [](const auto& left, const auto& right) { return left.second < right.second; });
    return {true, latency.value("mean_us").toDouble(), total,
            energy.value("npu_arithmetic_pj").toDouble() / frames / 1'000'000.0,
            dominant->first, total > 0.0 ? 100.0 * dominant->second / total : 0.0};
  }

  bool isGuidedSensitivityConfigured() const {
    return QFileInfo(scenario_path_).absoluteFilePath() == QFileInfo(publicMnistProfilePath()).absoluteFilePath() &&
           arithmetic_scale_->currentData().isValid() &&
           std::abs(arithmetic_scale_->currentData().toDouble() - 0.333333) < 0.000001;
  }

  void handleTutorialResult(const QJsonObject& result) {
    const bool public_profile = result.value("workload_mode").toString() == "profiled_graph";
    if (tutorial_step_ == TutorialStep::AwaitingPublicRun) {
      if (!public_profile) {
        tutorial_notice_ = "This guide needs the public MNIST-12 profiled workload. Select Run public MNIST-12 to continue.";
        updateTutorialPanel();
        return;
      }
      tutorial_baseline_ = tutorialSnapshot(result);
      tutorial_notice_ = QString("This public-workload run completed at %1 µs mean latency and %2 µJ/frame. %3 is the largest modeled energy component (%4%). These are model outputs, not silicon measurements.")
          .arg(tutorial_baseline_.mean_latency_us, 0, 'f', 2)
          .arg(tutorial_baseline_.energy_per_frame_uj, 0, 'f', 2)
          .arg(tutorial_baseline_.dominant_component)
          .arg(tutorial_baseline_.dominant_share_percent, 0, 'f', 1);
      tutorial_step_ = TutorialStep::ExplainingBaseline;
      updateTutorialPanel();
      if (tutorial_smoke_run_) {
        QTimer::singleShot(0, this, [this] {
          prepareGuidedSensitivity();
          runScenario();
        });
      }
      return;
    }
    if (tutorial_step_ != TutorialStep::AwaitingSensitivityRun) return;
    if (!public_profile || !isGuidedSensitivityConfigured()) {
      tutorial_notice_ = "This run did not use the guided public profile and 3× arithmetic-energy setting. Restore the guided setting, then run the model again.";
      updateTutorialPanel();
      return;
    }
    const TutorialSnapshot candidate = tutorialSnapshot(result);
    const double energy_delta = 100.0 * (candidate.energy_per_frame_uj / tutorial_baseline_.energy_per_frame_uj - 1.0);
    const double latency_delta = 100.0 * (candidate.mean_latency_us / tutorial_baseline_.mean_latency_us - 1.0);
    const QString energy_direction = energy_delta <= 0.0 ? "decreased" : "increased";
    tutorial_notice_ = QString("Observed sensitivity: total modeled energy %1 by %2% while mean latency changed %3%. NPU arithmetic changed, but %4 remains the largest component at %5% of this run. This is why component-level efficiency should be evaluated at system level.")
        .arg(energy_direction)
        .arg(std::abs(energy_delta), 0, 'f', 1)
        .arg(latency_delta, 0, 'f', 1)
        .arg(candidate.dominant_component)
        .arg(candidate.dominant_share_percent, 0, 'f', 1);
    tutorial_step_ = TutorialStep::ExplainingTradeoff;
    updateTutorialPanel();
    if (tutorial_smoke_run_) {
      QTimer::singleShot(0, this, [this] {
        if (tutorial_step_ == TutorialStep::ExplainingTradeoff) QCoreApplication::exit(0);
      });
    }
  }

  void changeScenario(int index) {
    if (editor_->document()->isModified()) {
      const auto choice = QMessageBox::question(this, "Discard unsaved edits?",
                                                 "Switching scenarios will discard the unsaved manifest edits.",
                                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
      if (choice != QMessageBox::Discard) {
        const QSignalBlocker blocker(scenario_);
        scenario_->setCurrentIndex(previous_scenario_index_);
        return;
      }
    }
    previous_scenario_index_ = index;
    rvv_lanes_->setCurrentIndex(0);
    npu_macs_->setCurrentIndex(0);
    dram_bandwidth_->setCurrentIndex(0);
    arithmetic_scale_->setCurrentIndex(0);
    loadScenario(scenario_->currentData().toString());
  }

  void openScenario() {
    const QString path = QFileDialog::getOpenFileName(this, "Open AxisSim scenario", scenario_path_, "AxisSim config (*.conf);;All files (*)");
    if (path.isEmpty()) return;
    scenario_->addItem(QFileInfo(path).fileName(), path);
    scenario_->setCurrentIndex(scenario_->count() - 1);
  }

  bool saveAs() {
    const QString path = QFileDialog::getSaveFileName(this, "Save AxisSim scenario", scenario_path_, "AxisSim config (*.conf)");
    if (path.isEmpty()) return false;
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
      showError("Unable to save scenario", "Could not write:\n" + path);
      return false;
    }
    QTextStream stream(&output);
    stream << editor_->toPlainText();
    output.close();
    scenario_path_ = QFileInfo(path).absoluteFilePath();
    scenario_path_label_->setText(QFileInfo(path).fileName());
    scenario_path_label_->setToolTip(scenario_path_);
    editor_->document()->setModified(false);
    statusBar()->showMessage("Saved " + scenario_path_, 5'000);
    return true;
  }

  void exportResult() {
    if (last_result_json_.isEmpty()) return;
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString suggested = QDir(documents.isEmpty() ? QDir::homePath() : documents).filePath("axissim_result.json");
    const QString path = QFileDialog::getSaveFileName(this, "Export AxisSim result", suggested, "JSON result (*.json)");
    if (path.isEmpty()) return;
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      showError("Unable to export result", "Could not write:\n" + path);
      return;
    }
    output.write(last_result_json_);
    output.close();
    statusBar()->showMessage("Exported result to " + path, 8'000);
  }

  bool ensureSaved() {
    if (!editor_->document()->isModified()) return true;
    const auto choice = QMessageBox::question(this, "Save scenario copy",
                                               "This manifest has unsaved edits. Save a copy before running?\n\nSaving a copy preserves the checked-in scenario and keeps the model run reproducible.",
                                               QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Save);
    return choice == QMessageBox::Save && saveAs();
  }

  QStringList quickOverrides(bool include_rvv_lanes) const {
    QStringList arguments;
    const auto append = [&arguments](const QString& key, const QComboBox* combo) {
      if (combo->currentData().isValid()) arguments << "--set" << key + "=" + combo->currentData().toString();
    };
    if (include_rvv_lanes) append("rvv_lanes", rvv_lanes_);
    append("npu_macs", npu_macs_);
    append("dram_bandwidth_gbps", dram_bandwidth_);
    if (arithmetic_scale_->currentData().isValid()) {
      arguments << "--set" << "npu_arithmetic_energy_scale=" + QString::number(arithmetic_scale_->currentData().toDouble(), 'g', 8);
    }
    return arguments;
  }

  void runScenario() {
    if (!ensureSaved()) return;
    const QString executable = cliExecutable();
    if (executable.isEmpty()) {
      showError("AxisSim CLI not found", "Build the axissim CLI and desktop targets together. The app searched beside the executable and in the project build directory.");
      return;
    }
    temporary_ = std::make_unique<QTemporaryDir>();
    if (!temporary_->isValid()) {
      showError("Unable to create run workspace", "AxisSim could not create a temporary directory for JSON and trace artifacts.");
      return;
    }
    const QString json_path = temporary_->filePath("result.json");
    const QString trace_path = temporary_->filePath("trace.csv");
    QStringList arguments = {"run", scenario_path_, "--json", json_path, "--trace", trace_path};
    arguments.append(quickOverrides(true));
    beginProcess("Running model…");
    connect(process_.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, json_path, trace_path](int exit_code, QProcess::ExitStatus exit_status) {
              const QString output = QString::fromUtf8(process_->readAllStandardOutput() + process_->readAllStandardError());
              if (exit_status != QProcess::NormalExit || exit_code != 0) {
                finishProcess(false, "Model run failed");
                showError("AxisSim model run failed", output);
                return;
              }
              showResult(json_path, trace_path, output);
            });
    process_->setWorkingDirectory(QFileInfo(scenario_path_).absolutePath());
    process_->start(executable, arguments);
  }

  void runPublicMnistProfile() {
    const QString profile_path = publicMnistProfilePath();
    const int profile_index = scenario_->findData(profile_path);
    if (profile_index < 0) {
      showError("Public profile unavailable", "The checked-in MNIST-12 profile scenario could not be located:\n" + profile_path);
      return;
    }

    if (scenario_->currentIndex() != profile_index) {
      scenario_->setCurrentIndex(profile_index);
      if (QFileInfo(scenario_path_).absoluteFilePath() != QFileInfo(profile_path).absoluteFilePath()) return;
    } else if (editor_->document()->isModified()) {
      const auto choice = QMessageBox::question(
          this, "Restore public profile?",
          "Running the public example restores its checked-in manifest and clears unsaved editor changes. Continue?",
          QMessageBox::RestoreDefaults | QMessageBox::Cancel, QMessageBox::RestoreDefaults);
      if (choice != QMessageBox::RestoreDefaults) return;
      loadScenario(profile_path);
    }

    // This is intentionally a reproducible, no-overrides starting point rather
    // than a hidden demo. Users can then change values in Experiment Lab.
    rvv_lanes_->setCurrentIndex(0);
    npu_macs_->setCurrentIndex(0);
    dram_bandwidth_->setCurrentIndex(0);
    arithmetic_scale_->setCurrentIndex(0);
    statusBar()->showMessage("Loaded public MNIST-12 profile; running with checked-in scenario defaults.", 5'000);
    runScenario();
  }

  void runLaneSweep() {
    if (!ensureSaved()) return;
    const QString executable = cliExecutable();
    if (executable.isEmpty()) {
      showError("AxisSim CLI not found", "Build the axissim CLI and desktop targets together.");
      return;
    }
    QStringList arguments = {"sweep", scenario_path_, "--sweep", "rvv_lanes=2,4,8,16,32"};
    arguments.append(quickOverrides(false));
    beginProcess("Sweeping RVV lanes…");
    connect(process_.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
              const QString output = QString::fromUtf8(process_->readAllStandardOutput() + process_->readAllStandardError());
              if (exit_status != QProcess::NormalExit || exit_code != 0) {
                finishProcess(false, "Lane sweep failed");
                showError("AxisSim lane sweep failed", output);
                return;
              }
              populateSweep(output);
              log_->setPlainText("RVV lane sweep\n\n" + output);
              result_views_->setCurrentIndex(2);
              finishProcess(true, "Lane sweep complete");
            });
    process_->setWorkingDirectory(QFileInfo(scenario_path_).absolutePath());
    process_->start(executable, arguments);
  }

  void runAra2Check() {
    const QString executable = cliExecutable();
    const QString config = project_root_.filePath("configs/baseline/manifest.conf");
    const QString reference = project_root_.filePath("reference_data/ara/ara2_published_latency.csv");
    if (executable.isEmpty() || !QFileInfo::exists(config) || !QFileInfo::exists(reference)) {
      showError("Ara2 reference inputs not found", "Build from a complete AxisSim checkout so the CLI, calibration config, and reference CSV are available.");
      return;
    }
    beginProcess("Checking Ara2 reference…");
    connect(process_.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
              const QString output = QString::fromUtf8(process_->readAllStandardOutput() + process_->readAllStandardError());
              if (exit_status != QProcess::NormalExit || exit_code != 0) {
                finishProcess(false, "Ara2 check failed");
                showError("Ara2 reference check failed", output);
                return;
              }
              log_->setPlainText("Published Ara2 compute-only consistency check\n\n" + output +
                                 "\nScope: normalized vector-FMA timing derived from public Ara2 data. This is not end-to-end Ara, silicon-power, or full-SoC validation.");
              result_views_->setCurrentIndex(log_tab_index_);
              finishProcess(true, "Ara2 reference check complete");
            });
    process_->setWorkingDirectory(project_root_.absolutePath());
    process_->start(executable, {"calibrate", config, "--reference", reference});
  }

  void beginProcess(const QString& activity) {
    process_ = std::make_unique<QProcess>(this);
    connect(process_.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
      if (error != QProcess::FailedToStart) return;
      finishProcess(false, "Could not start model");
      showError("Unable to start AxisSim CLI", process_->errorString());
    });
    setBusy(true, activity);
  }

  void finishProcess(bool success, const QString& message) {
    setBusy(false, message);
    status_pill_->setProperty("state", success ? "success" : "error");
    status_pill_->style()->unpolish(status_pill_);
    status_pill_->style()->polish(status_pill_);
    if (!success && result_context_) result_context_->setText("The last operation did not complete. Review the error and configuration, then run again.");
    statusBar()->showMessage(message, success ? 8'000 : 12'000);
  }

  void setBusy(bool busy, const QString& message) {
    progress_->setVisible(busy);
    status_pill_->setText(message);
    status_pill_->setProperty("state", busy ? "busy" : "idle");
    status_pill_->style()->unpolish(status_pill_);
    status_pill_->style()->polish(status_pill_);
    if (busy && result_context_) result_context_->setText("Running the deterministic CLI and preparing JSON and trace artifacts…");
    scenario_->setEnabled(!busy);
    browse_button_->setEnabled(!busy);
    save_button_->setEnabled(!busy);
    run_button_->setEnabled(!busy);
    mnist_button_->setEnabled(!busy);
    tutorial_start_button_->setEnabled(!busy);
    tutorial_action_button_->setEnabled(!busy);
    tutorial_skip_button_->setEnabled(!busy);
    sweep_button_->setEnabled(!busy);
    ara_button_->setEnabled(!busy);
    rvv_lanes_->setEnabled(!busy);
    npu_macs_->setEnabled(!busy);
    dram_bandwidth_->setEnabled(!busy);
    arithmetic_scale_->setEnabled(!busy);
    open_action_->setEnabled(!busy);
    save_action_->setEnabled(!busy);
    run_action_->setEnabled(!busy);
    mnist_action_->setEnabled(!busy);
    sweep_action_->setEnabled(!busy);
    ara_action_->setEnabled(!busy);
    tutorial_start_action_->setEnabled(!busy);
    tutorial_reset_action_->setEnabled(!busy);
  }

  void showError(const QString& title, const QString& detail) {
    if (exit_after_run_) {
      QCoreApplication::exit(1);
      return;
    }
    QMessageBox message(QMessageBox::Critical, title, detail, QMessageBox::Ok, this);
    message.setTextInteractionFlags(Qt::TextSelectableByMouse);
    message.exec();
  }

  void showResult(const QString& json_path, const QString& trace_path, const QString& cli_output) {
    QFile result_file(json_path);
    if (!result_file.open(QIODevice::ReadOnly)) {
      finishProcess(false, "Result artifact missing");
      showError("AxisSim result unavailable", "The CLI completed without producing the expected JSON artifact.");
      return;
    }
    last_result_json_ = result_file.readAll();
    refreshTutorialEntry();
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(last_result_json_, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
      finishProcess(false, "Invalid result JSON");
      showError("AxisSim result is invalid", parse_error.errorString());
      return;
    }
    const QJsonObject result = document.object();
    const QJsonObject latency = result.value("latency").toObject();
    const QJsonObject deadlines = result.value("deadlines").toObject();
    const QJsonObject traffic = result.value("traffic").toObject();
    const QJsonObject energy = result.value("energy").toObject();
    const QJsonObject power = result.value("power").toObject();
    const int frames = std::max(1, deadlines.value("vision_jobs").toInt());

    const double mean_latency_us = latency.value("mean_us").toDouble();
    const double deadline_us = result.value("deadline_us").toDouble();
    const double deadline_margin_us = deadline_us - latency.value("p99_us").toDouble();
    const std::array<std::pair<QString, double>, 4> energy_components = {{
        {"Vector", energy.value("vector_pj").toDouble() / frames / 1'000'000.0},
        {"NPU arithmetic", energy.value("npu_arithmetic_pj").toDouble() / frames / 1'000'000.0},
        {"DRAM", energy.value("dram_pj").toDouble() / frames / 1'000'000.0},
        {"DMA", energy.value("dma_pj").toDouble() / frames / 1'000'000.0},
    }};
    const auto dominant_energy = std::max_element(energy_components.cbegin(), energy_components.cend(),
                                                   [](const auto& left, const auto& right) { return left.second < right.second; });
    const double energy_total = std::max(energy.value("it_per_frame_uj").toDouble(), 0.000001);
    latency_card_->setValue(QString::number(mean_latency_us, 'f', 2) + " µs",
                            QString("P99 %1 µs").arg(latency.value("p99_us").toDouble(), 0, 'f', 2));
    deadline_card_->setValue(QString::number(deadline_margin_us / 1'000.0, 'f', 3) + " ms",
                             QString("%1 / %2 missed").arg(deadlines.value("vision_misses").toInt()).arg(frames));
    energy_card_->setValue(QString::number(energy.value("it_per_frame_uj").toDouble(), 'f', 2) + " µJ", "modeled estimate");
    traffic_card_->setValue(QString("%1 %2%").arg(dominant_energy->first).arg(100.0 * dominant_energy->second / energy_total, 0, 'f', 1),
                            QString("%1 µJ / frame").arg(dominant_energy->second, 0, 'f', 2));
    energy_finding_->setText(QString("Data movement owns %1% of dynamic energy.")
                                 .arg(100.0 * (energy_components[2].second + energy_components[3].second) /
                                      std::max(energy.value("per_frame_uj").toDouble(), 0.000001), 0, 'f', 1));

    energy_chart_->setSegments({
        {"Vector", energy.value("vector_pj").toDouble() / frames / 1'000'000.0, AxisTheme::chartRvv()},
        {"NPU arithmetic", energy.value("npu_arithmetic_pj").toDouble() / frames / 1'000'000.0, AxisTheme::chartNpu()},
        {"DRAM", energy.value("dram_pj").toDouble() / frames / 1'000'000.0, AxisTheme::chartDma()},
        {"DMA", energy.value("dma_pj").toDouble() / frames / 1'000'000.0, AxisTheme::chartControl()},
        {"Static", energy.value("static_per_frame_uj").toDouble(), AxisTheme::textSecondary()},
    });
    populateMetrics(result);
    populateProxies(result);
    const bool public_profile = result.value("workload_mode").toString() == "profiled_graph";
    if (public_profile) {
      QFile profile(project_root_.filePath("reference_data/workloads/mnist12_profile.json"));
      QJsonParseError profile_error;
      const QJsonDocument profile_document = profile.open(QIODevice::ReadOnly)
          ? QJsonDocument::fromJson(profile.readAll(), &profile_error) : QJsonDocument();
      const QJsonObject profile_root = profile_document.object();
      const QJsonObject source = profile_root.value("source").toObject();
      profile_evidence_->setText(QString("Public workload evidence · %1 · %2 operators · static input %3 · %4 license · SHA-256 %5 · %6\nStatic operation/traffic characterization only; not a measured runtime or compiler mapping.")
          .arg(source.value("model_name").toString("MNIST-12"))
          .arg(profile_root.value("operators").toArray().size())
          .arg("1×1×28×28")
          .arg(source.value("license").toString("recorded upstream"))
          .arg(source.value("model_sha256").toString("unavailable"))
          .arg(source.value("source_url").toString("source unavailable")));
    } else {
      profile_evidence_->setText("No public workload profile is selected. Profile evidence appears here when a profiled-graph scenario runs.");
    }
    populateTrace(trace_path);
    log_->setPlainText(QString("Scenario: %1  •  model %2  •  result schema %3\n\n%4\n\n"
                               "Interpretation boundary\n"
                               "AxisSim reports model outputs under explicit assumptions. Area, thermal, and FIT reliability values are sensitivity proxies—not implementation signoff or measured hardware.")
                           .arg(result.value("scenario_name").toString(), result.value("model_version").toString(), result.value("schema_version").toString(), cli_output));
    export_button_->setEnabled(true);
    export_action_->setEnabled(true);
    result_views_->setCurrentIndex(0);
    result_context_->setText(scenario_->currentText());
    finishProcess(true, power.value("budget_w").toDouble() > 0.0 && !power.value("feasible").toBool() ? "Complete • budget exceeded" : "Model complete");
    handleTutorialResult(result);
    if (!snapshot_path_.isEmpty()) {
      bool tab_ok = false;
      const int requested_tab = qEnvironmentVariable("AXISSIM_SNAPSHOT_TAB").toInt(&tab_ok);
      if (tab_ok && requested_tab >= 0 && requested_tab < result_views_->count()) result_views_->setCurrentIndex(requested_tab);
      QTimer::singleShot(100, this, [this] {
        QFileInfo output(snapshot_path_);
        output.dir().mkpath(".");
        const bool saved = grab().save(snapshot_path_);
        QCoreApplication::exit(saved ? 0 : 1);
      });
      return;
    }
    if (exit_after_run_) QCoreApplication::exit(0);
  }

  static void appendRow(QTableWidget* table, const QString& name, const QString& value, const QString& interpretation) {
    const int row = table->rowCount();
    table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(name));
    table->setItem(row, 1, new QTableWidgetItem(value));
    table->setItem(row, 2, new QTableWidgetItem(interpretation));
  }

  void populateMetrics(const QJsonObject& result) {
    metrics_->setRowCount(0);
    const QJsonObject latency = result.value("latency").toObject();
    const QJsonObject timing = result.value("timing").toObject();
    const QJsonObject energy = result.value("energy").toObject();
    const QJsonObject power = result.value("power").toObject();
    const QJsonObject deployment = result.value("deployment").toObject();
    appendRow(metrics_, "Mean latency", QString::number(latency.value("mean_us").toDouble(), 'f', 3) + " µs", "End-to-end mean across completed vision jobs");
    appendRow(metrics_, "P99 latency", QString::number(latency.value("p99_us").toDouble(), 'f', 3) + " µs", "Modeled tail latency across completed vision jobs");
    appendRow(metrics_, "Modeled makespan", QString::number(timing.value("makespan_us").toDouble() / 1'000'000.0, 'f', 6) + " s", "Elapsed simulated workload window");
    appendRow(metrics_, "Dynamic energy / frame", QString::number(energy.value("per_frame_uj").toDouble(), 'f', 3) + " µJ", "Activity-count estimate; excludes modeled static power");
    appendRow(metrics_, "IT energy / frame", QString::number(energy.value("it_per_frame_uj").toDouble(), 'f', 3) + " µJ", "Dynamic plus configured static-capacity power");
    appendRow(metrics_, "Facility energy / frame", QString::number(energy.value("facility_per_frame_uj").toDouble(), 'f', 3) + " µJ", "IT energy multiplied by configured PUE");
    appendRow(metrics_, "Average IT power", QString::number(power.value("average_it_w").toDouble(), 'f', 6) + " W", "Average over the modeled makespan");
    appendRow(metrics_, "Average facility power", QString::number(power.value("average_facility_w").toDouble(), 'f', 6) + " W", "Average IT power × PUE");
    const double budget_w = power.value("budget_w").toDouble();
    appendRow(metrics_, "IT power budget", budget_w > 0.0 ? QString::number(budget_w, 'f', 6) + " W" : "Not configured",
              budget_w > 0.0 ? (power.value("feasible").toBool() ? "Feasible" : "Infeasible") : "No budget constraint applied");
    appendRow(metrics_, "PUE", QString::number(power.value("pue").toDouble(), 'f', 3), "Facility multiplier; not a chip property");
    const double deployment_hours = deployment.value("hours").toDouble();
    if (deployment_hours > 0.0) {
      appendRow(metrics_, "Deployment scope", QString("%1 XPUs × %2 h").arg(deployment.value("xpu_count").toInt()).arg(deployment_hours, 0, 'f', 2), "Straight-line extrapolation of average facility power");
      appendRow(metrics_, "Deployment facility energy", QString::number(deployment.value("facility_energy_kwh").toDouble(), 'f', 3) + " kWh", "Modeled deployment estimate");
      appendRow(metrics_, "Electricity cost", "$" + QString::number(deployment.value("electricity_cost").toDouble(), 'f', 2),
                QString("At $%1/kWh; excludes demand charges and other TCO").arg(deployment.value("electricity_cost_per_kwh").toDouble(), 0, 'f', 4));
    } else {
      appendRow(metrics_, "Deployment economics", "Not configured", "Set deployment_hours, deployment_xpu_count, and electricity_cost_per_kwh");
    }
  }

  void populateProxies(const QJsonObject& result) {
    proxies_->setRowCount(0);
    const QJsonObject area = result.value("area_proxy").toObject();
    const QJsonObject thermal = result.value("thermal_proxy").toObject();
    const QJsonObject reliability = result.value("reliability_proxy").toObject();
    const bool area_enabled = area.value("modeled_mm2").toDouble() > 0.0 || area.value("limit_mm2").toDouble() > 0.0;
    if (area_enabled) {
      appendRow(proxies_, "Architecture area", QString::number(area.value("modeled_mm2").toDouble(), 'f', 4) + " mm²",
                area.value("limit_mm2").toDouble() > 0.0 ? (area.value("feasible").toBool() ? "Within configured limit" : "Exceeds configured limit") : "No limit configured");
      appendRow(proxies_, "Area coefficients", QString("base %1; NPU %2/MAC; RVV %3/lane; SRAM %4/KiB")
                                                       .arg(area.value("base_mm2").toDouble(), 0, 'f', 3)
                                                       .arg(area.value("npu_per_mac_mm2").toDouble(), 0, 'g', 4)
                                                       .arg(area.value("rvv_per_lane_mm2").toDouble(), 0, 'g', 4)
                                                       .arg(area.value("sram_per_kib_mm2").toDouble(), 0, 'g', 4),
                "User-supplied coefficient sum; no placement or routing");
    } else {
      appendRow(proxies_, "Architecture area", "Not configured", "Coefficient sum only; no placement, routing, or GDS prediction");
    }
    const bool thermal_enabled = thermal.value("resistance_c_per_w").toDouble() > 0.0;
    if (thermal_enabled) {
      appendRow(proxies_, "Transient junction temperature", QString::number(thermal.value("end_c").toDouble(), 'f', 3) + " °C",
                thermal.value("limit_c").toDouble() > 0.0 ? (thermal.value("feasible").toBool() ? "Within configured limit" : "Exceeds configured limit") : "No limit configured");
      appendRow(proxies_, "Steady-state temperature", QString::number(thermal.value("steady_state_c").toDouble(), 'f', 3) + " °C", "Single-node first-order RC proxy; no hotspot/package model");
      appendRow(proxies_, "Thermal assumptions", QString("Rθ %1 °C/W, Cth %2 J/°C").arg(thermal.value("resistance_c_per_w").toDouble(), 0, 'f', 4).arg(thermal.value("capacitance_j_per_c").toDouble(), 0, 'f', 4), "User-supplied sensitivity inputs");
    } else {
      appendRow(proxies_, "Thermal response", "Not configured", "Set thermal resistance and capacitance; not thermal signoff");
    }
    const bool reliability_enabled = reliability.value("fit_rate").toDouble() > 0.0;
    if (reliability_enabled) {
      appendRow(proxies_, "Expected deployment failures", QString::number(reliability.value("expected_failures").toDouble(), 'f', 6), "FIT extrapolation, not a predicted silicon failure rate");
      appendRow(proxies_, "Reliability assumptions", QString("%1 FIT × %2 voltage multiplier").arg(reliability.value("fit_rate").toDouble(), 0, 'f', 3).arg(reliability.value("voltage_multiplier").toDouble(), 0, 'f', 3), "User-supplied; no soft-error, yield, ECC, or aging model");
    } else {
      appendRow(proxies_, "Reliability", "Not configured", "Set a sourced FIT rate and deployment horizon for sensitivity analysis");
    }
  }

  void populateTrace(const QString& trace_path) {
    QFile trace_file(trace_path);
    if (!trace_file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    trace_->setRowCount(0);
    QVector<TimelineEvent> events;
    QTextStream input(&trace_file);
    input.readLine();
    while (!input.atEnd()) {
      const QStringList fields = input.readLine().split(',');
      if (fields.size() != 6 || fields[0] != "0") continue;
      const int row = trace_->rowCount();
      trace_->insertRow(row);
      constexpr std::array<int, 5> source_columns = {0, 1, 2, 3, 5};
      for (int column = 0; column < 5; ++column) {
        trace_->setItem(row, column, new QTableWidgetItem(fields[source_columns[static_cast<std::size_t>(column)]]));
      }
      bool start_ok = false;
      bool end_ok = false;
      const double start = fields[3].toDouble(&start_ok);
      const double end = fields[4].toDouble(&end_ok);
      QColor color = AxisTheme::chartDma();
      if (fields[2] == "RVV") color = AxisTheme::chartRvv();
      if (fields[2] == "NPU") color = AxisTheme::chartNpu();
      if (start_ok && end_ok) events.push_back({fields[1], fields[2], start, end, color});
    }
    if (!events.isEmpty()) {
      const auto longest = std::max_element(events.cbegin(), events.cend(),
                                            [](const TimelineEvent& left, const TimelineEvent& right) {
                                              return left.end_us - left.start_us < right.end_us - right.start_us;
                                            });
      const double duration = longest->end_us - longest->start_us;
      const double frame_end = std::max_element(events.cbegin(), events.cend(),
                                                [](const TimelineEvent& left, const TimelineEvent& right) {
                                                  return left.end_us < right.end_us;
                                                })->end_us;
      latency_finding_->setText(QString("%1 owns %2% of frame latency.")
                                    .arg(longest->task)
                                    .arg(100.0 * duration / std::max(frame_end, 0.001), 0, 'f', 1));
    }
    timeline_chart_->setEvents(std::move(events));
  }

  void populateSweep(const QString& output) {
    sweep_->setRowCount(0);
    QVector<SweepPoint> points;
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (qsizetype index = 1; index < lines.size(); ++index) {
      const QStringList fields = lines[index].split(',');
      if (fields.size() < 9) continue;
      bool lanes_ok = false;
      bool latency_ok = false;
      const double lanes = fields[3].toDouble(&lanes_ok);
      const double latency = fields[4].toDouble(&latency_ok);
      bool p99_ok = false;
      bool traffic_ok = false;
      bool energy_ok = false;
      const double p99 = fields[5].toDouble(&p99_ok);
      const double traffic = fields[7].toDouble(&traffic_ok);
      const double energy = fields[8].toDouble(&energy_ok);
      if (lanes_ok && latency_ok && p99_ok && traffic_ok && energy_ok) {
        points.push_back({lanes, latency, p99, traffic, energy});
      }
    }
    std::sort(points.begin(), points.end(), [](const SweepPoint& left, const SweepPoint& right) { return left.lanes < right.lanes; });
    if (!points.isEmpty()) {
      const auto baseline = std::find_if(points.cbegin(), points.cend(), [](const SweepPoint& point) { return point.lanes == 8.0; });
      const auto best = std::min_element(points.cbegin(), points.cend(), [](const SweepPoint& left, const SweepPoint& right) {
        return left.latency_us < right.latency_us;
      });
      for (const auto& point : points) {
        const int row = sweep_->rowCount();
        sweep_->insertRow(row);
        const bool is_baseline = baseline != points.cend() && point.lanes == baseline->lanes;
        const bool is_best = best != points.cend() && point.lanes == best->lanes;
        const QString candidate = is_baseline ? "Baseline" : (is_best ? "Fastest" : "Candidate");
        const QString delta = baseline == points.cend() ? "—" : QString("%1%")
            .arg(100.0 * (point.latency_us / baseline->latency_us - 1.0), 0, 'f', 1);
        const QStringList values = {
            candidate,
            QString::number(point.lanes, 'f', 0),
            QString::number(point.latency_us, 'f', 2),
            delta,
            QString::number(point.dram_traffic_mb, 'f', 3),
            QString::number(point.energy_per_frame_uj, 'f', 3),
        };
        for (int column = 0; column < values.size(); ++column) {
          auto* item = new QTableWidgetItem(values[column]);
          if (is_best && !is_baseline) item->setForeground(AxisTheme::accent());
          sweep_->setItem(row, column, item);
        }
      }
      if (baseline != points.cend() && best != points.cend()) {
        const double delta = 100.0 * (best->latency_us / baseline->latency_us - 1.0);
        comparison_summary_->setText(QString("Fastest emitted candidate: %1 RVV lanes · %2 µs mean latency (%3% vs. 8-lane baseline). The ledger keeps DRAM traffic and energy visible beside latency.")
                                         .arg(best->lanes, 0, 'f', 0)
                                         .arg(best->latency_us, 0, 'f', 2)
                                         .arg(delta, 0, 'f', 1));
      }
    }
    sweep_chart_->setPoints(std::move(points));
  }

  QDir project_root_;
  QFrame* nav_rail_ = nullptr;
  QVBoxLayout* nav_layout_ = nullptr;
  QLabel* nav_title_ = nullptr;
  QLabel* nav_subtitle_ = nullptr;
  QLabel* nav_scope_ = nullptr;
  QLabel* appearance_label_ = nullptr;
  QLabel* header_title_ = nullptr;
  QLabel* header_subtitle_ = nullptr;
  QVector<QPushButton*> nav_buttons_;
  QStringList nav_labels_;
  QStringList nav_short_labels_;
  QComboBox* scenario_ = nullptr;
  QComboBox* rvv_lanes_ = nullptr;
  QComboBox* npu_macs_ = nullptr;
  QComboBox* dram_bandwidth_ = nullptr;
  QComboBox* arithmetic_scale_ = nullptr;
  QComboBox* appearance_combo_ = nullptr;
  QPushButton* browse_button_ = nullptr;
  QPushButton* save_button_ = nullptr;
  QPushButton* export_button_ = nullptr;
  QPushButton* run_button_ = nullptr;
  QPushButton* mnist_button_ = nullptr;
  QPushButton* tutorial_start_button_ = nullptr;
  QPushButton* tutorial_action_button_ = nullptr;
  QPushButton* tutorial_skip_button_ = nullptr;
  QPushButton* sweep_button_ = nullptr;
  QPushButton* ara_button_ = nullptr;
  QAction* export_action_ = nullptr;
  QAction* open_action_ = nullptr;
  QAction* save_action_ = nullptr;
  QAction* run_action_ = nullptr;
  QAction* mnist_action_ = nullptr;
  QAction* sweep_action_ = nullptr;
  QAction* ara_action_ = nullptr;
  QAction* tutorial_start_action_ = nullptr;
  QAction* tutorial_reset_action_ = nullptr;
  QLabel* status_pill_ = nullptr;
  QLabel* scenario_path_label_ = nullptr;
  QLabel* result_context_ = nullptr;
  QLabel* comparison_summary_ = nullptr;
  QLabel* profile_evidence_ = nullptr;
  QLabel* latency_finding_ = nullptr;
  QLabel* energy_finding_ = nullptr;
  QLabel* tutorial_title_ = nullptr;
  QLabel* tutorial_text_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QFrame* quick_controls_ = nullptr;
  QFrame* experiment_panel_ = nullptr;
  QFrame* tutorial_panel_ = nullptr;
  QWidget* tutorial_target_ = nullptr;
  QSplitter* splitter_ = nullptr;
  QPlainTextEdit* editor_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QTabWidget* result_views_ = nullptr;
  QTableWidget* trace_ = nullptr;
  ResourceTimelineWidget* timeline_chart_ = nullptr;
  QTableWidget* sweep_ = nullptr;
  QTableWidget* metrics_ = nullptr;
  QTableWidget* proxies_ = nullptr;
  EnergyBreakdownWidget* energy_chart_ = nullptr;
  SweepChartWidget* sweep_chart_ = nullptr;
  QGridLayout* metric_layout_ = nullptr;
  QVector<KpiCard*> metric_cards_;
  KpiCard* latency_card_ = nullptr;
  KpiCard* energy_card_ = nullptr;
  KpiCard* deadline_card_ = nullptr;
  KpiCard* traffic_card_ = nullptr;
  int log_tab_index_ = 0;
  int previous_scenario_index_ = 0;
  QString scenario_path_;
  QByteArray last_result_json_;
  std::unique_ptr<QProcess> process_;
  std::unique_ptr<QTemporaryDir> temporary_;
  QString snapshot_path_;
  TutorialStep tutorial_step_ = TutorialStep::Inactive;
  TutorialSnapshot tutorial_baseline_;
  QString tutorial_notice_;
  bool exit_after_run_ = false;
  bool tutorial_smoke_run_ = false;
  bool tutorial_preview_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  const bool smoke_test = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--smoke-test";
  const bool smoke_test_run = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--smoke-test-run";
  const bool tutorial_smoke_run = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--tutorial-smoke-test-run";
  const bool audit_ui = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--audit-ui";
  const bool snapshot_run = argc == 5 && QString::fromLocal8Bit(argv[1]) == "--snapshot-run";
  const bool tutorial_preview = argc == 5 && QString::fromLocal8Bit(argv[1]) == "--tutorial-preview";
  const QString snapshot_path = (snapshot_run || tutorial_preview) ? QString::fromLocal8Bit(argv[2]) : QString();
  bool width_ok = false;
  bool height_ok = false;
  const int snapshot_width = (snapshot_run || tutorial_preview) ? QString::fromLocal8Bit(argv[3]).toInt(&width_ok) : 0;
  const int snapshot_height = (snapshot_run || tutorial_preview) ? QString::fromLocal8Bit(argv[4]).toInt(&height_ok) : 0;
  if ((snapshot_run || tutorial_preview) && (!width_ok || !height_ok || snapshot_width < 1'100 || snapshot_height < 720)) return 2;
  QApplication application(argc, argv);
  Q_INIT_RESOURCE(gui);
  QCoreApplication::setOrganizationName("AxisSim");
  QCoreApplication::setApplicationName("AxisSim Desktop Explorer");
  QCoreApplication::setApplicationVersion("0.2");
  application.setWindowIcon(QIcon(":/icons/axissim.png"));
  AxisTheme::installFonts();
  const AxisTheme::Mode appearance_mode =
      AxisTheme::modeFromKey(requestedAppearanceKey());
  AxisTheme::apply(application, appearance_mode);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  QObject::connect(application.styleHints(), &QStyleHints::colorSchemeChanged, &application,
                   [&application](Qt::ColorScheme) {
                     const AxisTheme::Mode current_mode =
                         AxisTheme::modeFromKey(QSettings().value("appearance/mode", "system").toString());
                     if (current_mode == AxisTheme::Mode::System) AxisTheme::apply(application, current_mode);
                   });
#endif
  AxisSimWindow window(smoke_test_run || snapshot_run, snapshot_path,
                       (snapshot_run || tutorial_preview) ? QSize(snapshot_width, snapshot_height) : QSize(), tutorial_smoke_run, tutorial_preview);
  window.show();
  if (smoke_test) QTimer::singleShot(0, &application, &QCoreApplication::quit);
  if (audit_ui) {
    QTimer::singleShot(0, &application, [&window] {
      const QStringList issues = window.accessibilityIssues();
      QTextStream error(stderr);
      for (const auto& issue : issues) error << issue << '\n';
      QCoreApplication::exit(issues.isEmpty() ? 0 : 1);
    });
  }
  return application.exec();
}
