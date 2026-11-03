/******************************************************************************
 * posfile.cpp
 * rfx library - file access by position, from several threads
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "posfile.h"

#include "error.h"

#include <cerrno>
#include <cstring>

namespace rfx {

namespace {

// 64-bit seeks: the captures are many gigabytes.
int seek(std::FILE *f, uint64_t offset, int whence)
{
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(offset), whence);
#else
    return fseeko(f, static_cast<off_t>(offset), whence);
#endif
}

int64_t tell(std::FILE *f)
{
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

std::string reason()
{
    return std::strerror(errno);
}

} // namespace

PosFile::PosFile(const QString &path, Mode mode) : m_path(path)
{
    const bool write = mode == Mode::Write;
#ifdef _WIN32
    m_file = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), write ? L"w+b" : L"rb");
#else
    m_file = std::fopen(path.toUtf8().constData(), write ? "w+b" : "rb");
#endif
    if (!m_file) {
        throw Error("cannot open " + path.toStdString() + ": " + reason());
    }
    std::setvbuf(m_file, nullptr, _IONBF, 0);
}

PosFile::~PosFile()
{
    if (m_file) {
        std::fclose(m_file);
    }
}

uint64_t PosFile::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (seek(m_file, 0, SEEK_END) != 0) {
        throw Error("cannot seek in " + m_path.toStdString() + ": " + reason());
    }
    const int64_t end = tell(m_file);
    if (end < 0) {
        throw Error("cannot tell the size of " + m_path.toStdString() + ": " + reason());
    }
    return static_cast<uint64_t>(end);
}

void PosFile::setSize(uint64_t size)
{
    if (size == 0 || size <= this->size()) {
        return;
    }
    // Writing the last byte extends the file (zero-filled, and sparse where the
    // system allows it).
    const char zero = 0;
    writeAt(&zero, 1, size - 1);
}

void PosFile::readAt(void *data, size_t length, uint64_t offset)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (seek(m_file, offset, SEEK_SET) != 0) {
        throw Error("cannot seek in " + m_path.toStdString() + ": " + reason());
    }
    if (length > 0 && std::fread(data, 1, length, m_file) != length) {
        throw Error("unexpected end of " + m_path.toStdString());
    }
}

void PosFile::writeAt(const void *data, size_t length, uint64_t offset)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (seek(m_file, offset, SEEK_SET) != 0) {
        throw Error("cannot seek in " + m_path.toStdString() + ": " + reason());
    }
    if (length > 0 && std::fwrite(data, 1, length, m_file) != length) {
        throw Error("cannot write " + m_path.toStdString() + ": " + reason());
    }
}

} // namespace rfx
