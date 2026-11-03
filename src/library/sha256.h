/******************************************************************************
 * sha256.h
 * rfx library - SHA-256, with the processor's SHA instructions when it has them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rfx {

// An archive's captures are hashed whole, twice (their bytes, and their samples),
// which is gigabytes: at the speed of a plain implementation that is most of the
// time pack takes on a GPU. The block function is chosen when the object is made:
// the SHA extensions of x86 processors if there are any, else portable code.
class Sha256
{
public:
    // `allowHardware` false forces the portable code (for the tests).
    explicit Sha256(bool allowHardware = true);

    void update(const void *data, size_t length);
    // The digest; the object is not usable after it.
    std::array<uint8_t, 32> finish();
    // The digest as 64 lower-case hex digits.
    std::string finishHex();

    // Whether this processor has the instructions.
    static bool hardwareAvailable();

private:
    using BlockFunction = void (*)(uint32_t *state, const uint8_t *blocks, size_t count);

    BlockFunction m_blocks;
    uint32_t m_state[8];
    uint8_t m_buffer[64];
    size_t m_buffered = 0;
    uint64_t m_length = 0;
};

} // namespace rfx
