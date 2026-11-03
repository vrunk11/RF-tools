/******************************************************************************
 * gpupredictor.h
 * rfx library - the predictor on an OpenCL GPU
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include "opencl.h"
#include "predictor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rfx {

// The device buffers of one chunk of segments: it travels from the upload to the
// kernel to the download, and then returns to a pool.
struct GpuChunkBuffers
{
    explicit GpuChunkBuffers(const std::shared_ptr<cl::Device> &device) : input(device), window(device), output(device), lens(device) {}

    cl::Buffer input;
    cl::Buffer window;
    cl::Buffer output;
    cl::Buffer lens;
    // Segments held.
    size_t segments = 0;
};

// The predictor of predictor.cpp for many segments at once, one work-group of 32
// work-items per segment (kernel.cl). Only the prediction runs there: the range
// coder stays on the CPU, which is possible because the residual model never looks
// at a prediction. It gives the CPU's bits: the same f32 operations in the same
// order (the kernel asks the device for rounded multiplications, divisions and
// square roots).
//
// Upload, kernel and download each have a command queue of their own, so that one
// chunk can be going up while another runs and a third comes back (each of the
// three steps is for one thread at a time). `run` chains them for the callers that
// want one call.
class GpuPredictor
{
public:
    // The kernel for `preset`, for segments of `segment` samples. Throws rfx::Error
    // when there is no usable GPU.
    GpuPredictor(const Preset &preset, bool useRef, size_t segment);

    // Distance between the channels of a segment's reference window.
    size_t windowStride() const { return m_stride; }
    // Reference window length per segment, all channels.
    size_t windowLength() const { return m_windowLength; }

    std::unique_ptr<GpuChunkBuffers> newBuffers() const { return std::make_unique<GpuChunkBuffers>(m_device); }

    // Send `lens.size()` segments of `segment` values (samples, or residuals; the
    // tail of a short segment is padding), their reference windows (none without
    // references) and their real lengths.
    void upload(GpuChunkBuffers &buffers, const std::vector<int32_t> &input, const std::vector<float> &window, const std::vector<int32_t> &lens);
    // mode 0: the predictions of the samples uploaded; 1: the samples whose
    // residuals were uploaded. The result stays on the device.
    void predict(GpuChunkBuffers &buffers, int mode);
    std::vector<int32_t> download(const GpuChunkBuffers &buffers);

    // The three steps in one call.
    std::vector<int32_t> run(int mode, const std::vector<int32_t> &input, const std::vector<float> &window, const std::vector<int32_t> &lens);

private:
    std::shared_ptr<cl::Device> m_device;
    std::unique_ptr<cl::Kernel> m_kernel;
    std::unique_ptr<cl::Queue> m_uploadQueue;
    std::unique_ptr<cl::Queue> m_kernelQueue;
    std::unique_ptr<cl::Queue> m_downloadQueue;
    std::unique_ptr<GpuChunkBuffers> m_own;
    size_t m_segment;
    size_t m_stride;
    size_t m_windowLength;
    float m_lam;
};

} // namespace rfx
