#include "charts.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonDocument>
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
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>

namespace {

class KpiCard final : public QFrame {
 public:
  KpiCard(const QString& title, const QString& caption, QWidget* parent = nullptr) : QFrame(parent) {
    setObjectName("kpiCard");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 11, 14, 11);
    layout->setSpacing(3);
    auto* title_label = new QLabel(title.toUpper(), this);
    title_label->setObjectName("kpiTitle");
    value_ = new QLabel("—", this);
    value_->setObjectName("kpiValue");
    caption_ = new QLabel(caption, this);
    caption_->setObjectName("kpiCaption");
    layout->addWidget(title_label);
    layout->addWidget(value_);
    layout->addWidget(caption_);
  }

  void setValue(const QString& value, const QString& caption = {}) {
    value_->setText(value);
    if (!caption.isEmpty()) caption_->setText(caption);
  }

 private:
  QLabel* value_ = nullptr;
  QLabel* caption_ = nullptr;
};

class AxisSimWindow final : public QMainWindow {
 public:
  explicit AxisSimWindow(bool exit_after_run = false) : exit_after_run_(exit_after_run) {
    project_root_ = findProjectRoot();
    setWindowTitle("AxisSim — AI Architecture Explorer");
    setWindowIcon(QIcon(":/icons/axissim.png"));
    resize(1'440, 920);
    setMinimumSize(1'080, 720);
    buildInterface();
    buildMenus();
    restoreWindowState();
    loadScenario(scenario_->currentData().toString());
    if (exit_after_run_) QTimer::singleShot(0, this, [this] { runScenario(); });
  }

 protected:
  void closeEvent(QCloseEvent* event) override {
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/splitter", splitter_->saveState());
    QMainWindow::closeEvent(event);
  }

 private:
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
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(20, 18, 20, 14);
    layout->setSpacing(12);

    auto* header = new QFrame(root);
    header->setObjectName("headerPanel");
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(20, 14, 20, 14);
    auto* title_layout = new QVBoxLayout();
    title_layout->setSpacing(1);
    auto* title = new QLabel("AxisSim", header);
    title->setObjectName("appTitle");
    auto* subtitle = new QLabel("Workload-driven AI architecture, power, and implementation sensitivity explorer", header);
    subtitle->setObjectName("appSubtitle");
    title_layout->addWidget(title);
    title_layout->addWidget(subtitle);
    header_layout->addLayout(title_layout);
    header_layout->addStretch();
    progress_ = new QProgressBar(header);
    progress_->setRange(0, 0);
    progress_->setFixedWidth(120);
    progress_->setTextVisible(false);
    progress_->hide();
    status_pill_ = new QLabel("Ready", header);
    status_pill_->setObjectName("statusPill");
    status_pill_->setToolTip("Current simulator activity");
    header_layout->addWidget(progress_);
    header_layout->addWidget(status_pill_);
    layout->addWidget(header);

    auto* controls = new QFrame(root);
    controls->setObjectName("controlPanel");
    auto* controls_layout = new QVBoxLayout(controls);
    controls_layout->setContentsMargins(16, 13, 16, 13);
    controls_layout->setSpacing(10);
    auto* scenario_row = new QHBoxLayout();
    scenario_row->setSpacing(8);
    scenario_row->addWidget(mutedLabel("SCENARIO", controls));
    scenario_ = new QComboBox(controls);
    scenario_->setMinimumWidth(280);
    scenario_->addItem("Baseline Physical AI", project_root_.filePath("configs/baseline/manifest.conf"));
    scenario_->addItem("Fixed power-budget study", project_root_.filePath("configs/power_budget/manifest.conf"));
    scenario_->addItem("Deployment & implementation proxies", project_root_.filePath("configs/implementation_proxy/manifest.conf"));
    scenario_->setToolTip("Choose a checked-in scenario or open your own manifest");
    scenario_row->addWidget(scenario_, 1);
    browse_button_ = new QPushButton("Open…", controls);
    save_button_ = new QPushButton("Save As…", controls);
    export_button_ = new QPushButton("Export JSON…", controls);
    export_button_->setEnabled(false);
    scenario_row->addWidget(browse_button_);
    scenario_row->addWidget(save_button_);
    scenario_row->addWidget(export_button_);
    controls_layout->addLayout(scenario_row);

    auto* experiment_row = new QHBoxLayout();
    experiment_row->setSpacing(8);
    experiment_row->addWidget(mutedLabel("QUICK EXPERIMENT", controls));
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
    experiment_row->addWidget(rvv_lanes_);
    experiment_row->addWidget(npu_macs_);
    experiment_row->addWidget(dram_bandwidth_);
    experiment_row->addWidget(arithmetic_scale_);
    experiment_row->addStretch();
    sweep_button_ = new QPushButton("Lane sweep", controls);
    ara_button_ = new QPushButton("Ara2 check", controls);
    run_button_ = new QPushButton("Run model", controls);
    run_button_->setObjectName("primaryButton");
    run_button_->setToolTip("Run the selected scenario with any explicit quick overrides (⌘R)");
    experiment_row->addWidget(sweep_button_);
    experiment_row->addWidget(ara_button_);
    experiment_row->addWidget(run_button_);
    controls_layout->addLayout(experiment_row);
    layout->addWidget(controls);

    splitter_ = new QSplitter(Qt::Horizontal, root);
    auto* editor_panel = new QFrame(splitter_);
    editor_panel->setObjectName("editorPanel");
    auto* editor_layout = new QVBoxLayout(editor_panel);
    editor_layout->setContentsMargins(14, 13, 14, 14);
    auto* editor_title_row = new QHBoxLayout();
    auto* editor_title = new QLabel("Scenario manifest", editor_panel);
    editor_title->setObjectName("sectionTitle");
    scenario_path_label_ = mutedLabel("", editor_panel);
    scenario_path_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    editor_title_row->addWidget(editor_title);
    editor_title_row->addWidget(scenario_path_label_, 1);
    editor_layout->addLayout(editor_title_row);
    auto* editor_help = mutedLabel("Edit advanced parameters directly. Quick experiment controls are temporary overrides and never rewrite this file.", editor_panel);
    editor_help->setWordWrap(true);
    editor_layout->addWidget(editor_help);
    editor_ = new QPlainTextEdit(editor_panel);
    editor_->setPlaceholderText("Open a scenario manifest to edit it.");
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_layout->addWidget(editor_, 1);

    auto* content_panel = new QFrame(splitter_);
    content_panel->setObjectName("contentPanel");
    auto* content_layout = new QVBoxLayout(content_panel);
    content_layout->setContentsMargins(14, 13, 14, 14);
    auto* results_title = new QLabel("Architecture results", content_panel);
    results_title->setObjectName("sectionTitle");
    content_layout->addWidget(results_title);

    auto* kpi_grid = new QGridLayout();
    kpi_grid->setContentsMargins(0, 2, 0, 2);
    kpi_grid->setHorizontalSpacing(8);
    kpi_grid->setVerticalSpacing(8);
    latency_card_ = new KpiCard("Mean latency", "End-to-end per frame", content_panel);
    p99_card_ = new KpiCard("P99 latency", "Tail latency", content_panel);
    energy_card_ = new KpiCard("IT energy", "Per frame", content_panel);
    power_card_ = new KpiCard("Average IT power", "Modeled window", content_panel);
    deadline_card_ = new KpiCard("Deadline misses", "Vision jobs", content_panel);
    traffic_card_ = new KpiCard("DRAM traffic", "Complete run", content_panel);
    kpi_grid->addWidget(latency_card_, 0, 0);
    kpi_grid->addWidget(p99_card_, 0, 1);
    kpi_grid->addWidget(energy_card_, 0, 2);
    kpi_grid->addWidget(power_card_, 1, 0);
    kpi_grid->addWidget(deadline_card_, 1, 1);
    kpi_grid->addWidget(traffic_card_, 1, 2);
    content_layout->addLayout(kpi_grid);

    result_views_ = new QTabWidget(content_panel);
    auto* overview_page = new QWidget(result_views_);
    auto* overview_layout = new QVBoxLayout(overview_page);
    overview_layout->setContentsMargins(12, 12, 12, 12);
    energy_chart_ = new EnergyBreakdownWidget(overview_page);
    overview_layout->addWidget(energy_chart_);
    metrics_ = new QTableWidget(overview_page);
    metrics_->setColumnCount(3);
    metrics_->setHorizontalHeaderLabels({"Metric", "Modeled value", "Interpretation"});
    metrics_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    metrics_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    metrics_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    configureTable(metrics_);
    overview_layout->addWidget(metrics_, 1);
    auto* scope_note = mutedLabel("Model outputs are scenario-dependent estimates. Power, area, thermal, deployment, and reliability values are only as valid as their explicit input assumptions.", overview_page);
    scope_note->setWordWrap(true);
    overview_layout->addWidget(scope_note);
    result_views_->addTab(overview_page, "Overview");

    auto* timeline_page = new QWidget(result_views_);
    auto* timeline_layout = new QVBoxLayout(timeline_page);
    trace_ = new QTableWidget(timeline_page);
    trace_->setColumnCount(5);
    trace_->setHorizontalHeaderLabels({"Frame", "Task", "Resource", "Start (µs)", "Duration (µs)"});
    trace_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    configureTable(trace_);
    timeline_layout->addWidget(trace_);
    result_views_->addTab(timeline_page, "Timeline");

    auto* sweep_page = new QWidget(result_views_);
    auto* sweep_layout = new QVBoxLayout(sweep_page);
    sweep_chart_ = new SweepChartWidget(sweep_page);
    sweep_layout->addWidget(sweep_chart_, 2);
    sweep_ = new QTableWidget(sweep_page);
    sweep_->setColumnCount(4);
    sweep_->setHorizontalHeaderLabels({"RVV lanes", "Mean (µs)", "P99 (µs)", "Energy/frame (µJ)"});
    sweep_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    configureTable(sweep_);
    sweep_layout->addWidget(sweep_, 1);
    result_views_->addTab(sweep_page, "Lane scaling");

    auto* proxies_page = new QWidget(result_views_);
    auto* proxies_layout = new QVBoxLayout(proxies_page);
    auto* proxies_note = mutedLabel("Bounded sensitivity proxies only—not floorplanning, thermal signoff, or predicted silicon reliability.", proxies_page);
    proxies_note->setWordWrap(true);
    proxies_layout->addWidget(proxies_note);
    proxies_ = new QTableWidget(proxies_page);
    proxies_->setColumnCount(3);
    proxies_->setHorizontalHeaderLabels({"Proxy", "Modeled value", "Scope / status"});
    proxies_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    proxies_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    proxies_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    configureTable(proxies_);
    proxies_layout->addWidget(proxies_);
    result_views_->addTab(proxies_page, "Implementation proxies");

    log_ = new QPlainTextEdit(result_views_);
    log_->setReadOnly(true);
    log_->setPlaceholderText("Run a scenario to inspect the complete CLI output and validation notes.");
    log_tab_index_ = result_views_->addTab(log_, "Run log");
    content_layout->addWidget(result_views_, 1);

    splitter_->addWidget(editor_panel);
    splitter_->addWidget(content_panel);
    splitter_->setStretchFactor(0, 2);
    splitter_->setStretchFactor(1, 5);
    splitter_->setSizes({430, 920});
    layout->addWidget(splitter_, 1);
    setCentralWidget(root);

    connect(scenario_, &QComboBox::currentIndexChanged, this, [this](int index) { changeScenario(index); });
    connect(browse_button_, &QPushButton::clicked, this, [this] { openScenario(); });
    connect(save_button_, &QPushButton::clicked, this, [this] { saveAs(); });
    connect(export_button_, &QPushButton::clicked, this, [this] { exportResult(); });
    connect(run_button_, &QPushButton::clicked, this, [this] { runScenario(); });
    connect(sweep_button_, &QPushButton::clicked, this, [this] { runLaneSweep(); });
    connect(ara_button_, &QPushButton::clicked, this, [this] { runAra2Check(); });
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
    sweep_action_ = run_menu->addAction("Sweep RVV Lanes");
    sweep_action_->setShortcut(QKeySequence("Ctrl+Shift+R"));
    connect(sweep_action_, &QAction::triggered, this, [this] { runLaneSweep(); });
    ara_action_ = run_menu->addAction("Check Published Ara2 Point");
    connect(ara_action_, &QAction::triggered, this, [this] { runAra2Check(); });

    auto* help_menu = menuBar()->addMenu("Help");
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
    statusBar()->showMessage(message, success ? 8'000 : 12'000);
  }

  void setBusy(bool busy, const QString& message) {
    progress_->setVisible(busy);
    status_pill_->setText(message);
    scenario_->setEnabled(!busy);
    browse_button_->setEnabled(!busy);
    save_button_->setEnabled(!busy);
    run_button_->setEnabled(!busy);
    sweep_button_->setEnabled(!busy);
    ara_button_->setEnabled(!busy);
    rvv_lanes_->setEnabled(!busy);
    npu_macs_->setEnabled(!busy);
    dram_bandwidth_->setEnabled(!busy);
    arithmetic_scale_->setEnabled(!busy);
    open_action_->setEnabled(!busy);
    save_action_->setEnabled(!busy);
    run_action_->setEnabled(!busy);
    sweep_action_->setEnabled(!busy);
    ara_action_->setEnabled(!busy);
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

    latency_card_->setValue(QString::number(latency.value("mean_us").toDouble(), 'f', 2) + " µs");
    p99_card_->setValue(QString::number(latency.value("p99_us").toDouble(), 'f', 2) + " µs");
    energy_card_->setValue(QString::number(energy.value("it_per_frame_uj").toDouble(), 'f', 2) + " µJ",
                           QString("%1 µJ dynamic").arg(energy.value("per_frame_uj").toDouble(), 0, 'f', 2));
    power_card_->setValue(QString::number(power.value("average_it_w").toDouble(), 'f', 4) + " W",
                          power.value("budget_w").toDouble() > 0.0 ? (power.value("feasible").toBool() ? "Within configured budget" : "Budget exceeded") : "No IT budget configured");
    deadline_card_->setValue(QString("%1 / %2").arg(deadlines.value("vision_misses").toInt()).arg(frames), "Vision deadline outcomes");
    traffic_card_->setValue(QString::number(traffic.value("dram_bytes").toDouble() / 1'000'000.0, 'f', 2) + " MB");

    energy_chart_->setSegments({
        {"Vector", energy.value("vector_pj").toDouble() / frames / 1'000'000.0, QColor("#38bdf8")},
        {"NPU arithmetic", energy.value("npu_arithmetic_pj").toDouble() / frames / 1'000'000.0, QColor("#a78bfa")},
        {"DRAM", energy.value("dram_pj").toDouble() / frames / 1'000'000.0, QColor("#fb923c")},
        {"DMA", energy.value("dma_pj").toDouble() / frames / 1'000'000.0, QColor("#34d399")},
        {"Static", energy.value("static_per_frame_uj").toDouble(), QColor("#f472b6")},
    });
    populateMetrics(result);
    populateProxies(result);
    populateTrace(trace_path);
    log_->setPlainText(QString("Scenario: %1  •  model %2  •  result schema %3\n\n%4\n\n"
                               "Interpretation boundary\n"
                               "AxisSim reports model outputs under explicit assumptions. Area, thermal, and FIT reliability values are sensitivity proxies—not implementation signoff or measured hardware.")
                           .arg(result.value("scenario_name").toString(), result.value("model_version").toString(), result.value("schema_version").toString(), cli_output));
    export_button_->setEnabled(true);
    export_action_->setEnabled(true);
    result_views_->setCurrentIndex(0);
    finishProcess(true, power.value("budget_w").toDouble() > 0.0 && !power.value("feasible").toBool() ? "Complete • budget exceeded" : "Model complete");
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
    const QJsonObject timing = result.value("timing").toObject();
    const QJsonObject energy = result.value("energy").toObject();
    const QJsonObject power = result.value("power").toObject();
    const QJsonObject deployment = result.value("deployment").toObject();
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
    }
  }

  void populateSweep(const QString& output) {
    sweep_->setRowCount(0);
    QVector<SweepPoint> points;
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (qsizetype index = 1; index < lines.size(); ++index) {
      const QStringList fields = lines[index].split(',');
      if (fields.size() < 9) continue;
      const int row = sweep_->rowCount();
      sweep_->insertRow(row);
      constexpr std::array<int, 4> source_columns = {3, 4, 5, 8};
      for (int column = 0; column < 4; ++column) {
        sweep_->setItem(row, column, new QTableWidgetItem(fields[source_columns[static_cast<std::size_t>(column)]]));
      }
      bool lanes_ok = false;
      bool latency_ok = false;
      const double lanes = fields[3].toDouble(&lanes_ok);
      const double latency = fields[4].toDouble(&latency_ok);
      if (lanes_ok && latency_ok) points.push_back({lanes, latency});
    }
    sweep_chart_->setPoints(std::move(points));
  }

  QDir project_root_;
  QComboBox* scenario_ = nullptr;
  QComboBox* rvv_lanes_ = nullptr;
  QComboBox* npu_macs_ = nullptr;
  QComboBox* dram_bandwidth_ = nullptr;
  QComboBox* arithmetic_scale_ = nullptr;
  QPushButton* browse_button_ = nullptr;
  QPushButton* save_button_ = nullptr;
  QPushButton* export_button_ = nullptr;
  QPushButton* run_button_ = nullptr;
  QPushButton* sweep_button_ = nullptr;
  QPushButton* ara_button_ = nullptr;
  QAction* export_action_ = nullptr;
  QAction* open_action_ = nullptr;
  QAction* save_action_ = nullptr;
  QAction* run_action_ = nullptr;
  QAction* sweep_action_ = nullptr;
  QAction* ara_action_ = nullptr;
  QLabel* status_pill_ = nullptr;
  QLabel* scenario_path_label_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QSplitter* splitter_ = nullptr;
  QPlainTextEdit* editor_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QTabWidget* result_views_ = nullptr;
  QTableWidget* trace_ = nullptr;
  QTableWidget* sweep_ = nullptr;
  QTableWidget* metrics_ = nullptr;
  QTableWidget* proxies_ = nullptr;
  EnergyBreakdownWidget* energy_chart_ = nullptr;
  SweepChartWidget* sweep_chart_ = nullptr;
  KpiCard* latency_card_ = nullptr;
  KpiCard* p99_card_ = nullptr;
  KpiCard* energy_card_ = nullptr;
  KpiCard* power_card_ = nullptr;
  KpiCard* deadline_card_ = nullptr;
  KpiCard* traffic_card_ = nullptr;
  int log_tab_index_ = 0;
  int previous_scenario_index_ = 0;
  QString scenario_path_;
  QByteArray last_result_json_;
  std::unique_ptr<QProcess> process_;
  std::unique_ptr<QTemporaryDir> temporary_;
  bool exit_after_run_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  const bool smoke_test = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--smoke-test";
  const bool smoke_test_run = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--smoke-test-run";
  QApplication application(argc, argv);
  Q_INIT_RESOURCE(gui);
  QCoreApplication::setOrganizationName("AxisSim");
  QCoreApplication::setApplicationName("AxisSim Desktop Explorer");
  QCoreApplication::setApplicationVersion("0.2");
  application.setWindowIcon(QIcon(":/icons/axissim.png"));
  QFile stylesheet(":/theme/dark.qss");
  if (stylesheet.open(QIODevice::ReadOnly | QIODevice::Text)) application.setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
  AxisSimWindow window(smoke_test_run);
  window.show();
  if (smoke_test) QTimer::singleShot(0, &application, &QCoreApplication::quit);
  return application.exec();
}
