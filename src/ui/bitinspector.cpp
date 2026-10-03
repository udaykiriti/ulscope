#include "ui/bitinspector.h"

#include <cmath>
#include <cstdlib>

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "core/mpfr_util.h"
#include "ui/formatting.h"

using namespace ulpscope;

BitInspector::BitInspector(QWidget *parent) : QWidget(parent) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);

  auto *valueRow = new QHBoxLayout;
  valueRow->addWidget(new QLabel(QStringLiteral("Value:"), this));
  valueEdit_ = new QLineEdit(this);
  valueEdit_->setPlaceholderText(QStringLiteral("decimal, 0x… hex or 1.5p3"));
  valueEdit_->setToolTip(QStringLiteral(
      "Accepts decimal (1.5), C99 hex float (0x1.8p3), a plain hex bit pattern "
      "(0x3F80) or scientific notation."));
  valueRow->addWidget(valueEdit_, 1);
  root->addLayout(valueRow);

  auto *hexRow = new QHBoxLayout;
  hexRow->addWidget(new QLabel(QStringLiteral("Bits:"), this));
  hexEdit_ = new QLineEdit(this);
  hexEdit_->setToolTip(QStringLiteral("Raw bit pattern."));
  hexRow->addWidget(hexEdit_, 1);
  root->addLayout(hexRow);

  // editingFinished carries no argument, so bridge through the line edit's text.
  connect(valueEdit_, &QLineEdit::editingFinished, this,
          [this] { onTextEdited(valueEdit_->text()); });
  connect(hexEdit_, &QLineEdit::editingFinished, this,
          [this] { onHexEdited(hexEdit_->text()); });

  // --- clickable bit grid --------------------------------------------------
  bitBox_ = new QGroupBox(QStringLiteral("Toggle bits"), this);
  bitGrid_ = new QGridLayout(bitBox_);
  bitGrid_->setSpacing(2);
  // Keep the toggle rows packed at the top instead of stretched to fill.
  bitGrid_->setAlignment(Qt::AlignTop);
  root->addWidget(bitBox_);

  // --- readouts ------------------------------------------------------------
  auto addField = [&](QGridLayout *g, int row, const QString &title) {
    auto *label = new QLabel(this);
    label->setTextFormat(Qt::RichText);
    auto *titleLabel = new QLabel(QStringLiteral("<b>%1</b>").arg(title), this);
    g->addWidget(titleLabel, row, 0);
    g->addWidget(label, row, 1);
    return label;
  };

  auto *info = new QGridLayout;
  info->setColumnStretch(1, 1);
  classLabel_ = addField(info, 0, QStringLiteral("Class"));
  fieldsLabel_ = addField(info, 1, QStringLiteral("Fields"));
  detailsLabel_ = addField(info, 2, QStringLiteral("Values"));
  neighboursLabel_ = addField(info, 3, QStringLiteral("Neighbours"));
  probeLabel_ = addField(info, 4, QStringLiteral("Probe"));
  root->addLayout(info);

  rebuildBitGrid();
  refresh();
}

void BitInspector::setFormat(const FloatFormat &fmt) {
  fmt_ = fmt;
  bits_ &= fmt.mask(fmt.totalBits);
  rebuildBitGrid();
  refresh();
}

void BitInspector::rebuildBitGrid() {
  for (auto &b : buttons_)
    delete b.button;
  buttons_.clear();

  while (QLayoutItem *item = bitGrid_->takeAt(0))
    delete item;

  const int n = fmt_.totalBits;
  bitBox_->setTitle(QStringLiteral("Toggle bits (%1-bit %2)")
                        .arg(n)
                        .arg(QString::fromLatin1(fmt_.name)));

  for (int i = 0; i < n; ++i) {
    const int bit = n - 1 - i; // bit 0 on the right, like a binary number
    auto *button = new QPushButton(QString(), bitBox_);
    button->setCheckable(true);
    button->setFixedSize(22, 24);
    button->setToolTip(QStringLiteral("bit %1").arg(bit));
    button->setProperty("bitIndex", bit);
    connect(button, &QPushButton::clicked, this, &BitInspector::onBitToggled);
    bitGrid_->addWidget(button, i / 8, i % 8);
    buttons_.push_back({bit, button});
  }
}

void BitInspector::onBitToggled() {
  if (updating_)
    return;
  auto *button = qobject_cast<QPushButton *>(sender());
  if (!button)
    return;
  const int bit = button->property("bitIndex").toInt();
  const uint64_t mask = 1ULL << bit;
  bits_ = (bits_ & ~mask) | (button->isChecked() ? mask : 0ULL);
  refresh();
  emit bitsChanged(bits_);
}

void BitInspector::setBits(uint64_t bits) {
  bits_ = bits & fmt_.mask(fmt_.totalBits);
  refresh();
}

void BitInspector::onHexEdited(const QString &text) {
  QString t = text.trimmed();
  if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
    t = t.mid(2);
  bool ok = false;
  const uint64_t v = t.toULongLong(&ok, 16);
  if (ok)
    setBits(v);
  else
    refresh();
  emit bitsChanged(bits_);
}

void BitInspector::onTextEdited(const QString &text) {
  const QString t = text.trimmed();
  if (t.isEmpty())
    return;

  // 1) plain hex bit pattern, e.g. 0x3F80
  if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
    const QString body = t.mid(2);
    bool ok = false;
    const uint64_t v = body.toULongLong(&ok, 16);
    // A bare 0x... that does not parse as an integer may still be a hex float
    // such as 0x1.8p3.
    if (ok && !body.contains(QLatin1Char('.')) &&
        !body.contains(QLatin1Char('p'))) {
      setBits(v);
      emit bitsChanged(bits_);
      return;
    }
  }

  // 2) C99 hex float, e.g. 0x1.8p+3
  // 3) plain decimal or scientific
  bool ok = false;
  const double value = t.toDouble(&ok);
  if (ok) {
    setBits(encodeRounded(value, fmt_));
    emit bitsChanged(bits_);
    return;
  }

  // Nothing parsed: leave the existing value alone and put the text back.
  refresh();
}

void BitInspector::setProbe(bool valid, double actual, double expected,
                            uint64_t ulpError, bool nanDisagreement) {
  if (!valid) {
    probeLabel_->setText(QStringLiteral("<i>not available</i>"));
    return;
  }
  QString verdict;
  if (nanDisagreement) {
    verdict = QStringLiteral("<span style='color:#7b3fbf'>NaN disagreement</span>");
  } else if (ulpError == 0) {
    verdict = QStringLiteral("<span style='color:#2e8b57'>correct</span>");
  } else {
    verdict = QStringLiteral("<span style='color:#d62728'>%1 UPL off</span>")
                  .arg(formatUlp(ulpError));
  }
  probeLabel_->setText(QStringLiteral("got %1<br>want %2<br>%3")
                           .arg(formatExact(actual), formatExact(expected),
                                verdict));
}

void BitInspector::refresh() {
  updating_ = true;

  const uint64_t mask = fmt_.mask(fmt_.totalBits);
  bits_ &= mask;

  // Reflect the current bits_ in the bit buttons.
  for (auto &b : buttons_) {
    const bool on = ((bits_ >> b.index) & 1ULL) != 0;
    QSignalBlocker block(b.button);
    b.button->setChecked(on);
    b.button->setText(on ? QStringLiteral("1") : QStringLiteral("0"));
    b.button->setStyleSheet(on ? QStringLiteral("background:#cfe8ff;")
                               : QStringLiteral("background:#f0f0f0;"));
  }

  const double value = fmt_.decode(bits_);
  const Class cls = fmt_.classify(bits_);

  const int digits = (fmt_.totalBits + 3) / 4;
  QSignalBlocker vb(valueEdit_);
  QSignalBlocker hb(hexEdit_);
  valueEdit_->setText(formatExact(value));
  hexEdit_->setText(
      QStringLiteral("0x%1").arg(QString::number(bits_, 16)
                                     .rightJustified(digits, QChar('0'))
                                     .toUpper(),
                                 digits, QChar('0')));

  // --- fields --------------------------------------------------------------
  const uint64_t sign = bits_ & fmt_.signMask();
  const uint64_t expField = (bits_ >> fmt_.mantBits) & fmt_.expMask();
  const uint64_t frac = bits_ & fmt_.fracMask();

  // One place decides how a sign reads, so the Class and Fields rows cannot
  // drift apart.
  const QString signText =
      sign ? QStringLiteral("1 (negative)") : QStringLiteral("0 (positive)");

  QString expText;
  if (fmt_.hasInfNan && expField == fmt_.expMask())
    expText = QStringLiteral("%1 (%2)").arg(expField).arg(frac ? "NaN" : "infinity");
  else if (expField == 0)
    expText = QStringLiteral("0 (zero/denormal)");
  else
    expText = QStringLiteral("%1 (unbiased %2)").arg(expField).arg(
        static_cast<qlonglong>(expField) - fmt_.bias);

  // The Fields row below already shows the sign, so repeating it here just made
  // the panel look like it had lost track of its own output. The spacing is
  // more use next to the class than a duplicate of what is below.
  classLabel_->setText(
      QStringLiteral("%1<br><span style='color:#666'>spacing %2</span>")
          .arg(QString::fromLatin1(className(cls)))
          .arg(formatExact(fmt_.ulpOf(bits_))));

  fieldsLabel_->setText(
      QStringLiteral("sign: %1<br>exponent: %2<br>mantissa: %3 (0x%4)")
          .arg(signText, expText)
          .arg(frac)
          .arg(QString::number(frac, 16)));

  // --- values --------------------------------------------------------------
  QStringList details;
  details << QStringLiteral("decimal: %1").arg(formatExact(value));
  details << QStringLiteral("hex float: %1")
                 .arg(QString::fromStdString(formatHexFloat(value)));
  details << QStringLiteral("spacing: %1 ULP")
                 .arg(formatExact(fmt_.ulpOf(bits_)));
  if (cls == Class::Subnormal)
    details << QStringLiteral("denormal: %1 of the way from zero")
                   .arg(frac);
  if (cls == Class::Normal)
    details << QStringLiteral("2^%1")
                   .arg(static_cast<qlonglong>(expField) - fmt_.bias);

  detailsLabel_->setText(details.join(QStringLiteral("<br>")));

  // --- neighbours ----------------------------------------------------------
  const bool finite = cls != Class::NaN && cls != Class::Infinity;
  QStringList nb;
  if (finite && bits_ > 0) {
    nb << QStringLiteral("prev: %1").arg(
        formatExact(fmt_.decode(bits_ - 1)));
  }
  if (finite && bits_ + 1 <= mask) {
    nb << QStringLiteral("next: %1").arg(
        formatExact(fmt_.decode(bits_ + 1)));
  }
  nb << QStringLiteral("min normal: %1")
            .arg(formatExact(fmt_.minNormal()));
  nb << QStringLiteral("max finite: %1")
            .arg(formatExact(fmt_.maxFinite()));
  nb << QStringLiteral("inputs: %1").arg(fmt_.inputCount() > (1ull << 24)
                                              ? QStringLiteral("(too many)")
                                              : QString::number(fmt_.inputCount()));
  neighboursLabel_->setText(nb.join(QStringLiteral("<br>")));

  updating_ = false;
}