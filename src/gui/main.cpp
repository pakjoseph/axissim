#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
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
  AxisSimWindow() {
    setWindowTitle("AxisSim Desktop Explorer");
    resize(1'200, 760);

    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    auto* toolbar = new QHBoxLayout();
    scenario_ = new QComboBox(root);
    scenario_->addItem("Baseline Physical AI", "configs/baseline/manifest.conf");
    auto* browse = new QPushButton("Open scenario…", root);
    auto* save_as = new QPushButton("Save As…", root);
    auto* run = new QPushButton("Run scenario", root);
    auto* sweep = new QPushButton("Sweep RVV lanes", root);
    toolbar->addWidget(new QLabel("Scenario:", root));
    toolbar->addWidget(scenario_, 1);
    toolbar->addWidget(browse);
    toolbar->addWidget(save_as);
    toolbar->addWidget(run);
    toolbar->addWidget(sweep);
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
    loadScenario(scenario_->currentData().toString());
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

  void showResult(const QString& json_path, const QString& trace_path, const QString& cli_output) {
    QFile result_file(json_path);
    if (!result_file.open(QIODevice::ReadOnly)) {
      QMessageBox::critical(this, "AxisSim", "The run did not produce a result JSON.");
      return;
    }
    const auto document = QJsonDocument::fromJson(result_file.readAll());
    const QJsonObject result = document.object();
    const QJsonObject latency = result.value("latency").toObject();
    const QJsonObject deadlines = result.value("deadlines").toObject();
    const QJsonObject traffic = result.value("traffic").toObject();
    const QJsonObject energy = result.value("energy").toObject();
    summary_->setPlainText(QString("Scenario: %1 (model %2)\n\nLatency mean / P99: %3 / %4 µs\nVision deadline misses: %5 / %6\nDRAM traffic: %7 MB\nEnergy per frame: %8 µJ\n\n%9")
                               .arg(result.value("scenario_name").toString(), result.value("model_version").toString())
                               .arg(latency.value("mean_us").toDouble(), 0, 'f', 2)
                               .arg(latency.value("p99_us").toDouble(), 0, 'f', 2)
                               .arg(deadlines.value("vision_misses").toInt())
                               .arg(deadlines.value("vision_jobs").toInt())
                               .arg(traffic.value("dram_bytes").toDouble() / 1'000'000.0, 0, 'f', 2)
                               .arg(energy.value("per_frame_uj").toDouble(), 0, 'f', 2)
                               .arg(cli_output));
    populateTrace(trace_path);
    statusBar()->showMessage("Model run complete");
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
  QString scenario_path_;
  std::unique_ptr<QProcess> process_;
  std::unique_ptr<QTemporaryDir> temporary_;
};

}  // namespace

int main(int argc, char** argv) {
  const bool smoke_test = argc == 2 && QString::fromLocal8Bit(argv[1]) == "--smoke-test";
  QApplication application(argc, argv);
  AxisSimWindow window;
  window.show();
  if (smoke_test) QTimer::singleShot(0, &application, &QCoreApplication::quit);
  return application.exec();
}
