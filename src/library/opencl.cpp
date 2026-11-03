/******************************************************************************
 * opencl.cpp
 * rfx library - a thin OpenCL layer, loaded at run time
 *
 * The OpenCL library (OpenCL.dll, shipped with the GPU driver on Windows;
 * libOpenCL.so.1 elsewhere) is loaded when the GPU is first used, so that
 * building needs no SDK and a machine without it only loses the GPU paths.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "opencl.h"

#include "error.h"

#include <QLibrary>
#include <QProcessEnvironment>

#include <mutex>

#ifdef _WIN32
#define RFX_CL_CALL __stdcall
#else
#define RFX_CL_CALL
#endif

namespace rfx {
namespace cl {

namespace {

using Int = int32_t;
using Uint = uint32_t;
using Handle = void *;

constexpr uint64_t kDeviceTypeGpu = 1u << 2;
constexpr Uint kDeviceName = 0x102B;
constexpr Uint kDeviceExtensions = 0x1030;
constexpr Uint kProgramBuildLog = 0x1183;
constexpr uint64_t kMemReadWrite = 1u << 0;
constexpr Uint kTrue = 1;

void check(Int code, const char *what)
{
    if (code != 0) {
        throw Error(std::string("OpenCL ") + what + " failed (error " + std::to_string(code) + ")");
    }
}

} // namespace

// The OpenCL 1.2 entry points used here.
struct Device::Api
{
    QLibrary lib;
    Int(RFX_CL_CALL *getPlatformIds)(Uint, Handle *, Uint *) = nullptr;
    Int(RFX_CL_CALL *getDeviceIds)(Handle, uint64_t, Uint, Handle *, Uint *) = nullptr;
    Int(RFX_CL_CALL *getDeviceInfo)(Handle, Uint, size_t, void *, size_t *) = nullptr;
    Handle(RFX_CL_CALL *createContext)(const intptr_t *, Uint, const Handle *, const void *, void *, Int *) = nullptr;
    Handle(RFX_CL_CALL *createCommandQueue)(Handle, Handle, uint64_t, Int *) = nullptr;
    Handle(RFX_CL_CALL *createProgramWithSource)(Handle, Uint, const char **, const size_t *, Int *) = nullptr;
    Int(RFX_CL_CALL *buildProgram)(Handle, Uint, const Handle *, const char *, const void *, void *) = nullptr;
    Int(RFX_CL_CALL *getProgramBuildInfo)(Handle, Handle, Uint, size_t, void *, size_t *) = nullptr;
    Handle(RFX_CL_CALL *createKernel)(Handle, const char *, Int *) = nullptr;
    Handle(RFX_CL_CALL *createBuffer)(Handle, uint64_t, size_t, void *, Int *) = nullptr;
    Int(RFX_CL_CALL *releaseMemObject)(Handle) = nullptr;
    Int(RFX_CL_CALL *releaseKernel)(Handle) = nullptr;
    Int(RFX_CL_CALL *releaseCommandQueue)(Handle) = nullptr;
    Int(RFX_CL_CALL *setKernelArg)(Handle, Uint, size_t, const void *) = nullptr;
    Int(RFX_CL_CALL *enqueueWriteBuffer)(Handle, Handle, Uint, size_t, size_t, const void *, Uint, const Handle *, Handle *) = nullptr;
    Int(RFX_CL_CALL *enqueueReadBuffer)(Handle, Handle, Uint, size_t, size_t, void *, Uint, const Handle *, Handle *) = nullptr;
    Int(RFX_CL_CALL *enqueueNdRangeKernel)(Handle, Handle, Uint, const size_t *, const size_t *, const size_t *, Uint, const Handle *, Handle *) = nullptr;
    Int(RFX_CL_CALL *finish)(Handle) = nullptr;

    void load()
    {
#if defined(Q_OS_WIN)
        const QStringList names = {QStringLiteral("OpenCL.dll")};
#elif defined(Q_OS_MACOS)
        const QStringList names = {QStringLiteral("/System/Library/Frameworks/OpenCL.framework/OpenCL")};
#else
        const QStringList names = {QStringLiteral("libOpenCL.so.1"), QStringLiteral("libOpenCL.so")};
#endif
        bool loaded = false;
        for (const QString &n : names) {
            lib.setFileName(n);
            if (lib.load()) {
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            throw Error("no OpenCL runtime found (it comes with the GPU driver)");
        }
        auto sym = [this](auto &pointer, const char *name) {
            pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(lib.resolve(name));
            if (!pointer) {
                throw Error(std::string("OpenCL runtime lacks ") + name);
            }
        };
        sym(getPlatformIds, "clGetPlatformIDs");
        sym(getDeviceIds, "clGetDeviceIDs");
        sym(getDeviceInfo, "clGetDeviceInfo");
        sym(createContext, "clCreateContext");
        sym(createCommandQueue, "clCreateCommandQueue");
        sym(createProgramWithSource, "clCreateProgramWithSource");
        sym(buildProgram, "clBuildProgram");
        sym(getProgramBuildInfo, "clGetProgramBuildInfo");
        sym(createKernel, "clCreateKernel");
        sym(createBuffer, "clCreateBuffer");
        sym(releaseMemObject, "clReleaseMemObject");
        sym(releaseKernel, "clReleaseKernel");
        sym(releaseCommandQueue, "clReleaseCommandQueue");
        sym(setKernelArg, "clSetKernelArg");
        sym(enqueueWriteBuffer, "clEnqueueWriteBuffer");
        sym(enqueueReadBuffer, "clEnqueueReadBuffer");
        sym(enqueueNdRangeKernel, "clEnqueueNDRangeKernel");
        sym(finish, "clFinish");
    }
};

std::shared_ptr<Device> Device::get()
{
    static std::mutex mutex;
    static std::shared_ptr<Device> device;
    static std::string failure;
    std::lock_guard<std::mutex> lock(mutex);
    if (device) {
        return device;
    }
    if (!failure.empty()) {
        throw Error(failure);
    }
    try {
        std::shared_ptr<Device> d(new Device());
        d->open();
        device = d;
    } catch (const Error &e) {
        failure = e.what();
        throw;
    }
    return device;
}

void Device::open()
{
    m_api = std::make_unique<Api>();
    m_api->load();
    const Api &api = *m_api;
    Uint np = 0;
    check(api.getPlatformIds(0, nullptr, &np), "clGetPlatformIDs");
    std::vector<Handle> platforms(np);
    check(api.getPlatformIds(np, platforms.data(), nullptr), "clGetPlatformIDs");
    std::vector<Handle> devices;
    for (Handle p : platforms) {
        Uint nd = 0;
        if (api.getDeviceIds(p, kDeviceTypeGpu, 0, nullptr, &nd) != 0 || nd == 0) {
            continue;
        }
        std::vector<Handle> d(nd);
        check(api.getDeviceIds(p, kDeviceTypeGpu, nd, d.data(), nullptr), "clGetDeviceIDs");
        devices.insert(devices.end(), d.begin(), d.end());
    }
    bool ok = false;
    const uint pick = QProcessEnvironment::systemEnvironment().value(QStringLiteral("LD_OPENCL_DEVICE")).toUInt(&ok);
    const size_t index = ok ? pick : 0;
    if (index >= devices.size()) {
        throw Error("no OpenCL GPU device (found " + std::to_string(devices.size()) + ")");
    }
    m_device = devices[index];
    Int err = 0;
    m_context = api.createContext(nullptr, 1, &m_device, nullptr, nullptr, &err);
    check(err, "clCreateContext");
    auto info = [&](Uint what) {
        size_t size = 0;
        check(api.getDeviceInfo(m_device, what, 0, nullptr, &size), "clGetDeviceInfo");
        std::string buf(size, '\0');
        check(api.getDeviceInfo(m_device, what, size, &buf[0], nullptr), "clGetDeviceInfo");
        while (!buf.empty() && buf.back() == '\0') {
            buf.pop_back();
        }
        return buf;
    };
    m_name = info(kDeviceName);
    if (info(kDeviceExtensions).find("cl_khr_fp64") == std::string::npos) {
        throw Error("OpenCL device " + m_name + " has no double precision (cl_khr_fp64)");
    }
}

Device::~Device() = default;

std::unique_ptr<Kernel> Device::kernel(const std::string &source, const std::string &options, const char *name)
{
    const Api &a = *m_api;
    const char *text = source.c_str();
    const size_t length = source.size();
    Int err = 0;
    Handle program = a.createProgramWithSource(m_context, 1, &text, &length, &err);
    check(err, "clCreateProgramWithSource");
    if (a.buildProgram(program, 1, &m_device, options.c_str(), nullptr, nullptr) != 0) {
        size_t size = 0;
        a.getProgramBuildInfo(program, m_device, kProgramBuildLog, 0, nullptr, &size);
        std::string log(size, '\0');
        a.getProgramBuildInfo(program, m_device, kProgramBuildLog, size, &log[0], nullptr);
        throw Error("OpenCL kernel build failed:\n" + log);
    }
    Handle k = a.createKernel(program, name, &err);
    check(err, "clCreateKernel");
    return std::make_unique<Kernel>(shared_from_this(), k);
}

Buffer::~Buffer()
{
    if (m_mem) {
        m_device->api().releaseMemObject(m_mem);
    }
}

void Buffer::ensure(size_t bytes)
{
    if (m_capacity >= std::max<size_t>(bytes, 1)) {
        return;
    }
    if (m_mem) {
        m_device->api().releaseMemObject(m_mem);
        m_mem = nullptr;
    }
    size_t cap = 1;
    while (cap < std::max<size_t>(bytes, 1)) {
        cap <<= 1;
    }
    Int err = 0;
    m_mem = m_device->api().createBuffer(m_device->context(), kMemReadWrite, cap, nullptr, &err);
    check(err, "clCreateBuffer");
    m_capacity = cap;
}

Kernel::~Kernel()
{
    m_device->api().releaseKernel(m_handle);
}

void Kernel::setBuffer(unsigned index, const Buffer &buffer)
{
    Handle mem = buffer.handle();
    check(m_device->api().setKernelArg(m_handle, index, sizeof(Handle), &mem), "clSetKernelArg");
}

void Kernel::setInt(unsigned index, int32_t value)
{
    check(m_device->api().setKernelArg(m_handle, index, sizeof(value), &value), "clSetKernelArg");
}

void Kernel::setFloat(unsigned index, float value)
{
    check(m_device->api().setKernelArg(m_handle, index, sizeof(value), &value), "clSetKernelArg");
}

Queue::Queue(std::shared_ptr<Device> device) : m_device(std::move(device))
{
    Int err = 0;
    m_handle = m_device->api().createCommandQueue(m_device->context(), m_device->deviceId(), 0, &err);
    check(err, "clCreateCommandQueue");
}

Queue::~Queue()
{
    m_device->api().releaseCommandQueue(m_handle);
}

void Queue::write(const Buffer &buffer, const void *data, size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    check(m_device->api().enqueueWriteBuffer(m_handle, buffer.handle(), kTrue, 0, bytes, data, 0, nullptr, nullptr), "clEnqueueWriteBuffer");
}

void Queue::read(const Buffer &buffer, void *data, size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    check(m_device->api().enqueueReadBuffer(m_handle, buffer.handle(), kTrue, 0, bytes, data, 0, nullptr, nullptr), "clEnqueueReadBuffer");
}

void Queue::launch(const Kernel &kernel, size_t global, size_t local)
{
    const size_t g = (global + local - 1) / local * local;
    check(m_device->api().enqueueNdRangeKernel(m_handle, kernel.handle(), 1, nullptr, &g, &local, 0, nullptr, nullptr), "clEnqueueNDRangeKernel");
}

void Queue::finish()
{
    check(m_device->api().finish(m_handle), "clFinish");
}

} // namespace cl
} // namespace rfx
