#include "ui/worsttable.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QColor>
#include <QFont>

#include "ui/formatting.h"

using namespace ulpscope;

WorstTableModel::WorstTableModel(QObject *parent) : QAbstractTableModel(parent) {}

void WorstTableModel::setResult(const ScanResult *result) {
  beginResetModel();
  result_ = result;
  rebuild();
  endResetModel();
}

void WorstTableModel::setMismatchesOnly(bool only) {
  if (mismatchesOnly_ == only)
    return;
  beginResetModel();
  mismatchesOnly_ = only;
  rebuild();
  endResetModel();
}

void WorstTableModel::rebuild() {
  rows_.clear();
  if (!result_)
    return;
  rows_.reserve(result_->samples.size());
  for (size_t i = 0; i < result_->samples.size(); ++i) {
    if (mismatchesOnly_ && !result_->samples[i].mismatch)
      continue;
    rows_.push_back(static_cast<int>(i));
  }
  resort(sortColumn_, sortOrder_);
}

int WorstTableModel::sampleIndexForRow(int row) const {
  if (row < 0 || row >= static_cast<int>(rows_.size()))
    return -1;
  return rows_[row];
}

int WorstTableModel::rowForSampleIndex(int index) const {
  for (size_t r = 0; r < rows_.size(); ++r)
    if (rows_[r] == index)
      return static_cast<int>(r);
  return -1;
}

void WorstTableModel::resort(int column, Qt::SortOrder order) {
  if (!result_)
    return;
  const ScanResult *r = result_;
  const auto &samples = r->samples;
  const bool ascending = order == Qt::AscendingOrder;

  // A NaN disagreement has no ordering, so it always sorts to the top. That
  // matches how ScanResult::worst is ordered.
  auto rank = [&](int idx) {
    return samples[idx].nanDisagreement ? 0 : 1;
  };

  auto key = [&](int idx) -> long double {
    const Sample &s = samples[idx];
    switch (column) {
    case ColInputBits:
    case ColInputHex:
      return static_cast<long double>(s.inputBits);
    case ColInput:
      return std::isnan(s.input) ? 0.0L : static_cast<long double>(s.input);
    case ColExpectedBits:
    case ColActualBits:
      return static_cast<long double>(column == ColExpectedBits ? s.expectedBits
                                                                 : s.actualBits);
    case ColExpected:
      return std::isnan(s.expected) ? 0.0L : static_cast<long double>(s.expected);
    case ColActual:
      return std::isnan(s.actual) ? 0.0L : static_cast<long double>(s.actual);
    case ColUlpError:
      return static_cast<long double>(s.ulpError);
    case ColResult:
      return s.mismatch ? 1 : 0;
    case ColClass:
    default:
      return static_cast<long double>(s.inputBits);
    }
  };

  std::stable_sort(rows_.begin(), rows_.end(), [&](int a, int b) {
    // NaN disagreements first regardless of direction.
    const int ra = rank(a), rb = rank(b);
    if (ra != rb)
      return ra < rb;
    const long double ka = key(a), kb = key(b);
    if (ka != kb)
      return ascending ? ka < kb : ka > kb;
    return a < b; // stable, deterministic
  });
}

int WorstTableModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int WorstTableModel::columnCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : ColumnCount;
}

QVariant WorstTableModel::data(const QModelIndex &index, int role) const {
  if (!result_ || !index.isValid())
    return {};
  const int idx = sampleIndexForRow(index.row());
  if (idx < 0)
    return {};
  const Sample &s = result_->samples[idx];
  const FloatFormat &fmt = result_->config.fmt;

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
    case ColInputBits:
      return QVariant::fromValue(static_cast<qulonglong>(s.inputBits));
    case ColInputHex:
      return formatBitPattern(s.inputBits, fmt);
    case ColInput:
      return formatExact(s.input);
    case ColClass:
      return QString::fromLatin1(className(fmt.classify(s.inputBits)));
    case ColExpectedBits:
      return formatBitPattern(s.expectedBits, fmt);
    case ColExpected:
      return formatExact(s.expected);
    case ColActualBits:
      return formatBitPattern(s.actualBits, fmt);
    case ColActual:
      return formatExact(s.actual);
    case ColUlpError:
      return s.nanDisagreement ? QStringLiteral("NaN") : formatUlp(s.ulpError);
    case ColResult:
      return s.mismatch ? QStringLiteral("MISMATCH")
                        : QStringLiteral("ok");
    default:
      break;
    }
  }

  if (role == Qt::TextAlignmentRole) {
    if (index.column() == ColInput || index.column() == ColExpected ||
        index.column() == ColActual)
      return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    return static_cast<int>(Qt::AlignCenter);
  }

  if (role == Qt::ForegroundRole && s.mismatch) {
    const QColor colour = s.nanDisagreement ? QColor(0x7b, 0x3f, 0xbf)
                                            : QColor(0xc0, 0x20, 0x20);
    return QBrush(colour);
  }

  if (role == Qt::FontRole && s.mismatch) {
    QFont f;
    f.setBold(true);
    return f;
  }

  if (role == Qt::ToolTipRole) {
    return QStringLiteral(
               "input   %1 = %2 (%3)\n"
               "expected %4\n"
               "actual   %5\n"
               "%6 UPL apart")
        .arg(formatBitPattern(s.inputBits, fmt), formatExact(s.input),
             QString::fromLatin1(className(fmt.classify(s.inputBits))),
             formatExact(s.expected), formatExact(s.actual),
             s.nanDisagreement ? QStringLiteral("NaN") : formatUlp(s.ulpError));
  }

  return {};
}

void WorstTableModel::sort(int column, Qt::SortOrder order) {
  sortColumn_ = column;
  sortOrder_ = order;
  beginResetModel();
  resort(column, order);
  endResetModel();
}

QVariant WorstTableModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return QAbstractTableModel::headerData(section, orientation, role);
  switch (section) {
  case ColInputBits: return QStringLiteral("input bits");
  case ColInputHex: return QStringLiteral("input hex");
  case ColInput: return QStringLiteral("input");
  case ColClass: return QStringLiteral("class");
  case ColExpectedBits: return QStringLiteral("expected bits");
  case ColExpected: return QStringLiteral("expected");
  case ColActualBits: return QStringLiteral("actual bits");
  case ColActual: return QStringLiteral("actual");
  case ColUlpError: return QStringLiteral("ULP error");
  case ColResult: return QStringLiteral("result");
  default: return {};
  }
}