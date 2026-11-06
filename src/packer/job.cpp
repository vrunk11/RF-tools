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
#include "rfsync.h"
#include "verify.h"

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

std::vector<QString> jobArchives(const Job &job)
{
    std::vector<QString> out;
    if (!job.archive.isEmpty()) {
        out.push_back(job.archive);
        for (const QString &a : job.extraArchives) {
            out.push_back(a);
        }
    }
    return out;
}

QByteArray jobToJson(const Job &job)
{
    QJsonObject root;
    root["archive"] = job.archive;
    if (!job.extraArchives.isEmpty()) {
        root["extra_archives"] = QJsonArray::fromStringList(job.extraArchives);
    }
    root["output"] = job.output;
    root["codec"] = job.codec == Codec::Flac ? "flac" : (job.codec == Codec::Raw ? "raw" : "rfx");
    if (job.pass0Only) {
        root["pass0_only"] = true;
        if (job.only) {
            root["only"] = static_cast<double>(*job.only);
        }
    }
    if (job.verify) {
        root["verify"] = true;
        if (job.only) {
            root["only"] = static_cast<double>(*job.only);
        }
    }
    if (job.unpack) {
        root["unpack"] = true;
        if (job.only) {
            root["only"] = static_cast<double>(*job.only);
        }
    }
    QJsonArray picks;
    for (const rfx::Pick &p : job.picks) {
        QJsonObject o;
        o["from_archive"] = p.fromArchive;
        o["arch"] = static_cast<double>(p.arch);
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
        if (p.edit.info.standard) {
            e["standard"] = QString::fromStdString(*p.edit.info.standard);
        }
        if (p.edit.info.reference) {
            e["reference"] = QString::fromStdString(*p.edit.info.reference);
        }
        if (p.edit.info.copy) {
            e["copy"] = static_cast<double>(*p.edit.info.copy);
        }
        if (p.edit.info.capture) {
            e["capture"] = static_cast<double>(*p.edit.info.capture);
        }
        if (p.edit.info.face) {
            e["face"] = QString::fromStdString(*p.edit.info.face);
        }
        if (p.edit.info.signal) {
            e["signal"] = QString::fromStdString(*p.edit.info.signal);
        }
        o["edit"] = e;
        picks.append(o);
    }
    root["picks"] = picks;
    QJsonObject opt;
    opt["level"] = job.options.level;
    opt["segment"] = static_cast<double>(job.options.segment);
    opt["verify"] = job.options.verify;
    opt["keep_pass0"] = job.options.keepPass0;
    opt["assume_ld"] = job.options.assumeLaserdisc;
    if (job.options.medium) {
        opt["medium"] = QString::fromStdString(*job.options.medium);
    }
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
    for (const QJsonValue &v : root["extra_archives"].toArray()) {
        job.extraArchives << v.toString();
    }
    job.output = root["output"].toString();
    const QString codec = root["codec"].toString();
    job.codec = codec == QLatin1String("flac") ? Codec::Flac : (codec == QLatin1String("raw") ? Codec::Raw : Codec::Rfx);
    job.unpack = root["unpack"].toBool();
    job.pass0Only = root["pass0_only"].toBool();
    job.verify = root["verify"].toBool();
    if (root.contains("only")) {
        job.only = static_cast<size_t>(root["only"].toDouble());
    }
    for (const QJsonValue &v : root["picks"].toArray()) {
        const QJsonObject o = v.toObject();
        rfx::Pick p;
        p.fromArchive = o["from_archive"].toBool();
        p.arch = static_cast<size_t>(o["arch"].toDouble());
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
        if (e.contains("standard")) {
            p.edit.info.standard = e["standard"].toString().toStdString();
        }
        if (e.contains("reference")) {
            p.edit.info.reference = e["reference"].toString().toStdString();
        }
        if (e.contains("copy")) {
            p.edit.info.copy = static_cast<uint32_t>(e["copy"].toDouble());
        }
        if (e.contains("capture")) {
            p.edit.info.capture = static_cast<uint32_t>(e["capture"].toDouble());
        }
        if (e.contains("face")) {
            p.edit.info.face = e["face"].toString().toStdString();
        }
        if (e.contains("signal")) {
            p.edit.info.signal = e["signal"].toString().toStdString();
        }
        job.picks.push_back(std::move(p));
    }
    const QJsonObject opt = root["options"].toObject();
    job.options.level = opt["level"].toInt(3);
    job.options.segment = static_cast<size_t>(opt["segment"].toDouble(262144));
    job.options.verify = opt["verify"].toBool(true);
    job.options.keepPass0 = opt["keep_pass0"].toBool(false);
    job.options.assumeLaserdisc = opt["assume_ld"].toBool(false);
    if (opt.contains("medium")) {
        job.options.medium = opt["medium"].toString().toStdString();
    }
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
    const std::vector<QString> archives = jobArchives(job);
    for (size_t k = 0; k < job.picks.size(); ++k) {
        const rfx::Pick &p = job.picks[k];
        if (!p.fromArchive) {
            files.push_back(p.capture);
            continue;
        }
        if (p.arch >= archives.size()) {
            throw rfx::Error("a track comes from a file that is not in the job");
        }
        const QString &path = archives[p.arch];
        const QString dir = work + QStringLiteral("/") + QString::number(k);
        QDir().mkpath(dir);
        if (rfx::isFlacArchive(path)) {
            const std::vector<rfx::SourceMeta> stored = rfx::readFlacMeta(path);
            rfx::unpackFlac(path, dir, p.src, std::nullopt);
            files.push_back(dir + QStringLiteral("/") + QString::fromStdString(stored[p.src].name));
            continue;
        }
        const rfx::Archive archive(path);
        rfx::UnpackOptions unpack;
        unpack.only = p.src;
        unpack.device = job.options.device;
        unpack.threads = job.options.threads;
        rfx::unpackArchive(path, dir, unpack);
        files.push_back(dir + QStringLiteral("/") + QString::fromStdString(archive.meta().sources[p.src].name));
    }
    return files;
}
} // namespace

void runJob(const Job &job)
{
    if (job.verify) {
        if (job.archive.isEmpty()) {
            throw rfx::Error("nothing to check: no .rfx file is open");
        }
        const std::vector<rfx::TrackCheck> checks = rfx::verifyArchive(job.archive, job.only, [](double part) {
            std::fprintf(stderr, "verify: %d%%\r", static_cast<int>(part * 100.0));
            std::fflush(stderr);
        });
        std::fprintf(stderr, "verify: 100%%\n");
        std::string damaged;
        size_t intact = 0;
        size_t unchecked = 0;
        for (const rfx::TrackCheck &c : checks) {
            if (!c.ok) {
                damaged += (damaged.empty() ? "" : "; ") + std::string("track ") + std::to_string(c.track) + " (" + c.name + "): " + c.detail;
            } else if (!c.checked) {
                ++unchecked;
                std::fprintf(stderr, "track %zu (%s): %s\n", c.track, c.name.c_str(), c.detail.c_str());
            } else {
                ++intact;
                std::fprintf(stderr, "track %zu (%s): SHA-256 of the samples matches\n", c.track, c.name.c_str());
            }
        }
        if (!damaged.empty()) {
            throw rfx::Error("damaged: " + damaged);
        }
        std::fprintf(stderr, "all intact: %zu track(s) match their checksum%s\n", intact,
                     unchecked ? (", " + std::to_string(unchecked) + " with no checksum kept").c_str() : "");
        return;
    }
    if (job.pass0Only) {
        if (job.archive.isEmpty()) {
            throw rfx::Error("nothing to work on: no .rfx file is open");
        }
        // (a track asked for by itself is worked out again; for the whole file, the tracks that have it are left as they are)
        rfx::keepPass0(job.archive, job.options.rateMhz, !job.only, nullptr, job.only, job.options.assumeLaserdisc);
        return;
    }
    if (job.unpack) {
        if (job.archive.isEmpty()) {
            throw rfx::Error("nothing to unpack: no .rfx file is open");
        }
        if (rfx::isFlacArchive(job.archive)) {
            rfx::unpackFlac(job.archive, job.output, job.only, std::nullopt);
        } else {
            rfx::UnpackOptions unpack;
            unpack.only = job.only;
            unpack.device = job.options.device;
            unpack.threads = job.options.threads;
            rfx::unpackArchive(job.archive, job.output, unpack);
        }
        return;
    }
    if (job.picks.empty()) {
        throw rfx::Error("nothing to write: the list has no track");
    }
    const std::vector<QString> allArchives = jobArchives(job);
    if (job.codec == Codec::Rfx && !allArchives.empty() && rfx::isFlacArchive(allArchives.front())) {
        // From a FLAC or raw file to the predictor: the tracks come out as captures, which are coded as new ones (the lines are located again).
        const QString work = rfx::withExtension(job.output, QStringLiteral("work"));
        QDir(work).removeRecursively();
        const std::vector<QString> files = listAsFiles(job, work);
        std::vector<rfx::Pick> picks;
        for (size_t k = 0; k < job.picks.size(); ++k) {
            rfx::Pick p = rfx::Pick::fresh(files[k]);
            p.edit.rateHz = job.picks[k].edit.rateHz;
            p.edit.notes = job.picks[k].edit.notes;
            p.edit.bits = job.picks[k].edit.bits;
            picks.push_back(std::move(p));
        }
        rfx::assembleArchives({}, picks, job.output, job.options, std::nullopt);
        QDir(work).removeRecursively();
        if (job.options.keepPass0) {
            rfx::keepPass0(job.output, job.options.rateMhz, true, nullptr, std::nullopt, job.options.assumeLaserdisc);
        }
        return;
    }
    if (job.codec == Codec::Rfx) {
        const std::vector<QString> &archives = allArchives;
        rfx::assembleArchives(archives, job.picks, job.output, job.options, std::nullopt);
        if (job.options.keepPass0) {
            rfx::keepPass0(job.output, job.options.rateMhz, true, nullptr, std::nullopt, job.options.assumeLaserdisc);
        }
        return;
    }
    // The FLAC or the raw variant: the tracks are unpacked and written again, with the name, rate, note, files and bits the list says.
    rfx::assembleStored(allArchives, job.picks, job.output, job.options, job.codec == Codec::Raw ? rfx::StoredCodec::Raw : rfx::StoredCodec::Flac);
}

void removeJobLeftovers(const QString &output)
{
    if (output.isEmpty()) {
        return;   // (a job that writes no file leaves nothing: and an empty name would be the current folder)
    }
    // The names are those of the library (the output with its last extension replaced).
    for (const char *extension : {"rfx-tmp"}) {
        QFile::remove(rfx::withExtension(output, QString::fromLatin1(extension)));
    }
    for (const char *extension : {"edit", "work"}) {
        QDir(rfx::withExtension(output, QString::fromLatin1(extension))).removeRecursively();
    }
}
