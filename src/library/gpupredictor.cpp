/******************************************************************************
 * gpupredictor.cpp
 * rfx library - the predictor on an OpenCL GPU
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "gpupredictor.h"

#include "error.h"

#include <QProcessEnvironment>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rfx {

namespace {

// The source of kernel.cl, as bytes, ending with a 0.
const char kKernelSource[] = {
#include "kernel_source.inc"
};

// Work-items in a group: one group per segment.
constexpr size_t kGroupSize = 32;

} // namespace

GpuPredictor::GpuPredictor(const Preset &pre, bool useRef, size_t segment)
    : m_device(cl::Device::get()),
      m_segment(segment),
      m_stride(segment + 2 * pre.taps),
      m_windowLength(pre.channels() * (segment + 2 * pre.taps))
{
    std::string options = "-DOWN=" + std::to_string(pre.ownFor(useRef)) + " -DTAPS=" + std::to_string(pre.taps)
        + " -DNTOP=" + std::to_string(useRef ? pre.ntop : 0) + " -DTT=" + std::to_string(pre.ttaps)
        + " -DUSEREF=" + std::to_string(useRef ? 1 : 0) + " -DSTRIDE=" + std::to_string(pre.stride)
        + " -DEVERY=" + std::to_string(pre.every) + " -DD=" + std::to_string(pre.dimension(useRef))
        + " -DDP=" + std::to_string(pre.leaves(useRef));
    if (m_device->name().find("NVIDIA") != std::string::npos) {
        options += " -DNVIDIA=1";
    }
    options += " " + QProcessEnvironment::systemEnvironment().value(QStringLiteral("RFX_GPU_DEFS")).toStdString();
    std::fprintf(stderr, "OpenCL device %s\n", m_device->name().c_str());
    m_kernel = m_device->kernel(std::string(kKernelSource, sizeof(kKernelSource) - 1), options, "rfx_seg");
    m_uploadQueue = std::make_unique<cl::Queue>(m_device);
    m_kernelQueue = std::make_unique<cl::Queue>(m_device);
    m_downloadQueue = std::make_unique<cl::Queue>(m_device);
    std::memcpy(&m_lam, &pre.lamBits, sizeof(float));
}

void GpuPredictor::upload(GpuChunkBuffers &b, const std::vector<int32_t> &input, const std::vector<float> &window, const std::vector<int32_t> &lens)
{
    b.segments = lens.size();
    b.input.ensure(input.size() * 4);
    b.output.ensure(input.size() * 4);
    b.lens.ensure(lens.size() * 4);
    b.window.ensure(std::max<size_t>(window.size(), 1) * 4);
    m_uploadQueue->write(b.input, input.data(), input.size() * 4);
    m_uploadQueue->write(b.lens, lens.data(), lens.size() * 4);
    if (!window.empty()) {
        m_uploadQueue->write(b.window, window.data(), window.size() * 4);
    }
    m_uploadQueue->finish();
}

void GpuPredictor::predict(GpuChunkBuffers &b, int mode)
{
    m_kernel->setBuffer(0, b.input);
    m_kernel->setBuffer(1, b.window);
    m_kernel->setBuffer(2, b.output);
    m_kernel->setBuffer(3, b.lens);
    m_kernel->setInt(4, static_cast<int32_t>(m_segment));
    m_kernel->setInt(5, static_cast<int32_t>(m_windowLength));
    m_kernel->setInt(6, static_cast<int32_t>(m_stride));
    m_kernel->setFloat(7, m_lam);
    m_kernel->setInt(8, mode);
    m_kernelQueue->launch(*m_kernel, b.segments * kGroupSize, kGroupSize);
    m_kernelQueue->finish();
}

std::vector<int32_t> GpuPredictor::download(const GpuChunkBuffers &b)
{
    std::vector<int32_t> out(b.segments * m_segment);
    m_downloadQueue->read(b.output, out.data(), out.size() * 4);
    m_downloadQueue->finish();
    return out;
}

std::vector<int32_t> GpuPredictor::run(int mode, const std::vector<int32_t> &input, const std::vector<float> &window,
                                       const std::vector<int32_t> &lens)
{
    if (!m_own) {
        m_own = newBuffers();
    }
    upload(*m_own, input, window, lens);
    predict(*m_own, mode);
    return download(*m_own);
}

} // namespace rfx
