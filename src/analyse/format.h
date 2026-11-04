/******************************************************************************
 * format.h
 * rf-analyse - how the numbers are written
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QLocale>
#include <QString>

#include <cmath>
#include <cstdint>

namespace fmt {

// A time in milliseconds to a tenth of a microsecond, as the bar says it.
inline QString milliseconds(double seconds)
{
    return QString::number(seconds * 1000.0, 'f', 4) + QStringLiteral(" ms");
}

// A duration: seconds from one second up, milliseconds below.
inline QString duration(double seconds)
{
    return seconds >= 1.0 ? QString::number(seconds, 'f', 3) + QStringLiteral(" s") : QString::number(seconds * 1000.0, 'f', 3) + QStringLiteral(" ms");
}

inline QString bytes(uint64_t n)
{
    if (n >= 1000000000ull) {
        return QString::number(static_cast<double>(n) / 1e9, 'f', 2) + QStringLiteral(" GB");
    }
    if (n >= 1000000ull) {
        return QString::number(static_cast<double>(n) / 1e6, 'f', 2) + QStringLiteral(" MB");
    }
    if (n >= 1000ull) {
        return QString::number(static_cast<double>(n) / 1e3, 'f', 1) + QStringLiteral(" kB");
    }
    return QString::number(n) + QStringLiteral(" B");
}

inline QString percent(uint64_t part, uint64_t whole)
{
    return whole == 0 ? QStringLiteral("-") : QString::number(100.0 * static_cast<double>(part) / static_cast<double>(whole), 'f', 1) + QLatin1Char('%');
}

// A whole number with its thousands apart.
inline QString count(qint64 n)
{
    return QLocale(QLocale::English).toString(n);
}

// A signed whole number with a real minus.
inline QString signedNumber(qint64 v)
{
    return v > 0 ? QStringLiteral("+") + QString::number(v) : (v < 0 ? QString(QChar(0x2212)) + QString::number(-v) : QStringLiteral("0"));
}

inline QString megahertz(double hz)
{
    return QString::number(hz / 1e6, 'f', 3) + QStringLiteral(" MHz");
}

} // namespace fmt
