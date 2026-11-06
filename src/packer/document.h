/******************************************************************************
 * document.h
 * rf-packer - the list of tracks being made: those of an .rfx file and the captures added to them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "job.h"
#include "metadata.h"

#include <QObject>
#include <QString>

#include <vector>

// One line of the list: a track of the archive that was opened, or a capture that is to be added.
struct Entry
{
    bool fromArchive = false;
    // Which of the files of the list the track is in (0 is the first), and its number in it.
    size_t arch = 0;
    size_t src = 0;
    // The file, for a capture.
    QString capture;
    // What the archive says of the track, or what is known of the capture.
    rfx::SourceMeta meta;
    // Bytes the track takes in the archive (0 for a capture).
    uint64_t packed = 0;
    bool removed = false;

    // What it is now, from what the person changed.
    QString name;
    double rateHz = 0.0;
    QString notes;
    std::vector<rfx::Sidecar> sidecars;
    // What is said of the disc and the signal (see rfx::SourceMeta): empty, and 0, when not said.
    QString standard;
    QString reference;
    int copy = 0;
    int captureNumber = 0;
    QString face;
    QString signal;
};

enum class EntryState { Kept, Edited, Recode, New, Removed };

struct Summary
{
    int kept = 0;
    int added = 0;
    int removed = 0;
    int recoded = 0;
    int edited = 0;
};

class Document : public QObject
{
    Q_OBJECT

public:
    explicit Document(QObject *parent = nullptr) : QObject(parent) {}

    // Load the tracks of an .rfx file (the list starts from them). False, with the reason, when it is not one.
    bool open(const QString &path, QString *error);
    // Add the tracks of another .rfx file at the end: they are written along with the others, copied as they are where they can be (a file of the
    // predictor with one of the predictor, a file of the FLAC or raw variant with one of its variant). False, with the reason, when it cannot be.
    bool addArchive(const QString &path, QString *error);
    // Add a capture at the end. False, with the reason, when its format is not known.
    bool addCapture(const QString &path, QString *error);
    // An empty list, not made from an archive.
    void clear();

    Codec codec() const { return m_codec; }
    void setCodec(Codec codec);
    // What the samples contain, said once for the whole list: the rate (Hz) and the bits they carry. It is read from the archive
    // that was opened and cannot be changed then; only a new list is told what its captures hold.
    double rateHz() const { return m_rateHz; }
    int bits() const { return m_bits; }
    bool formatEditable() const { return !m_opened; }
    void setFormat(double rateHz, int bits);

    // The file the list was opened from (empty for a new list, even when files were added to it), and every file the tracks come from.
    QString archive() const { return m_opened && !m_archives.empty() ? m_archives.front() : QString(); }
    const std::vector<QString> &archives() const { return m_archives; }
    // The file of an entry (empty for a capture).
    QString archiveOf(int i) const;
    int count() const { return static_cast<int>(m_entries.size()); }
    const Entry &at(int i) const { return m_entries[static_cast<size_t>(i)]; }
    Entry &edit(int i) { return m_entries[static_cast<size_t>(i)]; }

    void remove(int i);
    void restore(int i);
    void move(int i, int delta);
    // Tell that an entry was changed through edit().
    void touched();

    EntryState state(int i) const;
    Summary summary() const;
    // Whether anything differs from what was opened (or the list is new and has tracks).
    bool isChanged() const;
    // The job that writes `output` from what is in the list.
    Job toJob(const QString &output, const rfx::PackOptions &options) const;

signals:
    void changed();

private:
    bool metadataEdited(const Entry &e) const;
    // The entries of a file, for the end of the list.
    std::vector<Entry> entriesOf(const QString &path, size_t arch, bool *stored, std::string *storedCodec, uint32_t *segment) const;

    std::vector<QString> m_archives;
    bool m_opened = false;
    std::vector<Entry> m_entries;
    bool m_touched = false;
    Codec m_codec = Codec::Rfx;
    double m_rateHz = 0.0;
    int m_bits = 0;
};

#endif // DOCUMENT_H
