#include "ui/mainwindow.h"

#include <string>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QCloseEvent>
#include <QFrame>
#include <QScrollArea>
#include <QDockWidget>
#include <QMenu>
#include <QProgressBar>
#include <QSettings>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

#include "core/exporter.h"
#include "core/mpfrref.h"
#include "core/sweep.h"
#include "ui/bitinspector.h"
#include "ui/formatting.h"
#include "ui/sweeptable.h"
#include "ui/ulpplot.h"
#include "ui/worsttable.h"

using namespace ulpscope;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  buildUi();
  restoreLayout();
#ifndef ULPSCOPE_DEMO_SO
  const QString demo = QStringLiteral("ulpscope_demo");
#else
  const QString demo = QStringLiteral(ULPSCOPE_DEMO_SO);
#endif
  loadLibrary(demo);
}

MainWindow::~MainWindow() {
  cancel_.store(true);
  if (worker_.joinable())
    worker_.join();
}

// Creates a dockable panel and records it for the View menu. The object name is
// what QMainWindow::saveState keys off, so it has to be stable and unique.
QDockWidget *MainWindow::addPanel(const QString &title, QWidget *widget,
                                  Qt::DockWidgetArea area) {
  auto *dock = new QDockWidget(title, this);
  dock->setObjectName(title);
  dock->setWidget(widget);
  // Everything movable and floatable: a panel the user cannot undock onto a
  // second monitor, or hide when they want the plot full-screen, is not really
  // dockable.
  dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                    QDockWidget::DockWidgetClosable);
  dock->setAllowedAreas(Qt::AllDockWidgetAreas);
  addDockWidget(area, dock);
  panels_.append(dock);
  return dock;
}

void MainWindow::buildViewMenu(QMenu *viewMenu) {
  // One checkable toggle per panel, kept in step with the dock itself so that
  // closing a panel from its title bar updates the menu.
  for (QDockWidget *dock : panels_) {
    auto *action = dock->toggleViewAction();
    action->setText(dock->windowTitle());
    viewMenu->addAction(action);
  }
  viewMenu->addSeparator();

  auto *reset = viewMenu->addAction(QStringLiteral("Reset layout"));
  connect(reset, &QAction::triggered, this, [this] {
    for (QDockWidget *dock : panels_)
      dock->setVisible(true);
    if (auto *options = findChild<QDockWidget *>(QStringLiteral("Scan options")))
      if (auto *filter = findChild<QDockWidget *>(QStringLiteral("Input filter")))
        tabifyDockWidget(options, filter);
    if (auto *plot = findChild<QDockWidget *>(QStringLiteral("ULP plot")))
      if (auto *results = findChild<QDockWidget *>(QStringLiteral("Results")))
        splitDockWidget(plot, results, Qt::Vertical);
    if (auto *results = findChild<QDockWidget *>(QStringLiteral("Results")))
      if (auto *sweep = findChild<QDockWidget *>(QStringLiteral("Library sweep")))
        tabifyDockWidget(results, sweep);
    resizeDocks({findChild<QDockWidget *>(QStringLiteral("Scan options"))},
                {620}, Qt::Horizontal);
    resizeDocks({findChild<QDockWidget *>(QStringLiteral("Input filter"))},
                {620}, Qt::Horizontal);
    saveLayout();
  });

  auto *floatAll = viewMenu->addAction(QStringLiteral("Float all panels"));
  floatAll->setToolTip(QStringLiteral(
      "Undock every panel into its own window, so a scan can be spread across "
      "two monitors."));
  connect(floatAll, &QAction::triggered, this, [this] {
    for (QDockWidget *dock : panels_)
      dock->setFloating(true);
  });
}

void MainWindow::saveLayout() {
  QSettings settings;
  settings.setValue(QStringLiteral("layout"), saveState());
  settings.setValue(QStringLiteral("geometry"), saveGeometry());
}

void MainWindow::restoreLayout() {
  QSettings settings;
  const QByteArray state = settings.value(QStringLiteral("layout")).toByteArray();
  if (state.isEmpty())
    return; // first run: keep the arrangement buildUi chose
  restoreState(state);
  const QByteArray geom = settings.value(QStringLiteral("geometry")).toByteArray();
  if (!geom.isEmpty())
    restoreGeometry(geom);
}

void MainWindow::closeEvent(QCloseEvent *e) {
  saveLayout();
  QMainWindow::closeEvent(e);
}

void MainWindow::showEvent(QShowEvent *e) {
  QMainWindow::showEvent(e);
  // resizeDocks is a request, and before the first real layout the dock areas
  // have no sizes yet, so requests against them are discarded. Applying the
  // default arrangement here, after the window has been laid out, is what makes
  // it stick.
  if (!defaultSizesApplied_) {
    defaultSizesApplied_ = true;
    // Deferred to the event loop on purpose: showEvent is emitted before the
    // first layout pass, so sizing the docks here is still too early.
    QMetaObject::invokeMethod(this, [this] { applyDefaultDockSizes(); },
                              Qt::QueuedConnection);
  }
}

void MainWindow::applyDefaultDockSizes() {
  auto find = [this](const char *name) {
    return findChild<QDockWidget *>(QString::fromLatin1(name));
  };
  if (auto *options = find("Scan options"))
    resizeDocks({options}, {660}, Qt::Horizontal);
  if (auto *plot = find("ULP plot"))
    if (auto *results = find("Results"))
      resizeDocks({plot, results}, {100000, 55000}, Qt::Vertical);
  if (auto *inspector = find("Bit inspector")) {
    resizeDocks({inspector}, {340}, Qt::Horizontal);
    // Fill the height of the column too; otherwise it sits at its minimum
    // height with a large empty gap below it.
    resizeDocks({inspector}, {100000}, Qt::Vertical);
  }
}

void MainWindow::buildUi() {
  setWindowTitle(QStringLiteral("ulpscope"));
  resize(1180, 820);

  // ---- top control bar ----------------------------------------------------
  auto *bar = addToolBar(QStringLiteral("controls"));
  bar->setMovable(false);

  bar->addWidget(new QLabel(QStringLiteral(" Format: "), this));
  formatBox_ = new QComboBox(this);
  // Keyed by index, not by width: bf16 and f16 are both 16-bit, so anything
  // derived from totalBits would silently pick the wrong format.
  const auto &formats = FloatFormat::all();
  for (int i = 0; i < static_cast<int>(formats.size()); ++i)
    formatBox_->addItem(QString::fromLatin1(formats[i].name), i);
  formatBox_->setToolTip(
      QStringLiteral("bf16 and f16 are both 16 bits wide and have completely "
                     "different encodings."));
  bar->addWidget(formatBox_);

  openButton_ = new QPushButton(QStringLiteral("Open .so…"), this);
  bar->addWidget(openButton_);
  libLabel_ = new QLabel(this);
  libLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  bar->addWidget(libLabel_);

  bar->addWidget(new QLabel(QStringLiteral(" Function: "), this));
  symbolBox_ = new QComboBox(this);
  symbolBox_->setMinimumWidth(180);
  symbolBox_->setEditable(true);
  bar->addWidget(symbolBox_);

  bar->addWidget(new QLabel(QStringLiteral(" ABI: "), this));
  convBox_ = new QComboBox(this);
  convBox_->addItem(QString::fromLatin1(callConvName(CallConv::Native16)),
                    static_cast<int>(CallConv::Native16));
  convBox_->addItem(QString::fromLatin1(callConvName(CallConv::Double)),
                    static_cast<int>(CallConv::Double));
  convBox_->addItem(QString::fromLatin1(callConvName(CallConv::Float)),
                    static_cast<int>(CallConv::Float));
  convBox_->addItem(QString::fromLatin1(callConvName(CallConv::Integer8)),
                    static_cast<int>(CallConv::Integer8));
  // From here on the ABI is the user's decision, and auto-detection stops.
  convBox_->setCurrentIndex(0);
  convBox_->setToolTip(QStringLiteral(
      "Inferred from the function name; pick one explicitly to override it."));
  bar->addWidget(convBox_);

  argsBox_ = new QSpinBox(this);
  argsBox_->setRange(1, 3);
  bar->addWidget(new QLabel(QStringLiteral(" Args: "), this));
  bar->addWidget(argsBox_);

  bar->addWidget(new QLabel(QStringLiteral(" Reference: "), this));
  refEdit_ = new QLineEdit(this);
  refEdit_->setPlaceholderText(QStringLiteral("auto"));
  refEdit_->setMaximumWidth(110);
  refEdit_->setToolTip(QStringLiteral(
      "Which function this is an implementation of, when the name does not "
      "say so. For example log2_viaf32 is log2. Leave empty to infer it."));
  bar->addWidget(refEdit_);

  bar->addSeparator();
  scanButton_ = new QPushButton(QStringLiteral("Scan"), this);
  bar->addWidget(scanButton_);
  sweepButton_ = new QPushButton(QStringLiteral("Scan all functions"), this);
  sweepButton_->setToolTip(QStringLiteral(
      "Check every exported function that ulpscope can both call and "
      "reference, and rank them by error."));
  bar->addWidget(sweepButton_);
  cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
  cancelButton_->setEnabled(false);
  bar->addWidget(cancelButton_);

  progress_ = new QProgressBar(this);
  progress_->setMaximumWidth(180);
  bar->addWidget(progress_);

  // ---- panels ------------------------------------------------------------
  // Every view is a dock widget: movable, floatable, hideable, and remembered
  // across runs. The scan options are laid out in a grid rather than one long
  // row, because in a row the labels end up squeezed against their controls and
  // the strip overflows - which is what it was doing before.

  // Dock areas are arranged *around* the central widget. A bare QWidget with a
  // zero minimum gets squeezed out entirely, and Qt then hands the whole middle
  // row to the right-hand dock - which makes a right-side panel render as a
  // full-width band and squeezes the plot. The minimum width is what forces a
  // real centre to exist.
  //
  // There is nothing to put *in* the centre, so it is collapsed vertically to
  // give that space to the plot. The width stays expanding, because that is what
  // keeps the right-hand column narrow.
  auto *central = new QWidget(this);
  central->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  central->setMinimumWidth(360);
  central->setFixedHeight(0);
  setCentralWidget(central);

  // Scan options.
  auto *options = new QWidget(this);
  auto *optLayout = new QGridLayout(options);
  optLayout->setContentsMargins(8, 6, 8, 6);
  optLayout->setHorizontalSpacing(8);
  optLayout->setVerticalSpacing(4);

  auto addLabel = [&](int row, int col, const QString &text) {
    auto *l = new QLabel(text, options);
    optLayout->addWidget(l, row, col);
  };

  modeBox_ = new QComboBox(options);
  modeBox_->addItem(QStringLiteral("Exhaustive"), static_cast<int>(ScanMode::Exhaustive));
  modeBox_->addItem(QStringLiteral("Random sample"),
                    static_cast<int>(ScanMode::Random));
  modeBox_->addItem(QStringLiteral("Range"), static_cast<int>(ScanMode::Range));

  sampleBox_ = new QSpinBox(options);
  sampleBox_->setRange(1, 4000000000ll);
  sampleBox_->setValue(1 << 20);

  rangeLo_ = new QSpinBox(options);
  rangeLo_->setRange(0, 0x7FFFFFFF);
  rangeLo_->setDisplayIntegerBase(16);
  rangeLo_->setPrefix(QStringLiteral("0x"));
  rangeHi_ = new QSpinBox(options);
  rangeHi_->setRange(0, 0x7FFFFFFF);
  rangeHi_->setDisplayIntegerBase(16);
  rangeHi_->setPrefix(QStringLiteral("0x"));
  rangeHi_->setValue(0x4000);

  argModeBox_ = new QComboBox(options);
  argModeBox_->addItem(QStringLiteral("fixed"), static_cast<int>(ArgMode::Fixed));
  argModeBox_->addItem(QStringLiteral("randomised"),
                       static_cast<int>(ArgMode::Randomised));
  argModeBox_->addItem(QStringLiteral("neighbouring"),
                       static_cast<int>(ArgMode::Neighbouring));
  argModeBox_->setToolTip(QStringLiteral(
      "How the second and later arguments are chosen. \"Neighbouring\" steps "
      "them around the first argument, which is what makes fmin, fmax, fdim "
      "and copysign testable: their defects live at equality and at the "
      "signed-zero boundary."));

  arg1_ = new QLineEdit(QStringLiteral("2.0"), options);
  arg1_->setMaximumWidth(90);
  arg2_ = new QLineEdit(QStringLiteral("0.0"), options);
  arg2_->setMaximumWidth(90);
  arg2_->setEnabled(false);

  toleranceBox_ = new QSpinBox(options);
  toleranceBox_->setRange(0, 1000000);
  toleranceBox_->setToolTip(QStringLiteral(
      "Largest ULP error still counted as correct. 0 demands a correctly "
      "rounded result everywhere, which most functions cannot achieve."));

  logBox_ = new QCheckBox(QStringLiteral("log scale"), options);
  logBox_->setChecked(true);

  colorBox_ = new QComboBox(options);
  colorBox_->addItem(QStringLiteral("result"), 0);
  colorBox_->addItem(QStringLiteral("input class"), 1);

  exportLimitBox_ = new QSpinBox(options);
  exportLimitBox_->setRange(1, 1000000);
  exportLimitBox_->setValue(1000);
  exportLimitBox_->setToolTip(
      QStringLiteral("How many failing cases an export includes."));

  int r = 0;
  addLabel(r, 0, QStringLiteral("Inputs:"));
  optLayout->addWidget(modeBox_, r, 1);
  addLabel(r, 2, QStringLiteral("n ="));
  optLayout->addWidget(sampleBox_, r, 3);
  addLabel(r, 4, QStringLiteral("range"));
  optLayout->addWidget(rangeLo_, r, 5);
  optLayout->addWidget(new QLabel(QStringLiteral("to"), options), r, 6);
  optLayout->addWidget(rangeHi_, r, 7);

  ++r;
  addLabel(r, 0, QStringLiteral("Args:"));
  optLayout->addWidget(argModeBox_, r, 1);
  addLabel(r, 2, QStringLiteral("fixed"));
  optLayout->addWidget(arg1_, r, 3);
  optLayout->addWidget(arg2_, r, 4);

  ++r;
  addLabel(r, 0, QStringLiteral("Tolerance:"));
  optLayout->addWidget(toleranceBox_, r, 1);
  optLayout->addWidget(logBox_, r, 2, 1, 2);
  addLabel(r, 4, QStringLiteral("colour:"));
  optLayout->addWidget(colorBox_, r, 5);
  addLabel(r, 6, QStringLiteral("export <= "));
  optLayout->addWidget(exportLimitBox_, r, 7);

  ++r;
  summaryLabel_ = new QLabel(options);
  summaryLabel_->setTextFormat(Qt::RichText);
  optLayout->addWidget(summaryLabel_, r, 0, 1, 8);

  // This dock holds a few rows and a summary line. Left unbounded it absorbs
  // every spare pixel, which starves the plot that the tool exists to show.
  options->setMaximumHeight(150);

  connect(modeBox_, &QComboBox::currentIndexChanged, this, [this] {
    const auto m = static_cast<ScanMode>(modeBox_->currentData().toInt());
    sampleBox_->setEnabled(m != ScanMode::Range);
    const bool ranged = m == ScanMode::Range;
    rangeLo_->setEnabled(ranged);
    rangeHi_->setEnabled(ranged);
  });

  // ---- views --------------------------------------------------------------
  plot_ = new UlpPlot(this);
  plot_->setToolTip(QStringLiteral(
      "Wheel zooms, left-drag pans, right-click or double-click resets the "
      "view, click selects a point."));

  model_ = new WorstTableModel(this);
  table_ = new QTableView(this);
  table_->setModel(model_);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->verticalHeader()->setVisible(false);
  table_->setAlternatingRowColors(true);
  table_->setSortingEnabled(true);
  // setSortingEnabled makes the view sort by its own default (column 0), which
  // would override the model's intended "worst first" ordering.
  table_->sortByColumn(WorstTableModel::ColUlpError, Qt::DescendingOrder);

  sweepModel_ = new SweepModel(this);
  sweepTable_ = new QTableView(this);
  sweepTable_->setModel(sweepModel_);
  sweepTable_->setSortingEnabled(true);
  sweepTable_->verticalHeader()->setVisible(false);
  sweepTable_->setAlternatingRowColors(true);
  sweepTable_->sortByColumn(SweepModel::ColMaxUlp, Qt::DescendingOrder);
  sweepTable_->setToolTip(
      QStringLiteral("Double-click a row to load that function and its scan."));

  // ---- make them all docks -------------------------------------------------
  buildFilterBox();
  filterBox_->setMaximumHeight(70);

  auto *optionsDock = addPanel(QStringLiteral("Scan options"), options,
                               Qt::TopDockWidgetArea);
  auto *filterDock =
      addPanel(QStringLiteral("Input filter"), filterBox_, Qt::TopDockWidgetArea);
  auto *plotDock = addPanel(QStringLiteral("ULP plot"), plot_,
                            Qt::BottomDockWidgetArea);
  auto *resultsDock =
      addPanel(QStringLiteral("Results"), table_, Qt::BottomDockWidgetArea);
  auto *sweepDock = addPanel(QStringLiteral("Library sweep"), sweepTable_,
                             Qt::BottomDockWidgetArea);

  // Results below the plot, with the sweep tabbed onto results: the plot stays
  // visible, and the two table views are one click apart.
  splitDockWidget(plotDock, resultsDock, Qt::Vertical);
  tabifyDockWidget(resultsDock, sweepDock);

  // Default arrangement, in case there is nothing saved to restore. The plot
  // is what the tool is for, so it gets the bulk of the space.
  resizeDocks({optionsDock, filterDock}, {90, 90}, Qt::Vertical);
  resizeDocks({plotDock, resultsDock}, {100000, 60000}, Qt::Vertical);
  resizeDocks({optionsDock, filterDock}, {620, 620}, Qt::Horizontal);
  for (QDockWidget *d : panels_)
    d->setVisible(true);

  inspector_ = new BitInspector(this);
  inspector_->setFormat(config_.fmt);
  // The inspector prints a lot of text, so its natural height is large enough
  // to swallow a dock area and squeeze out the plot. Scrolling it lets the
  // user give it as little or as much room as they want.
  auto *inspectorScroll = new QScrollArea(this);
  inspectorScroll->setWidget(inspector_);
  inspectorScroll->setWidgetResizable(true);
  inspectorScroll->setFrameShape(QFrame::NoFrame);
  inspectorScroll->setMinimumWidth(280);

  auto *inspectorDock = addPanel(QStringLiteral("Bit inspector"), inspectorScroll,
                                 Qt::RightDockWidgetArea);
  resizeDocks({inspectorDock}, {340}, Qt::Horizontal);

  // Hiding the dock must hide the widget it holds, or the scan keeps writing
  // into an invisible panel.
  connect(inspectorDock, &QDockWidget::visibilityChanged, inspector_,
          &QWidget::setVisible);

  // Picking a format by hand disables the name-driven default, since the user
  // is deliberately overriding it.
  connect(formatBox_, &QComboBox::activated, this,
          [this] { autoFormat_ = false; });

  // ---- menus --------------------------------------------------------------
  auto *fileMenu = menuBar()->addMenu(QStringLiteral("&Library"));
  fileMenu->addAction(QStringLiteral("&Open shared object…"), this,
                      [this] {
                        const QString path = QFileDialog::getOpenFileName(
                            this, QStringLiteral("Open shared object"), QString(),
                            QStringLiteral("Shared objects (*.so *.dylib *.dll);;All files (*)"));
                        if (!path.isEmpty())
                          loadLibrary(path);
                      });
  fileMenu->addSeparator();
  fileMenu->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this,
                      &QWidget::close);

  auto *scanMenu = menuBar()->addMenu(QStringLiteral("&Scan"));
  scanMenu->addAction(QStringLiteral("&Run"), QKeySequence(QStringLiteral("Ctrl+R")),
                      this, &MainWindow::startScan);
  scanMenu->addAction(QStringLiteral("&Cancel"), QKeySequence(QStringLiteral("Esc")),
                      this, &MainWindow::cancelScan);
  scanMenu->addSeparator();
  scanMenu->addAction(QStringLiteral("Sweep the whole &library"), this,
                      &MainWindow::startSweep);

  // Panels are created by buildUi() before this runs, so the list is complete.
  auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
  buildViewMenu(viewMenu);

  auto *exportMenu = menuBar()->addMenu(QStringLiteral("&Export"));
  auto addExport = [&](const QString &title, bool lit, bool csv) {
    exportMenu->addAction(title, this, [this, lit, csv, title] {
      const QString text = currentExport(lit, csv);
      if (text.isEmpty())
        return;
      QApplication::clipboard()->setText(text);
      statusLabel_->setText(QStringLiteral("%1: %2 characters copied to the clipboard")
                                .arg(title, QString::number(text.size())));
    });
  };
  addExport(QStringLiteral("Failing inputs as a C test table…"), false, false);
  addExport(QStringLiteral("Failing inputs as a lit test…"), true, false);
  addExport(QStringLiteral("Worst cases as CSV…"), false, true);

  exportMenu->addSeparator();
  exportMenu->addAction(QStringLiteral("Whole-library sweep as CSV…"), this,
                        [this] {
                          const QString text = sweepCsv();
                          if (text.isEmpty())
                            return;
                          QApplication::clipboard()->setText(text);
                          statusLabel_->setText(
                              QStringLiteral("sweep CSV: %1 characters copied to the clipboard")
                                  .arg(text.size()));
                        });

  statusLabel_ = new QLabel(QStringLiteral("ready"), this);
  statusBar()->addWidget(statusLabel_);

  // ---- connections --------------------------------------------------------
  connect(openButton_, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open shared object"), QString(),
        QStringLiteral("Shared objects (*.so *.dylib *.dll);;All files (*)"));
    if (!path.isEmpty())
      loadLibrary(path);
  });
  connect(scanButton_, &QPushButton::clicked, this, &MainWindow::startScan);
  connect(sweepButton_, &QPushButton::clicked, this, &MainWindow::startSweep);
  connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelScan);
  connect(colorBox_, &QComboBox::currentIndexChanged, this, [this] {
    plot_->setColorMode(colorBox_->currentData().toInt() == 1
                            ? UlpPlot::ColorMode::InputClass
                            : UlpPlot::ColorMode::Result);
  });
  connect(symbolBox_, &QComboBox::currentTextChanged, this,
          &MainWindow::onSymbolChanged);
  connect(convBox_, &QComboBox::activated, this, [this](int index) {
    Q_UNUSED(index);
    // activated fires only for direct user interaction, unlike
    // currentIndexChanged, which also fires when auto-detection moves the
    // combo. Only a real choice should disable auto-detection.
    convChosen_ = true;
    onSymbolChanged();
  });
  connect(logBox_, &QCheckBox::toggled, plot_, &UlpPlot::setLogScale);
  connect(plot_, &UlpPlot::samplePicked, this, &MainWindow::onSamplePicked);
  connect(inspector_, &BitInspector::bitsChanged, this,
          [this](uint64_t) { probeCurrentInput(); });
  connect(argsBox_, &QSpinBox::valueChanged, this, [this](int n) {
    arg1_->setEnabled(n >= 2);
    arg2_->setEnabled(n >= 3);
    onSymbolChanged();
  });
  connect(modeBox_, &QComboBox::currentIndexChanged, this, [this] {
    sampleBox_->setEnabled(modeBox_->currentData().toInt() ==
                           static_cast<int>(ScanMode::Random));
  });
  connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this](const QModelIndex &current, const QModelIndex &) {
            const int idx = model_->sampleIndexForRow(current.row());
            if (idx >= 0) {
              plot_->setSelectedIndex(idx);
              onSamplePicked(idx);
            }
          });

  // Double-clicking a sweep row should load that function and its scan, so the
  // two views are one workflow rather than two.
  connect(sweepTable_, &QTableView::doubleClicked, this,
          [this](const QModelIndex &index) {
            const FunctionSummary *s = sweepModel_->at(index.row());
            if (!s || !s->ok)
              return;
            showScanTab();
            selectFunction(QString::fromStdString(s->symbol));
            runScanBlocking();
          });
}

bool MainWindow::openLibrary(const QString &path, QString *error) {
  std::string err;
  if (!lib_.load(path.toStdString(), &err)) {
    if (error)
      *error = QString::fromStdString(err);
    return false;
  }
  loadLibrary(path);
  return true;
}

void MainWindow::selectFunction(const QString &symbol) {
  const int i = symbolBox_->findText(symbol);
  if (i >= 0)
    symbolBox_->setCurrentIndex(i);
  else
    symbolBox_->setCurrentText(symbol); // editable, so a typed name still works
  onSymbolChanged();
}

void MainWindow::setCallConv(CallConv conv) {
  const int i = convBox_->findData(static_cast<int>(conv));
  if (i >= 0)
    convBox_->setCurrentIndex(i);
  onSymbolChanged();
}

void MainWindow::readControls() {
  config_.fmt = currentFormat();
  config_.conv = static_cast<CallConv>(convBox_->currentData().toInt());
  config_.nargs = argsBox_->value();
  config_.symbol = symbolBox_->currentText().trimmed().toStdString();
  config_.mode = static_cast<ScanMode>(modeBox_->currentData().toInt());
  config_.sampleCount = static_cast<uint64_t>(sampleBox_->value());
  config_.lo = static_cast<uint64_t>(rangeLo_->value());
  config_.hi = static_cast<uint64_t>(rangeHi_->value());
  config_.fixedArgs[0] = arg1_->text().toDouble();
  config_.fixedArgs[1] = arg2_->text().toDouble();
  config_.argMode = static_cast<ArgMode>(argModeBox_->currentData().toInt());
  config_.referenceBase = refEdit_->text().trimmed().toStdString();
  config_.toleranceUlp = static_cast<uint64_t>(toleranceBox_->value());

  config_.filter = InputFilter{};
  config_.filter.includeZero = classChecks_[0]->isChecked();
  config_.filter.includeNormal = classChecks_[1]->isChecked();
  config_.filter.includeSubnormal = classChecks_[2]->isChecked();
  config_.filter.includeInfinity = classChecks_[3]->isChecked();
  config_.filter.includeNaN = classChecks_[4]->isChecked();
  config_.filter.restrictExponent = expCheck_->isChecked();
  config_.filter.minExponent = expLo_->value();
  config_.filter.maxExponent = expHi_->value();
}

std::string MainWindow::filterSummary() const { return config_.filter.describe(); }

bool MainWindow::runScanBlocking(QString *error) {
  if (worker_.joinable())
    worker_.join();
  readControls();
  if (!lib_.isOpen() || !fn_) {
    if (error)
      *error = QStringLiteral("no library or function selected");
    return false;
  }
  // No prompting here: this path is for scripts and tests. Fall back to a
  // sample instead of running an impossible scan.
  if (config_.wouldExhaustMemory())
    config_.mode = ScanMode::Random;

  auto result = Scanner::run(config_, lib_, fn_);
  ownedResult_.reset(new ScanResult(std::move(result)));
  onScanFinished();
  const ScanResult *r = ownedResult_.get();
  if (error && (!r->ok || r->cancelled))
    *error = r->cancelled ? QStringLiteral("cancelled")
                          : QString::fromStdString(r->error);
  return r->ok && !r->cancelled;
}

void MainWindow::loadLibrary(const QString &path) {
  std::string err;
  if (!lib_.load(path.toStdString(), &err)) {
    QMessageBox::warning(
        this, QStringLiteral("Could not load library"),
        QStringLiteral("%1\n\n%2").arg(path, QString::fromStdString(err)));
    statusLabel_->setText(QStringLiteral("load failed: %1").arg(path));
    return;
  }

  libLabel_->setText(QFileInfo(path).fileName());
  libLabel_->setToolTip(path);

  const QString previous = symbolBox_->currentText();
  {
    QSignalBlocker block(symbolBox_);
    symbolBox_->clear();
    // Offer everything the library exports, but lead with the names we can
    // actually compute a reference for, since those are the useful ones.
    std::vector<std::string> good, other;
    for (const std::string &s : lib_.symbols()) {
      if (!MpfrRef::resolveBase(s).empty())
        good.push_back(s);
      else
        other.push_back(s);
    }
    for (const auto &s : good)
      symbolBox_->addItem(QString::fromStdString(s));
    if (!other.empty() && !good.empty())
      symbolBox_->insertSeparator(good.size());
    for (const auto &s : other)
      symbolBox_->addItem(QString::fromStdString(s));
    if (const int i = symbolBox_->findText(previous); i >= 0)
      symbolBox_->setCurrentIndex(i);
  }

  // Count the library's own symbols we can actually judge, not the size of the
  // reference registry.
  int withReference = 0;
  for (const std::string &s : lib_.symbols())
    if (!MpfrRef::resolveBase(s).empty())
      ++withReference;

  statusLabel_->setText(QStringLiteral("loaded %1: %2 exported symbols, %3 of them have an MPFR reference")
                            .arg(QFileInfo(path).fileName())
                            .arg(lib_.symbols().size())
                            .arg(withReference));
  onSymbolChanged();
}

// Delegates to the core, so the sweep and this window cannot disagree about
// what a symbol name means.
int MainWindow::impliedFormatIndex(const std::string &symbol) const {
  return FloatFormat::indexImpliedByName(symbol);
}

int MainWindow::formatIndexByName(const char *name) const {
  return FloatFormat::indexByName(name);
}

FloatFormat MainWindow::currentFormat() const {
  const auto &formats = FloatFormat::all();
  const int i = formatBox_->currentIndex();
  if (i < 0 || i >= static_cast<int>(formats.size()))
    return FloatFormat::bf16();
  return formats[i];
}

// The filter controls what a scan visits, so they belong with the scan options
// rather than in the inspector.
void MainWindow::buildFilterBox() {
  filterBox_ = new QGroupBox(QStringLiteral("Filter inputs"), this);
  auto *layout = new QHBoxLayout(filterBox_);
  layout->setSpacing(10);

  // Indexed to match Class, so these checkboxes and ScanResult's per-class
  // counters cannot drift apart.
  static const char *names[5] = {"zero", "normal", "denormal", "inf", "nan"};
  for (int i = 0; i < 5; ++i) {
    classChecks_[i] = new QCheckBox(QString::fromLatin1(names[i]), filterBox_);
    classChecks_[i]->setChecked(true);
    layout->addWidget(classChecks_[i]);
  }

  layout->addSpacing(8);
  layout->addWidget(new QLabel(QStringLiteral("exponent:"), filterBox_));
  expCheck_ = new QCheckBox(QStringLiteral("limit"), filterBox_);
  layout->addWidget(expCheck_);
  expLo_ = new QSpinBox(filterBox_);
  expLo_->setValue(-10);
  expLo_->setEnabled(false);
  layout->addWidget(expLo_);
  layout->addWidget(new QLabel(QStringLiteral("to"), filterBox_));
  expHi_ = new QSpinBox(filterBox_);
  expHi_->setValue(10);
  expHi_->setEnabled(false);
  layout->addWidget(expHi_);

  layout->addStretch(1);

  // The usable exponent range depends on the format, so keep the boxes inside
  // it rather than letting the user pick a range that contains no inputs.
  const auto syncRange = [this] {
    const FloatFormat f = currentFormat();
    const int lo = 1 - f.bias - f.mantBits;
    const int hi = static_cast<int>(f.expMask()) - 1 - f.bias;
    const bool wasEnabled = expLo_->isEnabled();
    expLo_->setRange(lo, hi);
    expHi_->setRange(lo, hi);
    if (!wasEnabled) {
      expLo_->setEnabled(false);
      expHi_->setEnabled(false);
    }
  };
  connect(formatBox_, &QComboBox::currentIndexChanged, this, [this, syncRange] {
    autoFormatIndex_ = formatBox_->currentIndex();
    config_.fmt = currentFormat();
    inspector_->setFormat(config_.fmt);
    syncRange();
    refreshStatus();
  });
  connect(expCheck_, &QCheckBox::toggled, expLo_, &QWidget::setEnabled);
  connect(expCheck_, &QCheckBox::toggled, expHi_, &QWidget::setEnabled);
  syncRange();
}

// The results and sweep views are tabs of one dock now, so "show this one"
// means raising the dock and selecting its tab.
void MainWindow::showSweepTab() {
  bringPanelToFront(QStringLiteral("Library sweep"));
}

void MainWindow::showScanTab() {
  bringPanelToFront(QStringLiteral("Results"));
}

void MainWindow::bringPanelToFront(const QString &name) {
  auto *dock = findChild<QDockWidget *>(name);
  if (!dock)
    return;
  dock->setVisible(true);
  dock->raise();
  // A docked widget is only a tab if something else shares its area; raise()
  // is enough when it is on its own.
  if (auto *area = dock->parentWidget())
    area->raise();
}

void MainWindow::onFormatChanged() {
  config_.fmt = currentFormat();
  inspector_->setFormat(config_.fmt);
  refreshStatus();
}

void MainWindow::runSweepBlocking() {
  if (!lib_.isOpen())
    return;
  readControls();
  const std::vector<std::string> symbols = sweepableSymbols();
  if (symbols.empty())
    return;
  sweeping_ = true;
  sweepResults_ = LibrarySweep::run(lib_.path(), symbols, config_);
  sweeping_ = false;
  sweepModel_->setResults(sweepResults_);
  showSweepTab();
  statusLabel_->setText(
      QStringLiteral("swept %1 functions").arg(sweepResults_.size()));
}

void MainWindow::onSymbolChanged() {
  const QString symbol = symbolBox_->currentText().trimmed();
  config_.symbol = symbol.toStdString();
  config_.conv = static_cast<CallConv>(convBox_->currentData().toInt());

  // A function called "sqrtbf16" is a bf16 function. Scanning it as f16 would
  // report every input as wrong rather than failing, so the format follows the
  // name unless the user has picked one themselves.
  if (autoFormat_ && !symbol.isEmpty()) {
    const int idx = impliedFormatIndex(symbol.toStdString());
    if (idx >= 0 && idx != formatBox_->currentIndex())
      formatBox_->setCurrentIndex(idx);
  }

  // Keep the ABI in step with the name, but only when the user has not already
  // chosen one. A function called "sqrtf" almost certainly takes a float, and
  // calling it as if it took bf16 gives garbage rather than an error - so this
  // is a convenience that yields to an explicit choice.
  if (!convChosen_ && !symbol.isEmpty()) {
    const CallConv guess = ulpscope::guessCallConv(symbol.toStdString());
    if (guess != CallConv::Unknown) {
      const int i = convBox_->findData(static_cast<int>(guess));
      if (i >= 0)
        convBox_->setCurrentIndex(i);
    }
  }

  if (!symbol.isEmpty() && lib_.isOpen()) {
    void *p = nullptr;
    if (lib_.resolve(symbol.toStdString(), &p)) {
      fn_ = p;
      const int inferred = DynamicLibrary::inferArgCount(symbol.toStdString());
      if (inferred > 0 && argsBox_->value() != inferred)
        argsBox_->setValue(inferred);
    } else {
      fn_ = nullptr;
    }
  } else {
    fn_ = nullptr;
  }
  config_.nargs = argsBox_->value();

  const CallConv c = config_.conv;
  convBox_->setToolTip(QString::fromLatin1(
      c == CallConv::Native16
          ? callConvDescription(CallConv::Native16)
          : c == CallConv::Double ? callConvDescription(CallConv::Double)
                                  : callConvDescription(CallConv::Float)));

  // Warn when we cannot say what the correct answer is, rather than silently
  // reporting every input as a mismatch. The scan refuses outright now, but
  // the status line should say why before the user waits for a run.
  const std::string wantRef =
      config_.referenceBase.empty() ? config_.symbol : config_.referenceBase;
  if (!wantRef.empty() &&
      MpfrRef::resolveBase(wantRef).empty()) {
    statusLabel_->setText(QStringLiteral(
        "no MPFR reference for '%1'. Pick a known function in the Reference "
        "field (e.g. sqrt, lgamma, log2), or rename the symbol to a known one.")
                               .arg(QString::fromStdString(wantRef)));
  } else {
    refreshStatus();
  }
}

// Every exported symbol we can both call and reference. Anything else would
// either have no reference or a signature we cannot guess.
std::vector<std::string> MainWindow::sweepableSymbols() const {
  std::vector<std::string> out;
  for (const std::string &sym : lib_.symbols()) {
    if (MpfrRef::resolveBase(sym).empty())
      continue;
    if (DynamicLibrary::inferArgCount(sym) <= 0)
      continue; // unknown arity: calling it would be a guess
    out.push_back(sym);
  }
  return out;
}

void MainWindow::startSweep() {
  if (worker_.joinable())
    worker_.join();
  if (!lib_.isOpen()) {
    QMessageBox::information(this, QStringLiteral("Nothing to sweep"),
                             QStringLiteral("Load a shared object first."));
    return;
  }

  const std::vector<std::string> symbols = sweepableSymbols();
  if (symbols.empty()) {
    QMessageBox::information(
        this, QStringLiteral("Nothing to sweep"),
        QStringLiteral("No exported symbol in this library names a function "
                       "ulpscope has an MPFR reference for.\n\n"
                       "ulpscope recognises names like sqrtbf16, lgammaf, "
                       "exp_f16 by stripping the type suffix."));
    return;
  }

  readControls();
  const auto answer = QMessageBox::question(
      this, QStringLiteral("Sweep the whole library"),
      QStringLiteral("About to check %1 function(s) over %2.\n\n"
                     "This is the exhaustive check on the formats where it is "
                     "cheap, and a sample on the rest.")
          .arg(symbols.size())
          .arg(QString::fromStdString(config_.describeRange())),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  if (answer != QMessageBox::Yes)
    return;

  cancel_.store(false);
  sweeping_ = true;
  setBusy(true);
  showSweepTab();
  progress_->setRange(0, static_cast<int>(symbols.size()));
  progress_->setValue(0);
  progress_->setVisible(true);
  statusLabel_->setText(QStringLiteral("sweeping %1 functions…").arg(symbols.size()));

  const std::string path = lib_.path();
  const ScanConfig cfg = config_;
  worker_ = std::thread([this, path, symbols, cfg] {
    auto *results = new std::vector<FunctionSummary>(
        LibrarySweep::run(
            path, symbols, cfg,
            [this](const FunctionSummary &s, int index, int total) {
              QMetaObject::invokeMethod(
                  this,
                  [this, s, index, total] {
                    progress_->setValue(index);
                    statusLabel_->setText(
                        QStringLiteral("[%1/%2] %3: %4")
                            .arg(index)
                            .arg(total)
                            .arg(QString::fromStdString(s.symbol),
                                 s.ok ? (s.mismatchCount == 0
                                             ? QStringLiteral("correct")
                                             : QStringLiteral("%1 mismatches, worst %2 ULP")
                                                   .arg(s.mismatchCount)
                                                   .arg(s.maxUlp))
                                      : QString::fromStdString(s.error)));
                  },
                  Qt::QueuedConnection);
            },
            &cancel_));

    QMetaObject::invokeMethod(
        this,
        [this, results] {
          sweepResults_ = std::move(*results);
          delete results;
          sweeping_ = false;
          onScanFinished();
        },
        Qt::QueuedConnection);
  });
}

// An exhaustive scan of f32 or f64 would need more memory than the machine has.
// Offer a sample, and mean it: answering Yes switches mode and runs, answering
// No aborts. Running anyway is not an option.
bool MainWindow::resolveExhaustiveRequest() {
  if (!config_.wouldExhaustMemory())
    return true;

  const QMessageBox::StandardButton answer = QMessageBox::question(
      this, QStringLiteral("That is a lot of inputs"),
      QStringLiteral("%1 has %2 inputs. Scanning all of them would need more "
                     "memory than this machine has.\n\nSwitch to a random sample "
                     "of %3 inputs instead?")
          .arg(QString::fromLatin1(config_.fmt.name))
          .arg(config_.fmt.inputCount())
          .arg(sampleBox_->value()),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

  if (answer != QMessageBox::Yes) {
    statusLabel_->setText(QStringLiteral("scan cancelled: %1 has %2 inputs")
                              .arg(QString::fromLatin1(config_.fmt.name))
                              .arg(config_.fmt.inputCount()));
    return false;
  }

  modeBox_->setCurrentIndex(modeBox_->findData(
      static_cast<int>(ScanMode::Random))); // triggers readControls via Scan
  readControls();
  return true;
}

void MainWindow::startScan() {
  if (worker_.joinable())
    worker_.join();
  if (!lib_.isOpen() || !fn_) {
    QMessageBox::information(this, QStringLiteral("Nothing to scan"),
                             QStringLiteral("Load a shared object and pick a function first."));
    return;
  }

  readControls();

  if (!resolveExhaustiveRequest())
    return;

  // An empty filter would silently "pass" everything by visiting nothing. Ask
  // the counting helper rather than enumerating: enumerating an exhaustive f64
  // scan to answer this question is how the machine dies.
  if (countAdmittedInputs(config_, 1) == 0) {
    QMessageBox::warning(
        this, QStringLiteral("Filter matches nothing"),
        QStringLiteral("The current filter excludes every %1 input.")
            .arg(QString::fromLatin1(config_.fmt.name)));
    return;
  }

  cancel_.store(false);
  setBusy(true);
  progress_->setRange(0, 0); // indeterminate until we know the total
  progress_->setVisible(true);
  statusLabel_->setText(QStringLiteral("scanning…"));

  // The scan runs on its own thread; progress and completion come back through
  // queued invocations so the UI thread never blocks.
  worker_ = std::thread([this] {
    auto *result = new ScanResult;
    *result = Scanner::run(
        config_, lib_, fn_,
        [this](uint64_t done, uint64_t total) {
          QMetaObject::invokeMethod(
              this,
              [this, done, total] {
                if (progress_->maximum() == 0)
                  progress_->setRange(0, static_cast<int>(std::min<uint64_t>(total, 1u << 30)));
                progress_->setValue(static_cast<int>(done));
              },
              Qt::QueuedConnection);
        },
        &cancel_);

    QMetaObject::invokeMethod(
        this,
        [this, result] {
          ownedResult_.reset(result);
          onScanFinished();
        },
        Qt::QueuedConnection);
  });
}

void MainWindow::cancelScan() {
  if (worker_.joinable()) {
    cancel_.store(true);
    statusLabel_->setText(QStringLiteral("cancelling…"));
  }
}

void MainWindow::onScanFinished() {
  if (worker_.joinable())
    worker_.join();
  setBusy(false);
  progress_->setVisible(false);

  if (sweeping_) {
    sweepModel_->setResults(sweepResults_);
    uint64_t broken = 0, total = 0;
    for (const FunctionSummary &s : sweepResults_) {
      if (!s.ok)
        continue;
      total++;
      if (s.mismatchCount)
        broken++;
    }
    statusLabel_->setText(QStringLiteral(
        "swept %1 function(s): %2 not correctly rounded, %3 clean")
                               .arg(sweepResults_.size())
                               .arg(broken)
                               .arg(total - broken));
    summaryLabel_->setText(
        QStringLiteral("double-click a row to scan that function in detail"));
    return;
  }

  const ScanResult *r = ownedResult_.get();
  if (!r)
    return;

  if (!r->ok) {
    if (r->cancelled) {
      statusLabel_->setText(QStringLiteral("scan cancelled"));
      return;
    }
    QMessageBox::warning(this, QStringLiteral("Scan failed"),
                         QString::fromStdString(r->error));
    statusLabel_->setText(QStringLiteral("scan failed: %1")
                              .arg(QString::fromStdString(r->error)));
    return;
  }

  plot_->setResult(r);
  model_->setResult(r);
  updateProbeFromResult();
  refreshStatus();
}

void MainWindow::onSamplePicked(int index) {
  const ScanResult *r = ownedResult_.get();
  if (!r || index < 0 || index >= static_cast<int>(r->samples.size()))
    return;
  const Sample &s = r->samples[index];
  inspector_->setBits(s.inputBits);
  inspector_->setProbe(true, s.actual, s.expected, s.ulpError, s.nanDisagreement);

  // Keep the table selection in step when the click came from the plot.
  const int row = model_->rowForSampleIndex(index);
  if (row >= 0 && table_->currentIndex().row() != row) {
    QSignalBlocker block(table_->selectionModel());
    table_->setCurrentIndex(model_->index(row, 0));
    table_->scrollTo(model_->index(row, 0));
  }
}

void MainWindow::probeCurrentInput() {
  const ScanResult *r = ownedResult_.get();
  // Prefer showing the scanned result for this exact input when we have one.
  if (r) {
    for (const Sample &s : r->samples) {
      if (s.inputBits == inspector_->bits()) {
        inspector_->setProbe(true, s.actual, s.expected, s.ulpError,
                              s.nanDisagreement);
        return;
      }
    }
  }
  inspector_->setProbe(false, 0, 0, 0, false);
}

void MainWindow::updateProbeFromResult() {
  if (ownedResult_)
    probeCurrentInput();
}

void MainWindow::refreshStatus() {
  const ScanResult *r = ownedResult_.get();
  if (!r || !r->ok) {
    summaryLabel_->setText(QString());
    return;
  }
  const QString fmtName = QString::fromLatin1(r->config.fmt.name);
  const QString bad = QStringLiteral("<span style='color:#c02020'>%1</span>")
                          .arg(r->mismatchCount);

  QString text =
      QStringLiteral("%1 · %2 inputs · %3 mismatches · worst %4 UPL at %5")
          .arg(fmtName)
          .arg(r->total)
          .arg(bad)
          .arg(r->maxUlp)
          .arg(formatBitPattern(r->maxUlpAtBits, r->config.fmt));

  if (r->config.toleranceUlp)
    text += QStringLiteral(" (tol %1)").arg(r->config.toleranceUlp);
  if (!r->config.filter.isEverything())
    text += QStringLiteral("<br><span style='color:#666'>filter: %1</span>")
                .arg(QString::fromStdString(r->config.filter.describe()));

  // Name the class that actually holds the errors: a defect confined to
  // denormals and one spread across all normals need different fixes.
  uint64_t worstClass = 0;
  for (int i = 1; i < 5; ++i)
    if (r->mismatchesPerClass[i] > r->mismatchesPerClass[worstClass])
      worstClass = static_cast<uint64_t>(i);
  if (r->mismatchCount && r->mismatchesPerClass[worstClass] > 0) {
    text += QStringLiteral("<br><span style='color:#666'>worst class: %1 (%2)</span>")
                .arg(QString::fromLatin1(className(static_cast<Class>(worstClass))),
                     QString::number(r->mismatchesPerClass[worstClass]));
  }

  text += QStringLiteral("<br><span style='color:#666'>mean %1 · rms %2 · >1 ULP: %3 · "
                        ">16 ULP: %4</span>")
              .arg(r->meanUlp, 0, 'f', 4)
              .arg(r->rmsUlp, 0, 'f', 4)
              .arg(r->overUlp[0])
              .arg(r->overUlp[3]);

  // Say so when the filter left less to scan than was asked for. Reporting a
  // short scan as a success is the kind of quiet lie this tool exists to avoid.
  if (!r->inputSetComplete)
    text += QStringLiteral("<br><span style='color:#c06000'>only %1 of %2 requested "
                           "inputs exist</span>")
                .arg(r->total)
                .arg(r->requestedInputs);

  summaryLabel_->setText(text);
}

QString MainWindow::sweepCsv() const {
  if (sweepResults_.empty()) {
    QMessageBox::information(const_cast<MainWindow *>(this),
                             QStringLiteral("Nothing to export"),
                             QStringLiteral("Run \"Scan all functions\" first."));
    return {};
  }
  QString out =
      QStringLiteral("function,reference,format,inputs,mismatches,mismatch_pct,"
                     "max_ulp,rms_ulp,zero,denormal,normal,inf,nan\n");
  for (const FunctionSummary &s : sweepResults_) {
    if (!s.ok) {
      out += QStringLiteral("%1,,%2,,,%3\n")
                 .arg(QString::fromStdString(s.symbol),
                      QString::fromStdString(s.format),
                      QString::fromStdString(s.error));
      continue;
    }
    const double pct =
        s.total ? 100.0 * static_cast<double>(s.mismatchCount) /
                      static_cast<double>(s.total)
                : 0.0;
    out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13\n")
               .arg(QString::fromStdString(s.symbol),
                    QString::fromStdString(s.baseName),
                    QString::fromStdString(s.format))
               .arg(s.total)
               .arg(s.mismatchCount)
               .arg(QString::number(pct, 'f', 4))
               .arg(s.maxUlp)
               .arg(QString::number(s.rmsUlp, 'f', 6))
               .arg(s.mismatchesPerClass[0])
               .arg(s.mismatchesPerClass[1])
               .arg(s.mismatchesPerClass[2])
               .arg(s.mismatchesPerClass[3])
               .arg(s.mismatchesPerClass[4]);
  }
  return out;
}

QString MainWindow::currentExport(bool litStyle, bool csv) const {
  const ScanResult *r = ownedResult_.get();
  if (!r || !r->ok) {
    // currentExport is const, but asking the user to run a scan is worth it.
    QMessageBox::information(const_cast<MainWindow *>(this),
                             QStringLiteral("Nothing to export"),
                             QStringLiteral("Run a scan first."));
    return {};
  }
  ExportOptions opts;
  opts.functionName = symbolBox_->currentText().trimmed().toStdString();
  opts.maxCases = static_cast<uint64_t>(exportLimitBox_->value());
  if (csv)
    return QString::fromStdString(exportCsv(*r, opts.maxCases));
  return QString::fromStdString(litStyle ? exportLit(*r, opts)
                                         : exportTable(*r, opts));
}

void MainWindow::setBusy(bool busy) {
  scanButton_->setEnabled(!busy);
  sweepButton_->setEnabled(!busy);
  cancelButton_->setEnabled(busy);
  openButton_->setEnabled(!busy);
  symbolBox_->setEnabled(!busy);
  convBox_->setEnabled(!busy);
}