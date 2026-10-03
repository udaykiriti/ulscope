// Shared number formatting for the UI. Header-only so the widget sources stay
// independent of each other.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <QString>

#include "core/format.h"

namespace ulpscope {

// "0x3F80" for 16-bit formats, "0x3F800000" for f32, always wide enough for
// the format so columns line up.
inline QString formatBitPattern(uint64_t bits, const FloatFormat &fmt) {
  const int digits = (fmt.totalBits + 3) / 4;
  return QStringLiteral("0x%1")
      .arg(QString::number(bits, 16).rightJustified(digits, QChar('0'))
               .toUpper(),
           digits, QChar('0'));
}

inline QString formatBitPattern(double bits) {
  if (bits < 0)
    return QStringLiteral("-");
  return QStringLiteral("0x%1")
      .arg(QString::number(static_cast<qulonglong>(bits), 16).toUpper());
}

// Shortest decimal that reads back as the same double, with an explicit
// marker for the non-finite cases. Never prints "-nan", which is a compiler
// detail rather than a property of the value.
inline QString formatExact(double v) {
  if (std::isnan(v))
    return QStringLiteral("NaN");
  if (std::isinf(v))
    return v > 0 ? QStringLiteral("+Inf") : QStringLiteral("-Inf");

  char buf[64];
  for (int prec = 17; prec >= 1; --prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v)
      break;
  }
  QString s = QString::fromLatin1(buf);
  if (std::signbit(v) && v != 0.0 && !s.startsWith(QLatin1Char('-')))
    s.prepend(QLatin1Char('-'));
  return s;
}

// Axis label for an error magnitude: exact for small values, compact above.
inline QString formatUlpCompact(double err) {
  if (err == 0.0)
    return QStringLiteral("0");
  if (err < 1000.0)
    return QString::number(static_cast<int>(err));
  if (err < 1e6) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", err);
    return QString::fromLatin1(buf);
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", err);
  return QString::fromLatin1(buf);
}

inline QString formatUlp(uint64_t err) {
  return QString::number(static_cast<qulonglong>(err));
}

// C99 hex float, e.g. 0x1.8p+3. Much easier to compare against a hex dump than
// a decimal with 17 digits.
inline std::string formatHexFloat(double v) {
  if (std::isnan(v))
    return "nan";
  if (std::isinf(v))
    return v > 0 ? "inf" : "-inf";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%a", v);
  return buf;
}

// Format name as a QString, for the UI layer.
inline QString formatName(const FloatFormat &f) {
  return QString::fromLatin1(f.name);
}

// Hex pattern wide enough for `fmt`, with no leading zeros beyond the format's
// own width, e.g. "0x3F80". Used by the export writers, which must not depend
// on Qt.
inline std::string hexPattern(uint64_t bits, const FloatFormat &fmt) {
  char buf[32];
  const int digits = (fmt.totalBits + 3) / 4;
  std::snprintf(buf, sizeof(buf), "0x%0*llX", digits,
                static_cast<unsigned long long>(bits));
  return buf;
}

// Plain decimal, shortest form that reads back identically. The export writers
// emit this as a C literal so a reader can see the value, with the bit pattern
// alongside for the cases decimal cannot express (NaN, signed zero).
inline std::string decimalLiteral(double v) {
  if (std::isnan(v))
    return "__builtin_nan(\"\")";
  if (std::isinf(v))
    return v > 0 ? "__builtin_inff()" : "__builtin_inf()";
  if (v == 0.0)
    return std::signbit(v) ? "-0.0" : "0.0";

  char buf[64];
  for (int prec = 17; prec >= 1; --prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v)
      break;
  }
  std::string s(buf);
  if (s.find_first_of(".eEnN") == std::string::npos)
    s += ".0"; // keep it a double literal
  return s;
}

} // namespace ulpscope