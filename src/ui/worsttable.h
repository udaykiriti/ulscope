// Sortable, filterable view of a scan's results, worst errors first.
#pragma once

#include <QAbstractTableModel>
#include <vector>

#include "core/scan.h"

using ulpscope::Sample;
using ulpscope::ScanResult;

class WorstTableModel : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column {
    ColInputBits = 0,
    ColInputHex,
    ColInput,
    ColClass,
    ColExpectedBits,
    ColExpected,
    ColActualBits,
    ColActual,
    ColUlpError,
    ColResult,
    ColumnCount
  };

  explicit WorstTableModel(QObject *parent = nullptr);

  void setResult(const ScanResult *result);
  void setMismatchesOnly(bool only);

  // Index into ScanResult::samples for a row, or -1.
  int sampleIndexForRow(int row) const;
  int rowForSampleIndex(int index) const;

  const ScanResult *result() const { return result_; }

  int rowCount(const QModelIndex &parent = {}) const override;
  int columnCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;
  void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
  void rebuild();
  void resort(int column, Qt::SortOrder order);

  const ScanResult *result_ = nullptr;
  std::vector<int> rows_; // indices into result_->samples
  bool mismatchesOnly_ = false;
  int sortColumn_ = ColUlpError;
  Qt::SortOrder sortOrder_ = Qt::DescendingOrder;
};