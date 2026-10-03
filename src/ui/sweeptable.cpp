#include "ui/sweeptable.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QColor>
#include <QFont>

using namespace ulpscope;

SweepModel::SweepModel(QObject *parent) : QAbstractTableModel(parent) {}

void SweepModel::setResults(std::vector<FunctionSummary> results) {
  beginResetModel();
  results_ = std::move(results);
  sort(sortColumn_, sortOrder_);
  endResetModel();
}

const FunctionSummary *SweepModel::at(int row) const {
  if (row < 0 || row >= static_cast<int>(results_.size()))
    return nullptr;
  return &results_[row];
}

int SweepModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : static_cast<int>(results_.size());
}

int SweepModel::columnCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : ColumnCount;
}

QVariant SweepModel::data(const QModelIndex &index, int role) const {
  const FunctionSummary *s = at(index.row());
  if (!s || !index.isValid())
    return {};

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
    case ColFunction: return QString::fromStdString(s->symbol);
    case ColBase: return QString::fromStdString(s->baseName);
    case ColFormat:
      return s->sampled ? QStringLiteral("%1 (sampled)")
                             .arg(QString::fromStdString(s->format))
                        : QString::fromStdString(s->format);
    case ColAbi: return QString::fromStdString(s->callConv);
    case ColResult:
      if (s->cancelled)
        return QStringLiteral("cancelled");
      if (!s->ok)
        return QString::fromStdString(s->error);
      return s->mismatchCount ? QStringLiteral("%1 mismatching")
                              : QStringLiteral("correct");
    case ColInputs: return QVariant::fromValue(static_cast<qulonglong>(s->total));
    case ColMismatches:
      return QVariant::fromValue(static_cast<qulonglong>(s->mismatchCount));
    case ColMismatchPct:
      if (!s->ok || s->total == 0)
        return QStringLiteral("-");
      return QString::number(100.0 * static_cast<double>(s->mismatchCount) /
                                 static_cast<double>(s->total),
                             'f', 3);
    case ColMaxUlp: return QVariant::fromValue(static_cast<qulonglong>(s->maxUlp));
    case ColRmsUlp:
      return QString::number(s->rmsUlp, 'f', 4);
    default: break;
    }
  }

  if (role == Qt::TextAlignmentRole) {
    switch (index.column()) {
    case ColInputs:
    case ColMismatches:
    case ColMismatchPct:
    case ColMaxUlp:
    case ColRmsUlp:
      return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    default:
      return static_cast<int>(Qt::AlignCenter);
    }
  }

  // A clean function should read as good news, and a broken one as bad news,
  // without needing the numbers.
  if (role == Qt::ForegroundRole) {
    if (!s->ok)
      return QBrush(QColor(0x88, 0x88, 0x88));
    if (s->mismatchCount == 0)
      return QBrush(QColor(0x2e, 0x8b, 0x57));
    if (s->mismatchCount == s->total && s->total > 0)
      return QBrush(QColor(0x7b, 0x3f, 0xbf)); // wrong on everything: suspect the ABI
    if (s->maxUlp <= 1)
      return QBrush(QColor(0xd6, 0x8a, 0x27)); // nearly right
    return QBrush(QColor(0xc0, 0x20, 0x20));
  }

  if (role == Qt::ToolTipRole && s->ok) {
    return QStringLiteral(
               "%1\nresolved as: %2\ncalled as: %3\n%4 mismatches over %5 inputs\n"
               "worst %6 ULP, rms %7\n"
               "mismatches by class: zero %7, denormal %8, normal %9, inf %10, nan %11")
        .arg(QString::fromStdString(s->symbol),
             QString::fromStdString(s->baseName))
        .arg(s->mismatchCount)
        .arg(s->total)
        .arg(s->maxUlp)
        .arg(s->rmsUlp, 0, 'f', 4)
        .arg(s->mismatchesPerClass[0])
        .arg(s->mismatchesPerClass[1])
        .arg(s->mismatchesPerClass[2])
        .arg(s->mismatchesPerClass[3])
        .arg(s->mismatchesPerClass[4]);
  }

  return {};
}

QVariant SweepModel::headerData(int section, Qt::Orientation orientation,
                                int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return QAbstractTableModel::headerData(section, orientation, role);
  switch (section) {
  case ColFunction: return QStringLiteral("function");
  case ColBase: return QStringLiteral("reference");
  case ColFormat: return QStringLiteral("format");
  case ColAbi: return QStringLiteral("ABI");
  case ColResult: return QStringLiteral("result");
  case ColInputs: return QStringLiteral("inputs");
  case ColMismatches: return QStringLiteral("mismatches");
  case ColMismatchPct: return QStringLiteral("mismatch %");
  case ColMaxUlp: return QStringLiteral("worst ULP");
  case ColRmsUlp: return QStringLiteral("rms ULP");
  default: return {};
  }
}

void SweepModel::sort(int column, Qt::SortOrder order) {
  sortColumn_ = column;
  sortOrder_ = order;
  beginResetModel();

  const bool ascending = order == Qt::AscendingOrder;
  std::stable_sort(results_.begin(), results_.end(),
                   [&](const FunctionSummary &a, const FunctionSummary &b) {
                     // Anything that failed to run sorts away from the data.
                     if (a.ok != b.ok)
                       return a.ok; // successful runs first
                     auto key = [&](const FunctionSummary &s) -> double {
                       switch (column) {
                       case ColMismatches:
                         return static_cast<double>(s.mismatchCount);
                       case ColMismatchPct:
                         return s.total ? 100.0 * static_cast<double>(s.mismatchCount) /
                                              static_cast<double>(s.total)
                                        : 0.0;
                       case ColMaxUlp:
                         return static_cast<double>(s.maxUlp);
                       case ColRmsUlp:
                         return s.rmsUlp;
                       case ColInputs:
                         return static_cast<double>(s.total);
                       case ColFunction:
                       case ColBase:
                       case ColFormat:
                       case ColAbi:
                         return 0.0; // handled by the stable tiebreak below
                       case ColResult:
                       default:
                         return s.mismatchCount ? 1.0 : 0.0;
                       }
                     };
                     if (column == ColFunction || column == ColBase ||
                         column == ColFormat || column == ColAbi)
                       return ascending ? a.symbol < b.symbol : a.symbol > b.symbol;
                     const double ka = key(a), kb = key(b);
                     if (ka != kb)
                       return ascending ? ka < kb : ka > kb;
                     return a.symbol < b.symbol;
                   });

  endResetModel();
}