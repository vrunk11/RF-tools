/******************************************************************************
 * parallel.h
 * rfx library - run the iterations of a loop on several threads
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace rfx {

// One thread per processor.
inline unsigned defaultThreads()
{
    const unsigned n = std::thread::hardware_concurrency();
    return n != 0 ? n : 1;
}

// Call `body(i)` for every i in [0, count) on `threads` threads, in no
// particular order. The first exception stops the loop and is rethrown once
// every thread has finished.
template<typename Body>
void parallelFor(size_t count, unsigned threads, Body body)
{
    std::atomic<size_t> next{0};
    std::exception_ptr failure;
    std::mutex failureMutex;
    auto worker = [&]() {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= count) {
                return;
            }
            try {
                body(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(failureMutex);
                if (!failure) {
                    failure = std::current_exception();
                }
                next.store(count);
                return;
            }
        }
    };
    const size_t used = std::min<size_t>(threads == 0 ? 1 : threads, count);
    std::vector<std::thread> pool;
    for (size_t t = 1; t < used; ++t) {
        pool.emplace_back(worker);
    }
    worker();
    for (std::thread &t : pool) {
        t.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace rfx
