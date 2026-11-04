/******************************************************************************
 * job.cpp
 * rf-packer - a piece of work for the program that does it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "job.h"

#include "capture.h"
#include "error.h"

#include "archive.h"
#include "flacmode.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QJsonValue sidecarsToJson(const std::vector<rfx::Sidecar> &list)
{
    QJsonArray out;
    for (const rfx::Sidecar &sc : list) {
        QJsonObject o;
        o["name"] = QString::fromStdString(sc.name);
        o["sha256"] = QString::fromStdString(sc.sha256);
        o["data"] = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(sc.data.data()), static_cast<qsizetype>(sc.data.size())).toBase64());
        out.append(o);
    }
    return out;
}

std::vector<rfx::Sidecar> sidecarsFromJson(const QJsonArray &list)
{
    std::vector<rfx::Sidecar> out;
    for (const QJsonValue &v : list) {
        const QJsonObject o = v.toObject();
        rfx::Sidecar sc;
        sc.name = o["name"].toString().toStdString();
        sc.sha256 = o["sha256"].toString().toStdString();
        const QByteArray raw = QByteArray::fromBase64(o["data"].toString().toLatin1());
        sc.data.assign(raw.begin(), raw.end());
        out.push_back(std::move(sc));
    }
    return out;
}

} // namespace

QByteArray jobToJson(const Job &job)
{
    QJsonObject root;
    root["archive"] = job.archive;
    root["output"] = job.output;
    root["codec"] = job.codec == Codec::Flac ? "flac" : (job.codec == Codec::Raw ? "raw" : "rfx");
    QJsonArray picks;
    for (const rfx::Pick &p : job.picks) {
        QJsonObject o;
        o["from_archive"] = p.fromArchive;
        o["src"] = static_cast<double>(p.src);
        o["capture"] = p.capture;
        QJsonObject e;
        if (p.edit.name) {
            e["name"] = QString::fromStdString(*p.edit.name);
        }
        if (p.edit.rateHz) {
            e["rate_hz"] = *p.edit.rateHz;
        }
        if (p.edit.notes) {
            e["notes"] = QString::fromStdString(*p.edit.notes);
        }
        if (p.edit.sidecars) {
            e["sidecars"] = sidecarsToJson(*p.edit.sidecars);
        }
        if (p.edit.bits) {
            e["bits"] = static_cast<double>(*p.edit.bits);
        }
        o["edit"] = e;
        picks.append(o);
    }
    root["picks"] = picks;
    QJsonObject opt;
    opt["level"] = job.options.level;
    opt["segment"] = static_cast<double>(job.options.segment);
    opt["verify"] = job.options.verify;
    opt["rate_mhz"] = job.options.rateMhz;
    opt["device"] = job.options.device == rfx::Device::Gpu ? "gpu" : "cpu";
    opt["threads"] = static_cast<double>(job.options.threads);
    opt["ld_decode"] = job.options.ldDecode;
    if (job.options.pass0Dir) {
        opt["pass0"] = *job.options.pass0Dir;
    }
    root["options"] = opt;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

Job jobFromJson(const QByteArray &json)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (doc.isNull() || !doc.isObject()) {
        throw rfx::Error("the job is not a JSON document: " + error.errorString().toStdString());
    }
    const QJsonObject root = doc.object();
    Job job;
    job.archive = root["archive"].toString();
    job.output = root["output"].toString();
    const QString codec = root["codec"].toString();
    job.codec = codec == QLatin1String("flac") ? Codec::Flac : (codec == QLatin1String("raw") ? Codec::Raw : Codec::Rfx);
    for (const QJsonValue &v : root["picks"].toArray()) {
        const QJsonObject o = v.toObject();
        rfx::Pick p;
        p.fromArchive = o["from_archive"].toBool();
        p.arch = 0;
        p.src = static_cast<size_t>(o["src"].toDouble());
        p.capture = o["capture"].toString();
        const QJsonObject e = o["edit"].toObject();
        if (e.contains("name")) {
            p.edit.name = e["name"].toString().toStdString();
        }
        if (e.contains("rate_hz")) {
            p.edit.rateHz = e["rate_hz"].toDouble();
        }
        if (e.contains("notes")) {
            p.edit.notes = e["notes"].toString().toStdString();
        }
        if (e.contains("sidecars")) {
            p.edit.sidecars = sidecarsFromJson(e["sidecars"].toArray());
        }
        if (e.contains("bits")) {
            p.edit.bits = static_cast<uint32_t>(e["bits"].toDouble());
        }
        job.picks.push_back(std::move(p));
    }
    const QJsonObject opt = root["options"].toObject();
    job.options.level = opt["level"].toInt(3);
    job.options.segment = static_cast<size_t>(opt["segment"].toDouble(262144));
    job.options.verify = opt["verify"].toBool(true);
    job.options.rateMhz = opt["rate_mhz"].toDouble(75.0);
    job.options.device = opt["device"].toString() == QLatin1String("gpu") ? rfx::Device::Gpu : rfx::Device::Cpu;
    job.options.threads = static_cast<unsigned>(opt["threads"].toDouble(0));
    job.options.ldDecode = opt["ld_decode"].toString(QStringLiteral("ld-decode"));
    if (opt.contains("pass0")) {
        job.options.pass0Dir = opt["pass0"].toString();
    }
    job.options.order = rfx::OrderMode::Given;
    return job;
}

namespace {

// The captures of the list as files, in its order: a new capture is where it is, a track of the archive is unpacked into a folder
// of its own under `work` (the names of tracks may repeat).
std::vector<QString> listAsFiles(const Job &job, const QString &work)
{
    std::vector<QString> files;
    std::optional<rfx::Archive> archive;
    for (size_t k = 0; k < job.picks.size(); ++k) {
        const rfx::Pick &p = job.picks[k];
        if (!p.fromArchive) {
            files.push_back(p.capture);
            continue;
        }
        if (!archive) {
            archive.emplace(job.archive);
        }
        const QString dir = work + QStringLiteral("/") + QString::number(k);
        QDir().mkpath(dir);
        rfx::UnpackOptions unpack;
        unpack.only = p.src;
        unpack.device = job.options.device;
        unpack.threads = job.options.threads;
        rfx::unpackArchive(job.archive, dir, unpack);
        files.push_back(dir + QStringLiteral("/") + QString::fromStdString(archive->meta().sources[p.src].name));
    }
    return files;
}

} // namespace

void runJob(const Job &job)
{
    if (job.picks.empty()) {
        throw rfx::Error("nothing to write: the list has no track");
    }
    if (job.codec == Codec::Rfx) {
        std::vector<QString> archives;
        if (!job.archive.isEmpty()) {
            archives.push_back(job.archive);
        }
        rfx::assembleArchives(archives, job.picks, job.output, job.options, std::nullopt);
        return;
    }
    if (job.codec == Codec::Flac) {
        const QString work = rfx::withExtension(job.output, QStringLiteral("work"));
        QDir(work).removeRecursively();
        rfx::PackOptions options = job.options;
        options.order = rfx::OrderMode::Given;
        // The rate of the list (FLAC says it in its header); the captures are in the order they have in the list.
        for (const rfx::Pick &p : job.picks) {
            if (p.edit.rateHz && *p.edit.rateHz > 0) {
                options.rateMhz = *p.edit.rateHz / 1e6;
                break;
            }
        }
        const std::vector<QString> files = listAsFiles(job, work);
        rfx::packFlac(files, job.output, options);
        QDir(work).removeRecursively();
        return;
    }
    // Raw captures, in a folder: each with its .yaml, named as it was.
    QDir().mkpath(job.output);
    for (const rfx::Pick &p : job.picks) {
        if (p.fromArchive) {
            rfx::UnpackOptions unpack;
            unpack.only = p.src;
            unpack.device = job.options.device;
            unpack.threads = job.options.threads;
            // Into a folder of its own first: unpacking one track may clear what an earlier one left in the folder.
            const QString staging = rfx::withExtension(job.output, QStringLiteral("work")) + QStringLiteral("/") + QString::number(p.src);
            QDir(staging).removeRecursively();
            QDir().mkpath(staging);
            rfx::unpackArchive(job.archive, staging, unpack);
            for (const QFileInfo &f : QDir(staging).entryInfoList(QDir::Files)) {
                const QString target = job.output + QStringLiteral("/") + f.fileName();
                QFile::remove(target);
                if (!QFile::rename(f.absoluteFilePath(), target)) {
                    throw rfx::Error("cannot move " + f.fileName().toStdString() + " to " + job.output.toStdString());
                }
            }
            QDir(rfx::withExtension(job.output, QStringLiteral("work"))).removeRecursively();
        } else {
            const QString target = job.output + QStringLiteral("/") + QFileInfo(p.capture).fileName();
            QFile::remove(target);
            if (!QFile::copy(p.capture, target)) {
                throw rfx::Error("cannot copy " + p.capture.toStdString() + " to " + target.toStdString());
            }
        }
    }
}

void removeJobLeftovers(const QString &output)
{
    // The names are those of the library (the output with its last extension replaced).
    for (const char *extension : {"rfx-tmp"}) {
        QFile::remove(rfx::withExtension(output, QString::fromLatin1(extension)));
    }
    for (const char *extension : {"edit", "work"}) {
        QDir(rfx::withExtension(output, QString::fromLatin1(extension))).removeRecursively();
    }
}
