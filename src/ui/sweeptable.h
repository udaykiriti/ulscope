// Ranked table of a whole-library sweep.
#pragma once

#include <QAbstractTableModel>
#include <vector>

#include "core/sweep.h"

using ulpscope::FunctionSummary;

class SweepModel : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column {
    ColFunction = 0,
    ColBase,
    ColFormat,
    ColAbi,
    ColResult,
    ColInputs,
    ColMismatches,
    ColMismatchPct,
    ColMaxUlp,
    ColRmsUlp,
    ColumnCount
  };

  explicit SweepModel(QObject *parent = nullptr);

  void setResults(std::vector<FunctionSummary> results);

  const FunctionSummary *at(int row) const;

  int rowCount(const QModelIndex &parent = {}) const override;
  int columnCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;
  void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
  std::vector<FunctionSummary> results_;
  int sortColumn_ = ColMaxUlp;
  Qt::SortOrder sortOrder_ = Qt::DescendingOrder;
};