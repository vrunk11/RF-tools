/******************************************************************************
 * device.h
 * rfx library - where the predictor runs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

namespace rfx {

// Where the predictor runs: the processor, or an OpenCL GPU (the same bits).
enum class Device { Cpu, Gpu };

} // namespace rfx
