#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <QList>
#include <QMainWindow>

#include "core/dynlib.h"
#include "core/scan.h"
#include "core/sweep.h"

using ulpscope::CallConv;
using ulpscope::DynamicLibrary;
using ulpscope::FloatFormat;
using ulpscope::FunctionSummary;
using ulpscope::InputFilter;
using ulpscope::ScanConfig;
using ulpscope::ScanResult;

class BitInspector;
class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTabWidget;
class QTableView;
class UlpPlot;
class SweepModel;
class WorstTableModel;

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

  // --- scripting API -------------------------------------------------------
  // Lets a scan be driven without a mouse, which the self-test and any
  // batch/CI use of this tool need.

  // Loads a shared object. Returns false and fills *error on failure.
  bool openLibrary(const QString &path, QString *error = nullptr);

  // Selects a function by name and re-derives its argument count.
  void selectFunction(const QString &symbol);
  void setCallConv(CallConv conv);

  // Copies the current control values into `config_`.
  void readControls();

  // Runs the configured scan on the calling thread and refreshes the views.
  bool runScanBlocking(QString *error = nullptr);

  // The most recent scan, or nullptr.
  const ScanResult *lastResult() const { return ownedResult_.get(); }

  // Runs a whole-library sweep on the calling thread and shows the ranking.
  void runSweepBlocking();
  void showSweepTab();
  void showScanTab();
  void bringPanelToFront(const QString &name);
  const std::vector<FunctionSummary> &sweepResults() const {
    return sweepResults_;
  }

private:
  void buildUi();
  void buildFilterBox();
  QDockWidget *addPanel(const QString &title, QWidget *widget,
                         Qt::DockWidgetArea area);
  void buildViewMenu(QMenu *viewMenu);
  void saveLayout();
  void restoreLayout();
  void closeEvent(QCloseEvent *e) override;
  void showEvent(QShowEvent *e) override;
  void applyDefaultDockSizes();
  void loadLibrary(const QString &path);
  void onSymbolChanged();
  void onFormatChanged();
  void startScan();
  void startSweep();
  void cancelScan();
  void onScanFinished();
  void onSamplePicked(int index);
  void refreshStatus();
  void probeCurrentInput();
  void updateProbeFromResult();
  QString currentExport(bool litStyle, bool csv) const;
  QString sweepCsv() const;
  // Every exported symbol we can both call and reference. Anything else would
  // either have no reference or a signature we cannot guess.
  std::vector<std::string> sweepableSymbols() const;
  // Formats the current controls describe, for the status line.
  std::string filterSummary() const;

  void setBusy(bool busy);
  // Offers to switch an oversized exhaustive scan to a sample. Returns false if
  // the user declined, in which case the scan must not run.
  bool resolveExhaustiveRequest();
  FloatFormat currentFormat() const;
  // Index into FloatFormat::all() for the format a symbol name implies, or -1.
  int impliedFormatIndex(const std::string &symbol) const;
  int formatIndexByName(const char *name) const;

  // --- state
  DynamicLibrary lib_;
  ScanConfig config_;
  std::unique_ptr<ScanResult> ownedResult_;
  std::vector<FunctionSummary> sweepResults_;
  void *fn_ = nullptr;
  std::thread worker_;
  std::atomic<bool> cancel_{false};
  bool sweeping_ = false;
  // Set once the user picks an ABI by hand, which disables auto-detection.
  bool convChosen_ = false;
  // Track the format a symbol implies, so the inspector and plot follow along
  // instead of silently reporting nonsense for the wrong format.
  bool autoFormat_ = true;
  int autoFormatIndex_ = 0;

  // --- widgets
  QComboBox *formatBox_ = nullptr;
  QPushButton *openButton_ = nullptr;
  QLabel *libLabel_ = nullptr;
  QComboBox *symbolBox_ = nullptr;
  QLineEdit *refEdit_ = nullptr;
  QComboBox *convBox_ = nullptr;
  QSpinBox *argsBox_ = nullptr;
  QLineEdit *arg1_ = nullptr;
  QLineEdit *arg2_ = nullptr;
  QComboBox *argModeBox_ = nullptr;
  QComboBox *modeBox_ = nullptr;
  QSpinBox *sampleBox_ = nullptr;
  QSpinBox *rangeLo_ = nullptr;
  QSpinBox *rangeHi_ = nullptr;
  QCheckBox *logBox_ = nullptr;
  QComboBox *colorBox_ = nullptr;
  QSpinBox *toleranceBox_ = nullptr;
  QSpinBox *exportLimitBox_ = nullptr;
  QPushButton *sweepButton_ = nullptr;
  QPushButton *scanButton_ = nullptr;
  QPushButton *cancelButton_ = nullptr;
  QProgressBar *progress_ = nullptr;
  QLabel *statusLabel_ = nullptr;
  QLabel *summaryLabel_ = nullptr;
  QGroupBox *filterBox_ = nullptr;
  QList<QDockWidget *> panels_;
  // Docks can only be sized once the layout exists, so the default arrangement
  // is applied from showEvent rather than at construction.
  bool defaultSizesApplied_ = false;

  QCheckBox *classChecks_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  QCheckBox *expCheck_ = nullptr;
  QSpinBox *expLo_ = nullptr;
  QSpinBox *expHi_ = nullptr;

  UlpPlot *plot_ = nullptr;
  QTableView *table_ = nullptr;
  QTableView *sweepTable_ = nullptr;
  WorstTableModel *model_ = nullptr;
  SweepModel *sweepModel_ = nullptr;
  BitInspector *inspector_ = nullptr;
};