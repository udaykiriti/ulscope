#include <QApplication>
#include <QFile>
#include <QPixmap>
#include <QTextStream>
#include <QDockWidget>
#include <QTimer>

#include <cstdio>

#include "ui/formatting.h"
#include "ui/mainwindow.h"

namespace {

// Drives a scan without a mouse and saves a screenshot, so the UI can be
// checked on a headless machine.
int selfTest(const QString &outPath, QApplication &app) {
  QTextStream out(stdout);
  QTextStream err(stderr);

  MainWindow w;
  w.resize(1400, 900);
  w.show();

#ifndef ULPSCOPE_DEMO_SO
  const QString demo = QStringLiteral("ulpscope_demo");
#else
  const QString demo = QStringLiteral(ULPSCOPE_DEMO_SO);
#endif

  QString error;
  if (!w.openLibrary(demo, &error)) {
    err << "cannot load " << demo << ": " << error << "\n";
    return 2;
  }

  auto report = [](QTextStream &o, const char *what,
                   const ulpscope::ScanResult *r) {
    if (!r || !r->ok)
      return;
    o << what << ": " << r->total << " inputs, " << r->mismatchCount
      << " mismatches, max " << r->maxUlp << " ULP (fmt="
      << ulpscope::formatName(r->config.fmt) << ")\n";
  };

  // Both of these should land on bf16 and the 16-bit ABI automatically, from
  // the names alone. No explicit format or ABI is set here on purpose: this
  // exercises the auto-detection rather than bypassing it.
  w.selectFunction(QStringLiteral("sqrtbf16"));
  if (w.runScanBlocking(&error))
    report(out, "sqrtbf16", w.lastResult());
  else
    err << "sqrtbf16 scan failed: " << error << "\n";

  // A function with a 1 ULP defect, so the mismatch colouring is visible.
  w.selectFunction(QStringLiteral("lgammabf16"));
  if (w.runScanBlocking(&error))
    report(out, "lgammabf16", w.lastResult());
  else
    err << "lgammabf16 scan failed: " << error << "\n";

  // An fp8 function, which implies both the 8-bit format and the integer ABI.
  w.selectFunction(QStringLiteral("sqrte4m3"));
  if (w.runScanBlocking(&error))
    report(out, "sqrte4m3", w.lastResult());
  else
    err << "sqrte4m3 scan failed: " << error << "\n";

  // Exercise the whole-library sweep, which is the feature that answers "which
  // of these functions are wrong" rather than "is this one function wrong".
  w.selectFunction(QStringLiteral("lgammabf16"));
  w.runSweepBlocking();
  const auto &rows = w.sweepResults();
  out << "sweep: " << rows.size() << " functions\n";
  int broken = 0;
  for (const auto &r : rows)
    if (r.ok && r.mismatchCount)
      ++broken;
  out << "  " << broken << " of them are not correctly rounded\n";

  // Leave the window showing the detailed scan of one function, which is the
  // view the screenshot is most useful for.
  w.showSweepTab();
  w.selectFunction(QStringLiteral("lgammabf16"));
  w.showScanTab();
  if (!w.runScanBlocking(&error))
    err << "final scan failed: " << error << "\n";

  // Which dock area each panel ended up in, and the geometry it was given.
  for (QDockWidget *d : w.findChildren<QDockWidget *>())
    out << "  panel " << d->objectName() << " area="
        << int(w.dockWidgetArea(d)) << " at " << d->geometry().x() << ","
        << d->geometry().y() << " " << d->geometry().width() << "x"
        << d->geometry().height() << " floating=" << d->isFloating() << "\n";

  // Grab both views: the sweep is the one that answers "what is broken", the
  // detail scan answers "how and where".
  QTimer::singleShot(400, [&w, outPath, &app] {
    if (outPath.isEmpty()) {
      app.quit();
      return;
    }
    w.showSweepTab();
    app.processEvents();
    const QString base = outPath.contains(QLatin1Char('.'))
                             ? outPath.left(outPath.lastIndexOf(QLatin1Char('.')))
                             : outPath;
    w.grab().save(base + QStringLiteral("-sweep.png"));
    w.showScanTab();
    app.processEvents();
    w.grab().save(base + QStringLiteral("-scan.png"));
    w.grab().save(outPath);
    app.quit();
  });
  return app.exec();
}

} // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("ulpscope"));
  app.setApplicationVersion(QStringLiteral("0.2.0"));

  const QStringList args = app.arguments();
  if (args.size() >= 2 && args[1] == QLatin1String("--selftest"))
    return selfTest(args.size() >= 3 ? args[2] : QString(), app);

  MainWindow w;
  w.show();
  return app.exec();
}