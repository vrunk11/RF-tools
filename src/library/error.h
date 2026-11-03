/******************************************************************************
 * error.h
 * rfx library - the error the library throws
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <stdexcept>
#include <string>

namespace rfx {

// A file that cannot be read or written, or data that cannot be used: the tools
// catch it, print the message and exit with 1.
class Error : public std::runtime_error
{
public:
    explicit Error(const std::string &message) : std::runtime_error(message) {}
};

} // namespace rfx
