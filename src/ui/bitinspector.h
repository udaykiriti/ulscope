// Type a value or click bits; see what the format says about it.
#pragma once

#include <cstdint>
#include <vector>

#include <QWidget>

#include "core/format.h"

using ulpscope::FloatFormat;

class QGroupBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;

class BitInspector : public QWidget {
  Q_OBJECT

public:
  explicit BitInspector(QWidget *parent = nullptr);

  void setFormat(const FloatFormat &fmt);

  // Loads a bit pattern without emitting bitsChanged, for when the plot or the
  // table drives the inspector.
  void setBits(uint64_t bits);
  uint64_t bits() const { return bits_; }

  // Shows the result of probing the loaded function at the current input.
  void setProbe(bool valid, double actual, double expected, uint64_t ulpError,
                bool nanDisagreement);

signals:
  void bitsChanged(uint64_t bits);

private:
  struct BitButton {
    int index;
    QPushButton *button;
  };

  void rebuildBitGrid();
  void refresh();
  void onTextEdited(const QString &text);
  void onBitToggled();
  void onHexEdited(const QString &text);

  FloatFormat fmt_ = FloatFormat::bf16();

  QLineEdit *valueEdit_ = nullptr;
  QLineEdit *hexEdit_ = nullptr;
  QLabel *classLabel_ = nullptr;
  QLabel *fieldsLabel_ = nullptr;
  QLabel *detailsLabel_ = nullptr;
  QLabel *neighboursLabel_ = nullptr;
  QLabel *probeLabel_ = nullptr;
  QGroupBox *bitBox_ = nullptr;
  QGridLayout *bitGrid_ = nullptr;

  std::vector<BitButton> buttons_;
  uint64_t bits_ = 0x3F80;
  bool updating_ = false;
};