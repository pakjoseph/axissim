#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStatusBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <cstddef>
#include <memory>

namespace {

class AxisSimWindow final : public QMainWindow {
 public:
  explicit AxisSimWindow(bool exit_after_run = false) : exit_after_run_(exit_after_run) {
    setWindowTitle("AxisSim Desktop Explorer");
    resize(1'200, 760);

    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    auto* toolbar = new QHBoxLayout();
    scenario_ = new QComboBox(root);
    const QDir project_root(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(".."));
    scenario_->addItem("Baseline Physical AI", project_root.filePath("configs/baseline/manifest.conf"));
    scenario_->addItem("Power-budget Physical AI", project_root.filePath("configs/power_budget/manifest.conf"));
    scenario_->addItem("Deployment & implementation proxies", project_root.filePath("configs/implementation_proxy/manifest.conf"));
    auto* browse = new QPushButton("Open scenario…", root);
    auto* save_as = new QPushButton("Save As…", root);
    auto* run = new QPushButton("Run scenario", root);
    auto* sweep = new QPushButton("Sweep RVV lanes", root);
    auto* ara_check = new QPushButton("Check published Ara2 point", root);
    toolbar->addWidget(new QLabel("Scenario:", root));
    toolbar->addWidget(scenario_, 1);
    toolbar->addWidget(browse);
    toolbar->addWidget(save_as);
    toolbar->addWidget(run);
    toolbar->addWidget(sweep);
    toolbar->addWidget(ara_check);
    layout->addLayout(toolbar);

    auto* splitter = new QSplitter(Qt::Horizontal, root);
    editor_ = new QPlainTextEdit(splitter);
    editor_->setPlaceholderText("Open a scenario manifest to edit it.");
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto* right = new QWidget(splitter);
    auto* right_layout = new QVBoxLayout(right);
    summary_ = new QPlainTextEdit(right);
    summary_->setReadOnly(true);
    summary_->setPlaceholderText("Run a scenario to see modeled latency, deadline, traffic, and energy results.");
    auto* result_views = new QTabWidget(right);
    auto* timeline_page = new QWidget(result_views);
    auto* timeline_layout = new QVBoxLayout(timeline_page);
    trace_ = new QTableWidget(timeline_page);
    trace_->setColumnCount(5);
    trace_->setHorizontalHeaderLabels({"Frame", "Task", "Resource", "Start (µs)", "Duration (µs)"});
    trace_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    timeline_layout->addWidget(trace_);
    result_views->addTab(timeline_page, "First-frame timeline");
    sweep_ = new QTableWidget(result_views);
    sweep_->setColumnCount(4);
    sweep_->setHorizontalHeaderLabels({"RVV lanes", "Mean (µs)", "P99 (µs)", "Energy/frame (µJ)"});
    sweep_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    result_views->addTab(sweep_, "RVV lane sweep");
    metrics_ = new QTableWidget(result_views);
    metrics_->setColumnCount(3);
    metrics_->setHorizontalHeaderLabels({"Metric", "Modeled value", "Interpretation"});
    metrics_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    metrics_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    metrics_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    metrics_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result_views->addTab(metrics_, "Power & deployment");
    proxies_ = new QTableWidget(result_views);
    proxies_->setColumnCount(3);
    proxies_->setHorizontalHeaderLabels({"Proxy", "Modeled value", "Scope / status"});
    proxies_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    proxies_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    proxies_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    proxies_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result_views->addTab(proxies_, "Implementation proxies");
    right_layout->addWidget(new QLabel("Model result", right));
    right_layout->addWidget(summary_, 1);
    right_layout->addWidget(result_views, 2);
    splitter->addWidget(editor_);
    splitter->addWidget(right);
    splitter->setSizes({520, 680});
    layout->addWidget(splitter, 1);
    setCentralWidget(root);

    connect(scenario_, &QComboBox::currentIndexChanged, this, [this](int) { loadScenario(scenario_->currentData().toString()); });
    connect(browse, &QPushButton::clicked, this, [this] { openScenario(); });
    connect(save_as, &QPushButton::clicked, this, [this] { saveAs(); });
    connect(run, &QPushButton::clicked, this, [this] { runScenario(); });
    connect(sweep, &QPushButton::clicked, this, [this] { runLaneSweep(); });
    connect(ara_check, &QPushButton::clicked, this, [this] { runAra2Check(); });
    loadScenario(scenario_->currentData().toString());
    if (exit_after_run_) QTimer::singleShot(0, this, [this] { runScenario(); });
  }

 private:
  void loadScenario(const QString& path) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
      QMessageBox::warning(this, "AxisSim", "Could not open scenario: " + path);
      return;
    }
    editor_->setPlainText(QString::fromUtf8(input.readAll()));
    scenario_path_ = QFileInfo(path).absoluteFilePath();
    editor_->document()->setModified(false);
    statusBar()->showMessage("Loaded " + scenario_path_);
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
      QMessageBox::critical(this, "AxisSim", "Could not write scenario: " + path);
      return false;
    }
    QTextStream stream(&output);
    stream << editor_->toPlainText();
    output.close();
    scenario_path_ = QFileInfo(path).absoluteFilePath();
    editor_->document()->setModified(false);
    statusBar()->showMessage("Saved " + scenario_path_);
    return true;
  }

  void runScenario() {
    if (!ensureSaved()) return;
    const QString executable = QCoreApplication::applicationDirPath() + "/axissim";
    if (!QFileInfo::exists(executable)) {
      if (exit_after_run_) {
        QCoreApplication::exit(1);
        return;
      }
      QMessageBox::critical(this, "AxisSim", "The axissim CLI was not found beside the GUI. Build both targets together.");
      return;
    }
    temporary_ = std::make_unique<QTemporaryDir>();
    const QString json_path = temporary_->filePath("result.json");
    const QString trace_path = temporary_->filePath("trace.csv");
    process_ = std::make_unique<QProcess>(this);
    process_->setWorkingDirectory(QFileInfo(scenario_path_).absolutePath());
    connect(process_.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, json_path, trace_path](int exit_code, QProcess::ExitStatus exit_status) {
              const QString output = QString::fromUtf8(process_->readAllStandardOutput() + process_->readAllStandardError());
              if (exit_status != QProcess::NormalExit || exit_code != 0) {
                if (exit_after_run_) {
                  QCoreApplication::exit(1);
                  return;
                }
                QMessageBox::critical(this, "AxisSim run failed", output);
                return;
              }
              showResult(json_path, trace_path, output);
            });
    process_->start(executable, {"run", scenario_path_, "--json", json_path, "--trace", trace_path});
    statusBar()->showMessage("Running model…");
  }

  bool ensureSaved() {
    if (!editor_->document()->isModified()) return true;
    const auto choice = QMessageBox::question(this, "Save scenario", "The scenario has unsaved edits. Save it before running?");
    return choice == QMessageBox::Yes && saveAs();
  }

  void runLaneSweep() {
    if (!ensureSaved()) return;
    const QString executable = QCoreApplication::applicationDirPath() + "/axissim";
    if (!QFileInfo::exists(executable)) {
      QMessageBox::critical(this, "AxisSim", "The axissim CLI was not found beside the GUI. Build both targets together.");
      return;
    }
    process_ = std::make_unique<QProcess>(this);
    process_->setWorkingDirectory(QFileInfo(scenario_path_).absolutePath());
    connect(process_.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
              const QString output = QString::fromUtf8(process_->readAllStandardOutput() + process_->readAllStandardError());
              if (exit_status != QProcess::NormalExit || exit_code != 0) {
                QMessageBox::critical(this, "AxisSim sweep failed", output);
                return;
              }
              populateSweep(output);
              statusBar()->showMessage("RVV lane sweep complete");
            });
    process_->start(executable, {"sweep", scenario_path_, "--sweep", "rvv_lanes=2,4,8,16,32"});
    statusBar()->showMessage("Sweeping RVV lanes…");
  }

  void runAra2Check() {
    const QString executable = QCoreApplication::applicationDirPath() + "/axissim";
    const QDir project_root(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(".."));
    const QString config = project_root.filePath("configs/baseline/manifest.conf");
    const QString reference = project_root.filePath("reference_data/ara/ara2_published_latency.csv");
    if (!QFileInfo::exists(executable) || !QFileInfo::exists(config) || !QFileInfo::exists(reference)) {
      QMessageBox::critical(this, "AxisSim", "The checked-in CLI, calibration config, or Ara2 reference CSV was not found. Run from the CMake build directory.");
      return;
    }
    process_ = std::make_unique<QProcess>(this);
    process_->setWorkingDirectory(project_root.absolutePath());
    connect(process_.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
              const QString output = QString::fromUtf8(process_->readAllStandardOutput() + process_->readAllStandardError());
              if (exit_status != QProcess::NormalExit || exit_code != 0) {
                QMessageBox::critical(this, "Ara2 reference check failed", output);
                return;
              }
              summary_->setPlainText("Published Ara2 compute-only consistency check\n\n" + output +
                                     "\nThis compares a normalized vector-FMA stream to a value derived from Table IV of the Ara2 paper. It is not an end-to-end Ara or power-model validation.");
              statusBar()->showMessage("Published Ara2 reference check complete");
            });
    process_->start(executable, {"calibrate", config, "--reference", reference});
    statusBar()->showMessage("Checking published Ara2 reference…");
  }

  void showResult(const QString& json_path, const QString& trace_path, const QString& cli_output) {
    QFile result_file(json_path);
    if (!result_file.open(QIODevice::ReadOnly)) {
      if (exit_after_run_) {
        QCoreApplication::exit(1);
        return;
      }
      QMessageBox::critical(this, "AxisSim", "The run did not produce a result JSON.");
      return;
    }
    const auto document = QJsonDocument::fromJson(result_file.readAll());
    const QJsonObject result = document.object();
    const QJsonObject latency = result.value("latency").toObject();
    const QJsonObject deadlines = result.value("deadlines").toObject();
    const QJsonObject traffic = result.value("traffic").toObject();
    const QJsonObject energy = result.value("energy").toObject();
    const QJsonObject power = result.value("power").toObject();
    const QJsonObject deployment = result.value("deployment").toObject();
    const bool deployment_enabled = deployment.value("hours").toDouble() > 0.0;
    const QString deployment_summary = deployment_enabled
        ? QString("Deployment facility energy / electricity cost: %1 kWh / $%2")
              .arg(deployment.value("facility_energy_kwh").toDouble(), 0, 'f', 3)
              .arg(deployment.value("electricity_cost").toDouble(), 0, 'f', 2)
        : QString("Deployment estimate: not configured");
    summary_->setPlainText(QString("Scenario: %1 (model %2)\n\nLatency mean / P99: %3 / %4 µs\nVision deadline misses: %5 / %6\nDRAM traffic: %7 MB\nDynamic energy per frame: %8 µJ\nAverage IT / facility power: %9 / %10 W\n%11\n\nAll values are model outputs. Area, thermal, and reliability tabs are bounded assumption-driven proxies, not implementation signoff.\n\n%12")
                               .arg(result.value("scenario_name").toString(), result.value("model_version").toString())
                               .arg(latency.value("mean_us").toDouble(), 0, 'f', 2)
                               .arg(latency.value("p99_us").toDouble(), 0, 'f', 2)
                               .arg(deadlines.value("vision_misses").toInt())
                               .arg(deadlines.value("vision_jobs").toInt())
                               .arg(traffic.value("dram_bytes").toDouble() / 1'000'000.0, 0, 'f', 2)
                               .arg(energy.value("per_frame_uj").toDouble(), 0, 'f', 2)
                               .arg(power.value("average_it_w").toDouble(), 0, 'f', 4)
                               .arg(power.value("average_facility_w").toDouble(), 0, 'f', 4)
                               .arg(deployment_summary)
                               .arg(cli_output));
    populateMetrics(result);
    populateProxies(result);
    populateTrace(trace_path);
    statusBar()->showMessage("Model run complete");
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
    }
  }

  QComboBox* scenario_ = nullptr;
  QPlainTextEdit* editor_ = nullptr;
  QPlainTextEdit* summary_ = nullptr;
  QTableWidget* trace_ = nullptr;
  QTableWidget* sweep_ = nullptr;
  QTableWidget* metrics_ = nullptr;
  QTableWidget* proxies_ = nullptr;
  QString scenario_path_;
  std::unique_ptr<QProcess> process_;
  std::unique_ptr<QTemporaryDir> temporary_;
  bool exit_after_run_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  const bool smoke_test = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--smoke-test";
  const bool smoke_test_run = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--smoke-test-run";
  QApplication application(argc, argv);
  AxisSimWindow window(smoke_test_run);
  window.show();
  if (smoke_test) QTimer::singleShot(0, &application, &QCoreApplication::quit);
  return application.exec();
}
