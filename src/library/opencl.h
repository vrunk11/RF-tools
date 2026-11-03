/******************************************************************************
 * opencl.h
 * rfx library - a thin OpenCL layer, loaded at run time
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rfx {
namespace cl {

class Device;

// A device buffer, grown on demand (its contents are lost when it grows).
class Buffer
{
public:
    explicit Buffer(std::shared_ptr<Device> device) : m_device(std::move(device)) {}
    ~Buffer();
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    void ensure(size_t bytes);
    void *handle() const { return m_mem; }

private:
    std::shared_ptr<Device> m_device;
    void *m_mem = nullptr;
    size_t m_capacity = 0;
};

class Kernel
{
public:
    Kernel(std::shared_ptr<Device> device, void *handle) : m_device(std::move(device)), m_handle(handle) {}
    ~Kernel();
    Kernel(const Kernel &) = delete;
    Kernel &operator=(const Kernel &) = delete;

    void setBuffer(unsigned index, const Buffer &buffer);
    void setInt(unsigned index, int32_t value);
    void setFloat(unsigned index, float value);
    void *handle() const { return m_handle; }

private:
    std::shared_ptr<Device> m_device;
    void *m_handle;
};

// A command queue: each user of the device owns one, so several can submit at once.
class Queue
{
public:
    explicit Queue(std::shared_ptr<Device> device);
    ~Queue();
    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;

    // Blocking transfers.
    void write(const Buffer &buffer, const void *data, size_t bytes);
    void read(const Buffer &buffer, void *data, size_t bytes);
    // `global` work items, in groups of `local` (the global size rounded up).
    void launch(const Kernel &kernel, size_t global, size_t local);
    void finish();

private:
    std::shared_ptr<Device> m_device;
    void *m_handle = nullptr;
};

// The process's GPU: the first OpenCL GPU device (or the one LD_OPENCL_DEVICE
// numbers) and its context. Throws rfx::Error when there is none.
class Device : public std::enable_shared_from_this<Device>
{
public:
    static std::shared_ptr<Device> get();
    ~Device();

    const std::string &name() const { return m_name; }

    // Build `source` with `options` and create its kernel `name`.
    std::unique_ptr<Kernel> kernel(const std::string &source, const std::string &options, const char *name);

    struct Api;
    const Api &api() const { return *m_api; }
    void *context() const { return m_context; }
    void *deviceId() const { return m_device; }

private:
    Device() = default;
    void open();

    std::unique_ptr<Api> m_api;
    void *m_device = nullptr;
    void *m_context = nullptr;
    std::string m_name;
};

} // namespace cl
} // namespace rfx
