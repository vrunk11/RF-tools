/******************************************************************************
 * job.h
 * rf-packer - a piece of work for the program that does it: what to write, from what
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef JOB_H
#define JOB_H

#include "edit.h"
#include "packer.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

// How the list is written: with the predictor (an .rfx file), as FLAC (an .rfx file that audio programs open) or as raw captures in a
// folder.
enum class Codec { Rfx, Flac, Raw };

// What the window asks the program to do, in a file (the window runs the program again with it, so that the work can be
// stopped and shows what it says as it goes): the archive it starts from (none for a new one), the tracks to write in
// their order, each from the archive or from a capture, with what is changed in it, and how to code.
struct Job
{
    // The .rfx file the tracks come from; empty when the list is new.
    QString archive;
    // The other .rfx files that tracks come from, after `archive` (a pick says which by its number: 0 is `archive`, 1 the first of these, and so on).
    QStringList extraArchives;
    // The file to write (it may be the archive: it is written aside and takes its name).
    QString output;
    std::vector<rfx::Pick> picks;
    rfx::PackOptions options;
    Codec codec = Codec::Rfx;
    // Give the captures back instead of writing a file: `output` is then the folder, `only` the track of the archive (all when empty).
    bool unpack = false;
    // Locate the lines of the tracks of the archive (only: that one) and keep them in it, as it is (nothing is coded again).
    bool pass0Only = false;
    // Check the tracks of the archive (only: that one) against the checksums the file keeps, decoding them; nothing is written.
    bool verify = false;
    std::optional<size_t> only;
};

// Every .rfx file of the job, in the order of the numbers of the picks.
std::vector<QString> jobArchives(const Job &job);

QByteArray jobToJson(const Job &job);
// Throws rfx::Error for a document that is not a job.
Job jobFromJson(const QByteArray &json);

// Does the work of the job. Progress goes to the standard error as the library says it. Throws rfx::Error.
void runJob(const Job &job);

// What an interrupted job leaves beside its output: the file it writes aside and the folders it works in.
void removeJobLeftovers(const QString &output);

#endif // JOB_H
