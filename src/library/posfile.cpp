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

namespace rfx {

PosFile::PosFile(const QString &path, Mode mode) : m_path(path), m_file(path)
{
    QIODevice::OpenMode open = QIODevice::Unbuffered;
    if (mode == Mode::Read) {
        open |= QIODevice::ReadOnly;
    } else {
        open |= QIODevice::ReadWrite | QIODevice::Truncate;
    }
    if (!m_file.open(open)) {
        throw Error("cannot open " + path.toStdString() + ": " + m_file.errorString().toStdString());
    }
}

uint64_t PosFile::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint64_t>(m_file.size());
}

void PosFile::setSize(uint64_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file.resize(static_cast<qint64>(size))) {
        throw Error("cannot set the size of " + m_path.toStdString() + ": " + m_file.errorString().toStdString());
    }
}

void PosFile::readAt(void *data, size_t length, uint64_t offset)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file.seek(static_cast<qint64>(offset))) {
        throw Error("cannot seek in " + m_path.toStdString());
    }
    char *out = static_cast<char *>(data);
    while (length > 0) {
        const qint64 got = m_file.read(out, static_cast<qint64>(length));
        if (got <= 0) {
            throw Error("unexpected end of " + m_path.toStdString());
        }
        out += got;
        length -= static_cast<size_t>(got);
    }
}

void PosFile::writeAt(const void *data, size_t length, uint64_t offset)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file.seek(static_cast<qint64>(offset))) {
        throw Error("cannot seek in " + m_path.toStdString());
    }
    const char *in = static_cast<const char *>(data);
    while (length > 0) {
        const qint64 put = m_file.write(in, static_cast<qint64>(length));
        if (put <= 0) {
            throw Error("cannot write " + m_path.toStdString() + ": " + m_file.errorString().toStdString());
        }
        in += put;
        length -= static_cast<size_t>(put);
    }
}

} // namespace rfx
