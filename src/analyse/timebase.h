/******************************************************************************
 * timebase.h
 * rf-analyse - the time base of a capture corrected from the lines pass 0 located: every line at the place it has at the
 * nominal speed, the same place in every capture that has the same fields (as a stacker lines them up)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "alignment.h"

#include <cstddef>
#include <memory>
#include <vector>

// A track's lines against the corrected time: `tau` is where each line stands on the common time axis (in samples at the nominal speed), `x`
// where it is in the capture. Between two lines the capture is stretched linearly, so a line has the same width everywhere.
struct TimeBase
{
    std::vector<double> tau;
    std::vector<double> x;
    double slopeFirst = 1.0;
    double slopeLast = 1.0;

    // The place in the capture of the corrected time `m`.
    double sourceAt(double m) const;

    // From the fields of a capture (`pal`: the system, `rate`: its sample rate). With `anchor`, the fields of the capture that gives the common
    // axis: a field of `own` takes the place the field with the same frame code has in it, so that the same frame is at the same time in both
    // (a field with no code follows the ones next to it). Without a match, or without an anchor, the axis starts at the capture's own first
    // field. Null when there are too few lines to say.
    static std::shared_ptr<const TimeBase> make(const std::vector<rfx::Field> &own, bool pal, double rate, const std::vector<rfx::Field> *anchor);
};
