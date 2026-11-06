/******************************************************************************
 * document.cpp
 * rf-packer - the list of tracks being made
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "document.h"

#include "archive.h"
#include "capture.h"
#include "error.h"
#include "flacmode.h"
#include "sampleformat.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace {

bool isCompressedFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("flac") || suffix == QLatin1String("ldf");
}

bool sameSidecars(const std::vector<rfx::Sidecar> &a, const std::vector<rfx::Sidecar> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].data != b[i].data) {
            return false;
        }
    }
    return true;
}

} // namespace

// The tracks of a file as entries: of the predictor, or of the FLAC or raw variant (its tracks take their share of the file).
std::vector<Entry> Document::entriesOf(const QString &path, size_t arch, bool *stored, std::string *storedCodec, uint32_t *segment) const
{
    *stored = rfx::isFlacArchive(path);
    std::optional<rfx::Archive> archive;
    std::vector<rfx::SourceMeta> sources;
    if (*stored) {
        sources = rfx::readFlacMeta(path, storedCodec);
        *segment = 0;
    } else {
        archive.emplace(path);
        sources = archive->meta().sources;
        *segment = static_cast<uint32_t>(archive->meta().segment);
    }
    std::vector<Entry> entries;
    for (size_t k = 0; k < sources.size(); ++k) {
        Entry e;
        e.fromArchive = true;
        e.arch = arch;
        e.src = k;
        e.meta = sources[k];
        e.packed = *stored ? static_cast<uint64_t>(QFileInfo(path).size()) / std::max<size_t>(1, sources.size()) : archive->packedBytes(k);
        e.name = QString::fromStdString(e.meta.name);
        e.rateHz = e.meta.rateHz;
        e.notes = QString::fromStdString(e.meta.notes);
        e.sidecars = e.meta.sidecars;
        e.standard = QString::fromStdString(e.meta.standard);
        e.reference = QString::fromStdString(e.meta.reference);
        e.copy = static_cast<int>(e.meta.copy);
        e.captureNumber = static_cast<int>(e.meta.capture);
        e.face = QString::fromStdString(e.meta.face);
        e.signal = QString::fromStdString(e.meta.signal);
        entries.push_back(std::move(e));
    }
    return entries;
}

bool Document::open(const QString &path, QString *error)
{
    try {
        bool stored = false;
        std::string storedCodec;
        uint32_t segment = 0;
        std::vector<Entry> entries = entriesOf(path, 0, &stored, &storedCodec, &segment);
        m_entries = std::move(entries);
        m_archives = {path};
        m_opened = true;
        m_codec = stored ? (storedCodec == "raw" ? Codec::Raw : Codec::Flac) : Codec::Rfx;
        m_rateHz = m_entries.empty() ? 0.0 : m_entries.front().meta.rateHz;
        m_bits = m_entries.empty() ? 0 : static_cast<int>(m_entries.front().meta.bits);
        m_touched = false;
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    emit changed();
    return true;
}

bool Document::addArchive(const QString &path, QString *error)
{
    try {
        if (std::find(m_archives.begin(), m_archives.end(), path) != m_archives.end()) {
            throw rfx::Error(QFileInfo(path).fileName().toStdString() + " is in the list already");
        }
        bool stored = false;
        std::string storedCodec;
        uint32_t segment = 0;
        std::vector<Entry> entries = entriesOf(path, m_archives.size(), &stored, &storedCodec, &segment);
        if (!m_archives.empty()) {
            // The files of the list are of one family: tracks of the predictor, with one segment length, or tracks of the FLAC or raw variant.
            const QString &first = m_archives.front();
            bool firstStored = false;
            std::string firstCodec;
            uint32_t firstSegment = 0;
            entriesOf(first, 0, &firstStored, &firstCodec, &firstSegment);
            if (firstStored != stored) {
                throw rfx::Error("a file of the predictor and a file of the FLAC or raw variant cannot be written together: save the one as the variant of the other first");
            }
            if (!stored && firstSegment != segment) {
                throw rfx::Error("the files do not use the same segment length");
            }
        } else {
            // A new list takes the variant of the file it starts from.
            m_codec = stored ? (storedCodec == "raw" ? Codec::Raw : Codec::Flac) : Codec::Rfx;
        }
        if (m_rateHz <= 0.0 && !entries.empty()) {
            m_rateHz = entries.front().meta.rateHz;
            m_bits = static_cast<int>(entries.front().meta.bits);
        }
        for (Entry &e : entries) {
            m_entries.push_back(std::move(e));
        }
        m_archives.push_back(path);
        m_touched = true;
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    emit changed();
    return true;
}

QString Document::archiveOf(int i) const
{
    const Entry &e = m_entries[static_cast<size_t>(i)];
    return e.fromArchive && e.arch < m_archives.size() ? m_archives[e.arch] : QString();
}
bool Document::addCapture(const QString &path, QString *error)
{
    try {
        const QFileInfo info(path);
        if (!info.isFile()) {
            throw rfx::Error(path.toStdString() + " is not a file");
        }
        const rfx::SampleFormat format = rfx::inputFormat(path, "auto");
        Entry e;
        e.capture = path;
        e.meta.name = info.fileName().toStdString();
        e.meta.size = static_cast<uint64_t>(info.size());
        e.meta.fmt = rfx::formatName(format);
        // A compressed capture is decoded when it is packed: its samples are not counted here.
        e.meta.words = isCompressedFile(path) ? 0 : rfx::formatSamples(format, e.meta.size);
        e.meta.sidecars = rfx::findSidecars(path);
        const std::optional<double> hz = rfx::sidecarRateHz(e.meta.sidecars);
        e.meta.rateHz = hz.value_or(0.0);
        if (!m_opened && m_rateHz <= 0.0 && hz) {
            m_rateHz = *hz;
        }
        e.meta.fileTime = info.lastModified().toUTC().toString(Qt::ISODate).toStdString();
        e.name = QString::fromStdString(e.meta.name);
        e.rateHz = e.meta.rateHz;
        e.sidecars = e.meta.sidecars;
        m_entries.push_back(std::move(e));
    } catch (const std::exception &e) {
        if (error) {
            *error = QString::fromUtf8(e.what());
        }
        return false;
    }
    emit changed();
    return true;
}

void Document::setCodec(Codec codec)
{
    m_codec = codec;
    emit changed();
}

void Document::setFormat(double rateHz, int bits)
{
    m_rateHz = rateHz;
    m_bits = bits;
    m_touched = true;
    emit changed();
}

void Document::clear()
{
    m_entries.clear();
    m_archives.clear();
    m_opened = false;
    m_rateHz = 0.0;
    m_bits = 0;
    m_codec = Codec::Rfx;
    m_touched = false;
    emit changed();
}

void Document::remove(int i)
{
    Entry &e = m_entries[static_cast<size_t>(i)];
    if (e.fromArchive) {
        e.removed = true;
    } else {
        m_entries.erase(m_entries.begin() + i);
    }
    m_touched = true;
    emit changed();
}

void Document::restore(int i)
{
    m_entries[static_cast<size_t>(i)].removed = false;
    emit changed();
}

void Document::move(int i, int delta)
{
    const int j = i + delta;
    if (i < 0 || j < 0 || i >= count() || j >= count()) {
        return;
    }
    std::swap(m_entries[static_cast<size_t>(i)], m_entries[static_cast<size_t>(j)]);
    m_touched = true;
    emit changed();
}

void Document::touched()
{
    m_touched = true;
    emit changed();
}

bool Document::metadataEdited(const Entry &e) const
{
    return e.name != QString::fromStdString(e.meta.name) || e.notes != QString::fromStdString(e.meta.notes) ||
           !sameSidecars(e.sidecars, e.meta.sidecars) || e.standard != QString::fromStdString(e.meta.standard) ||
           e.reference != QString::fromStdString(e.meta.reference) || e.copy != static_cast<int>(e.meta.copy) || e.captureNumber != static_cast<int>(e.meta.capture) ||
           e.face != QString::fromStdString(e.meta.face) || e.signal != QString::fromStdString(e.meta.signal);
}

EntryState Document::state(int i) const
{
    const Entry &e = m_entries[static_cast<size_t>(i)];
    if (e.removed) {
        return EntryState::Removed;
    }
    if (!e.fromArchive) {
        return EntryState::New;
    }
    // A track is predicted from tracks before it: it can be copied as it is only if every one of them is still there, before
    // it. Where the tracks of the archive stand in what is to be written, by their number in the archive:
    std::vector<std::pair<std::pair<size_t, size_t>, int>> where;
    int index = 0;
    int mine = -1;
    for (const Entry &other : m_entries) {
        if (other.removed) {
            continue;
        }
        if (other.fromArchive) {
            where.emplace_back(std::make_pair(other.arch, other.src), index);
            if (&other == &e) {
                mine = index;
            }
        }
        ++index;
    }
    for (const size_t r : e.meta.refs) {
        const auto it = std::find_if(where.begin(), where.end(), [&e, r](const std::pair<std::pair<size_t, size_t>, int> &w) { return w.first == std::make_pair(e.arch, r); });
        if (it == where.end() || it->second >= mine) {
            return EntryState::Recode;
        }
    }
    return metadataEdited(e) ? EntryState::Edited : EntryState::Kept;
}

Summary Document::summary() const
{
    Summary s;
    for (int i = 0; i < count(); ++i) {
        switch (state(i)) {
        case EntryState::Kept: ++s.kept; break;
        case EntryState::Edited: ++s.kept; ++s.edited; break;
        case EntryState::Recode: ++s.kept; ++s.recoded; break;
        case EntryState::New: ++s.added; break;
        case EntryState::Removed: ++s.removed; break;
        }
    }
    return s;
}

bool Document::isChanged() const
{
    if (!m_opened) {
        return !m_entries.empty();
    }
    if (m_archives.size() > 1) {
        return true;
    }
    if (m_codec != Codec::Rfx) {
        return true;
    }
    if (m_touched) {
        return true;
    }
    for (int i = 0; i < count(); ++i) {
        const EntryState s = state(i);
        if (s != EntryState::Kept) {
            return true;
        }
    }
    return false;
}

Job Document::toJob(const QString &output, const rfx::PackOptions &options) const
{
    Job job;
    if (!m_archives.empty()) {
        job.archive = m_archives.front();
        for (size_t a = 1; a < m_archives.size(); ++a) {
            job.extraArchives << m_archives[a];
        }
    }
    job.output = output;
    job.options = options;
    job.codec = m_codec;
    for (const Entry &e : m_entries) {
        if (e.removed) {
            continue;
        }
        rfx::Pick p = e.fromArchive ? rfx::Pick::old(e.arch, e.src) : rfx::Pick::fresh(e.capture);
        if (e.fromArchive) {
            if (e.name != QString::fromStdString(e.meta.name)) {
                p.edit.name = e.name.toStdString();
            }
            if (e.notes != QString::fromStdString(e.meta.notes)) {
                p.edit.notes = e.notes.toStdString();
            }
            if (!sameSidecars(e.sidecars, e.meta.sidecars)) {
                p.edit.sidecars = e.sidecars;
            }
            // (what the person changed of the disc and the signal, and only that)
            if (e.standard != QString::fromStdString(e.meta.standard)) {
                p.edit.info.standard = e.standard.toStdString();
            }
            if (e.reference != QString::fromStdString(e.meta.reference)) {
                p.edit.info.reference = e.reference.toStdString();
            }
            if (e.copy != static_cast<int>(e.meta.copy)) {
                p.edit.info.copy = static_cast<uint32_t>(e.copy);
            }
            if (e.captureNumber != static_cast<int>(e.meta.capture)) {
                p.edit.info.capture = static_cast<uint32_t>(e.captureNumber);
            }
            if (e.face != QString::fromStdString(e.meta.face)) {
                p.edit.info.face = e.face.toStdString();
            }
            if (e.signal != QString::fromStdString(e.meta.signal)) {
                p.edit.info.signal = e.signal.toStdString();
            }
        } else {
            if (m_rateHz > 0) {
                p.edit.rateHz = m_rateHz;
            } else if (e.meta.rateHz > 0) {
                p.edit.rateHz = e.meta.rateHz;
            }
            if (m_bits > 0) {
                p.edit.bits = static_cast<uint32_t>(m_bits);
            }
            if (!e.notes.isEmpty()) {
                p.edit.notes = e.notes.toStdString();
            }
            // (a capture that is new has no earlier value: what is said is what it gets)
            if (!e.standard.isEmpty()) {
                p.edit.info.standard = e.standard.toStdString();
            }
            if (!e.reference.isEmpty()) {
                p.edit.info.reference = e.reference.toStdString();
            }
            if (e.copy > 0) {
                p.edit.info.copy = static_cast<uint32_t>(e.copy);
            }
            if (e.captureNumber > 0) {
                p.edit.info.capture = static_cast<uint32_t>(e.captureNumber);
            }
            if (!e.face.isEmpty()) {
                p.edit.info.face = e.face.toStdString();
            }
            if (!e.signal.isEmpty()) {
                p.edit.info.signal = e.signal.toStdString();
            }
        }
        job.picks.push_back(std::move(p));
    }
    return job;
}
