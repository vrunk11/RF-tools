/******************************************************************************
 * packer_tests.cpp
 * rf-packer - the list of tracks, the job, and the window, driven on an offscreen screen
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "archive.h"
#include "edit.h"
#include "rfsync.h"
#include "document.h"
#include "flacmode.h"
#include "job.h"
#include "verify.h"
#include "mainwindow.h"
#include "packer.h"
#include "processdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <cmath>
#include <cstring>
#include <string>

namespace {

constexpr size_t kSamples = 400000;

QByteArray capture(int lag, uint32_t seed)
{
    QByteArray out(static_cast<qsizetype>(kSamples * 2), 0);
    uint32_t x = seed;
    for (size_t i = 0; i < kSamples; ++i) {
        x = x * 1664525u + 1013904223u;
        const double t = static_cast<double>(static_cast<int64_t>(i) + lag);
        const double v = 180 * std::sin(t * 0.21) + 120 * std::sin(t * 0.0137 + 1) + 60 * std::sin(t * 0.77);
        const int tenBit = static_cast<int>(std::lround(v)) + static_cast<int>(x >> 29) - 3;
        const int16_t s = static_cast<int16_t>(tenBit * 64);
        std::memcpy(out.data() + 2 * i, &s, 2);
    }
    return out;
}

QByteArray pass0Dump(double first)
{
    std::string text;
    for (size_t f = 0; f < 15; ++f) {
        for (int half = 0; half < 2; ++half) {
            text += half == 0 ? "true " : "false ";
            text += "Some([" + std::to_string(f + 1000) + ", 0])";
            const double start = first + (static_cast<double>(f) * 2 + half) * 20000.0;
            for (size_t i = 0; i < 20; ++i) {
                const double pos = start + static_cast<double>(i) * 1000.0;
                uint64_t bits;
                std::memcpy(&bits, &pos, sizeof(bits));
                char buf[24];
                std::snprintf(buf, sizeof(buf), " %llx", static_cast<unsigned long long>(bits));
                text += buf;
            }
            text += "\n";
        }
    }
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

void write(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "cannot write a test file");
    f.write(data);
}

rfx::PackOptions options()
{
    rfx::PackOptions o;
    o.threads = 2;
    o.order = rfx::OrderMode::Given;
    o.segment = 65536;
    return o;
}

} // namespace

class PackerTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void statesFollowTheEdits();
    void jobSurvivesItsDocument();
    void notesCanBeEditedAndDeleted();
    void writesACopyAndOverwrites();
    void createsAnArchiveFromCaptures();
    void codecsAndTheFormatOfTheList();
    void unpacksAllAndOneTrack();
    void computesThePass0Curve();
    void keepsThePass0OfWhatItPacks();
    void mergesFilesWithoutCodingAgain();
    void saysWhatTheDiscIs();
    void checksTheTracksAgainstTheirChecksums();
    void panelOpensAndCloses();

private:
    QTemporaryDir m_dir;
    QString m_archive;
};

void PackerTests::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("RF-tools-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("rf-packer-tests"));
    QSettings().clear();
    ProgressDialog::setWorkerProgram(QStringLiteral(PACKER_PROGRAM));
    QVERIFY(m_dir.isValid());
    write(m_dir.filePath("a.ddd.s16"), capture(0, 1));
    write(m_dir.filePath("b.ddd.s16"), capture(5, 2));
    write(m_dir.filePath("c.ddd.s16"), capture(9, 3));
    QDir().mkpath(m_dir.filePath("p0"));
    write(m_dir.filePath("p0/pass0_0.txt"), pass0Dump(500));
    write(m_dir.filePath("p0/pass0_1.txt"), pass0Dump(495));
    rfx::PackOptions pack = options();
    pack.pass0Dir = m_dir.filePath("p0");
    m_archive = m_dir.filePath("two.rfx");
    rfx::packCaptures({m_dir.filePath("a.ddd.s16"), m_dir.filePath("b.ddd.s16")}, m_archive, pack);
}

void PackerTests::statesFollowTheEdits()
{
    Document doc;
    QString error;
    QVERIFY(doc.open(m_archive, &error));
    QCOMPARE(doc.count(), 2);
    QVERIFY(!doc.isChanged());
    QCOMPARE(doc.state(0), EntryState::Kept);
    QCOMPARE(doc.state(1), EntryState::Kept);
    QVERIFY(!doc.at(0).meta.fileTime->empty());

    // A note changes the metadata only: the track is edited, not coded again.
    doc.edit(0).notes = QStringLiteral("Pioneer, side A");
    doc.touched();
    QCOMPARE(doc.state(0), EntryState::Edited);
    QCOMPARE(doc.state(1), EntryState::Kept);
    QVERIFY(doc.isChanged());
    doc.edit(0).notes.clear();
    QCOMPARE(doc.state(0), EntryState::Kept);

    // The second track is predicted from the first: without it, or before it, it has to be coded again.
    doc.move(0, 1);
    QCOMPARE(doc.state(0), EntryState::Recode);
    doc.move(0, 1);
    QCOMPARE(doc.state(1), EntryState::Kept);
    doc.remove(0);
    QCOMPARE(doc.state(0), EntryState::Removed);
    QCOMPARE(doc.state(1), EntryState::Recode);
    doc.restore(0);

    QVERIFY(doc.addCapture(m_dir.filePath("c.ddd.s16"), &error));
    QCOMPARE(doc.count(), 3);
    QCOMPARE(doc.state(2), EntryState::New);
    QVERIFY(doc.at(2).meta.fileTime.has_value());
    QVERIFY(!doc.addCapture(m_dir.filePath("missing.s16"), &error));
    QVERIFY(!error.isEmpty());
}

void PackerTests::jobSurvivesItsDocument()
{
    Document doc;
    QString error;
    QVERIFY(doc.open(m_archive, &error));
    doc.edit(0).notes = QStringLiteral("a\nb");
    doc.edit(1).name = QStringLiteral("second.s16");
    doc.edit(1).sidecars.push_back(rfx::Sidecar{"x.yaml", "00", {1, 2, 3}});
    doc.touched();
    rfx::PackOptions o = options();
    o.pass0Dir = m_dir.filePath("p0");
    const Job job = doc.toJob(m_dir.filePath("out.rfx"), o);
    const Job back = jobFromJson(jobToJson(job));
    QCOMPARE(back.output, job.output);
    QCOMPARE(back.archive, m_archive);
    QCOMPARE(back.picks.size(), size_t(2));
    QCOMPARE(QString::fromStdString(*back.picks[0].edit.notes), QStringLiteral("a\nb"));
    QVERIFY(!back.picks[0].edit.name);
    QCOMPARE(QString::fromStdString(*back.picks[1].edit.name), QStringLiteral("second.s16"));
    QVERIFY(!back.picks[1].edit.rateHz);
    QVERIFY(back.codec == Codec::Rfx);
    QVERIFY(back.picks[1].edit.sidecars && back.picks[1].edit.sidecars->size() == 1);
    QCOMPARE((*back.picks[1].edit.sidecars)[0].data, (std::vector<uint8_t>{1, 2, 3}));
    QVERIFY(back.options.pass0Dir.has_value());
    QVERIFY(back.options.order == rfx::OrderMode::Given);
    QVERIFY_THROWS_EXCEPTION(std::exception, jobFromJson("not json"));
}

void PackerTests::notesCanBeEditedAndDeleted()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    QVERIFY(w.openArchive(m_archive));
    QCoreApplication::processEvents();
    auto *notes = w.findChild<QPlainTextEdit *>("infoNotes");
    auto *erase = w.findChild<QPushButton *>("deleteNotes");
    auto *process = w.findChild<QPushButton *>("processButton");
    QVERIFY(notes && erase && process);
    QCOMPARE(process->text(), QStringLiteral("Process…"));
    QVERIFY(!process->isEnabled());
    QCOMPARE(w.picked(), 0);

    notes->setPlainText(QStringLiteral("Disc 1, side A\nclean"));
    QCOMPARE(w.document()->at(0).notes, QStringLiteral("Disc 1, side A\nclean"));
    QCOMPARE(w.document()->state(0), EntryState::Edited);
    QTest::qWait(30);
    QVERIFY(process->isEnabled());

    // Another track, then back: the note is still there; deleting it empties it.
    w.pick(1);
    QVERIFY(notes->toPlainText().isEmpty());
    w.pick(0);
    QCOMPARE(notes->toPlainText(), QStringLiteral("Disc 1, side A\nclean"));
    QTest::mouseClick(erase, Qt::LeftButton);
    QVERIFY(notes->toPlainText().isEmpty());
    QVERIFY(w.document()->at(0).notes.isEmpty());
    QCOMPARE(w.document()->state(0), EntryState::Kept);

    // The name in the panel renames the track.
    auto *name = w.findChild<QLineEdit *>("infoName");
    QVERIFY(name);
    name->selectAll();
    QTest::keyClicks(name, QStringLiteral("renamed.s16"));
    QCOMPARE(w.document()->at(0).name, QStringLiteral("renamed.s16"));
    // (a window with changes asks before closing; the test discards them)
    w.document()->open(m_archive, nullptr);
}

void PackerTests::writesACopyAndOverwrites()
{
    const QString copy = m_dir.filePath("copy.rfx");
    {
        MainWindow w;
        QVERIFY(w.openArchive(m_archive));
        w.document()->edit(0).notes = QStringLiteral("kept in the copy");
        w.document()->edit(1).name = QStringLiteral("second.s16");
        w.document()->touched();
        rfx::PackOptions o = options();
        QVERIFY(w.process(copy, &o));
        QCOMPARE(w.document()->archive(), copy);
        QVERIFY(!w.document()->isChanged());
        QCOMPARE(w.document()->at(0).notes, QStringLiteral("kept in the copy"));
    }
    {
        rfx::Archive written(copy);
    QCOMPARE(written.meta().sources.size(), size_t(2));
    QCOMPARE(QString::fromStdString(written.meta().sources[0].notes), QStringLiteral("kept in the copy"));
    QCOMPARE(QString::fromStdString(written.meta().sources[1].name), QStringLiteral("second.s16"));
    // The archive it came from is as it was.
    QVERIFY(rfx::Archive(m_archive).meta().sources[0].notes.empty());
    }
    QVERIFY(!QFile::exists(m_dir.filePath("copy.rfx-tmp")));

    // Overwriting: the note is deleted again, in the file itself.
    {
        MainWindow w;
        QVERIFY(w.openArchive(copy));
        w.document()->edit(0).notes.clear();
        w.document()->touched();
        rfx::PackOptions o = options();
        QVERIFY(w.process(copy, &o));
    }
    QVERIFY(rfx::Archive(copy).meta().sources[0].notes.empty());
    QCOMPARE(QString::fromStdString(rfx::Archive(copy).meta().sources[1].name), QStringLiteral("second.s16"));
}

void PackerTests::createsAnArchiveFromCaptures()
{
    const QString made = m_dir.filePath("made.rfx");
    {
        MainWindow w;
        QVERIFY(w.document()->archive().isEmpty());
        auto *process = w.findChild<QPushButton *>("processButton");
        QVERIFY(process);
        QCOMPARE(process->text(), QStringLiteral("Create RFX…"));
        QVERIFY(w.addCaptures({m_dir.filePath("c.ddd.s16")}));
        QCOMPARE(w.document()->count(), 1);
        QCOMPARE(w.picked(), 0);
        w.document()->edit(0).notes = QStringLiteral("only one");
        w.document()->touched();
        rfx::PackOptions o = options();
        QVERIFY(w.process(made, &o));
        QCOMPARE(w.document()->archive(), made);
    }
    rfx::Archive archive(made);
    QCOMPARE(archive.meta().sources.size(), size_t(1));
    QCOMPARE(QString::fromStdString(archive.meta().sources[0].notes), QStringLiteral("only one"));
    QVERIFY(archive.meta().sources[0].fileTime.has_value());
}

void PackerTests::unpacksAllAndOneTrack()
{
    MainWindow w;
    QVERIFY(!w.unpack(std::nullopt, m_dir.filePath("none")));
    QVERIFY(w.openArchive(m_archive));
    const QString all = m_dir.filePath("unpacked-all");
    QVERIFY(w.unpack(std::nullopt, all));
    QVERIFY(QFile::exists(all + "/a.ddd.s16"));
    QVERIFY(QFile::exists(all + "/b.ddd.s16"));
    QCOMPARE(QFileInfo(all + "/a.ddd.s16").size(), QFileInfo(m_dir.filePath("a.ddd.s16")).size());
    const QString one = m_dir.filePath("unpacked-one");
    QVERIFY(w.unpack(size_t(1), one));
    QVERIFY(QFile::exists(one + "/b.ddd.s16"));
    QVERIFY(!QFile::exists(one + "/a.ddd.s16"));
    QVERIFY(w.findChild<QToolButton *>("rowUnpack"));
}

void PackerTests::computesThePass0Curve()
{
    // What the job says is kept through its file, and the work on an opened file leaves it as it was (a capture with no video in it has no
    // lines to keep: the file is added to all the same, and still opens).
    Job job;
    job.archive = QStringLiteral("a.rfx");
    job.pass0Only = true;
    job.options.keepPass0 = true;
    const Job back = jobFromJson(jobToJson(job));
    QVERIFY(back.pass0Only && back.options.keepPass0);
    QVERIFY(!jobFromJson(jobToJson(Job())).pass0Only);

    const QString copy = m_dir.filePath("p0-copy.rfx");
    QVERIFY(QFile::copy(m_archive, copy));
    MainWindow w;
    QVERIFY(!w.computePass0(false));
    QVERIFY(w.openArchive(copy));
    QVERIFY(w.computePass0(false));
    Document doc;
    QString error;
    QVERIFY2(doc.open(copy, &error), qPrintable(error));
    QCOMPARE(doc.count(), 2);
    QVERIFY(QFileInfo(copy).size() >= QFileInfo(m_archive).size());
    QVERIFY(w.findChild<QAction *>("computePass0"));
}

void PackerTests::saysWhatTheDiscIs()
{
    // The information panel says the standard, the reference, the copy, the face and the signal; they are written to the file (a track copied
    // as it is keeps its blocks: only its description changes), and read back.
    MainWindow w;
    QVERIFY(w.openArchive(m_archive));
    w.pick(1);
    auto *standard = w.findChild<QComboBox *>("infoStandard");
    auto *reference = w.findChild<QLineEdit *>("infoReference");
    auto *copy = w.findChild<QSpinBox *>("infoCopy");
    auto *captureNumber = w.findChild<QSpinBox *>("infoCapture");
    auto *face = w.findChild<QComboBox *>("infoFace");
    auto *cvbs = w.findChild<QPushButton *>("infoCvbs");
    auto *yc = w.findChild<QPushButton *>("infoYc");
    QVERIFY(standard && reference && copy && captureNumber && face && cvbs && yc);
    QCOMPARE(standard->count(), 6);   // (not said, then PAL, NTSC, PAL-M, SECAM, NTSC-VHD)
    QCOMPARE(face->count(), 5);       // (not said, then A, B, AB, BA)
    QVERIFY(standard->isEnabled() && !w.document()->isChanged());
    standard->setCurrentIndex(standard->findData(QStringLiteral("pal-m")));
    reference->setText(QStringLiteral("LD-1234"));
    QTest::keyClick(reference, Qt::Key_X);   // (typed: the text is taken from the box)
    reference->setText(QStringLiteral("LD-1234"));
    copy->setValue(2);
    captureNumber->setValue(5);
    face->setCurrentIndex(face->findData(QStringLiteral("BA")));
    yc->click();
    QVERIFY(yc->isChecked() && !cvbs->isChecked());
    cvbs->click();
    QVERIFY(cvbs->isChecked() && !yc->isChecked());
    cvbs->click();   // (again: nothing said)
    QVERIFY(!cvbs->isChecked() && !yc->isChecked());
    yc->click();
    const Entry &e = w.document()->at(1);
    QCOMPARE(e.standard, QStringLiteral("pal-m"));
    QCOMPARE(e.copy, 2);
    QCOMPARE(e.captureNumber, 5);
    QCOMPARE(e.face, QStringLiteral("BA"));
    QCOMPARE(e.signal, QStringLiteral("yc"));
    QVERIFY(w.document()->state(1) == EntryState::Edited);
    QVERIFY(w.document()->state(0) == EntryState::Kept);
    const Job job = w.document()->toJob(m_dir.filePath("said.rfx"), options());
    QVERIFY(job.picks[1].edit.info.standard && *job.picks[1].edit.info.standard == "pal-m");
    QVERIFY(!job.picks[0].edit.info.any());
    const Job back = jobFromJson(jobToJson(job));
    QVERIFY(back.picks[1].edit.info.copy && *back.picks[1].edit.info.copy == 2);
    QVERIFY(w.process(m_dir.filePath("said.rfx"), nullptr));
    const rfx::Archive archive(m_dir.filePath("said.rfx"));
    const rfx::SourceMeta &said = archive.meta().sources[1];
    QCOMPARE(QString::fromStdString(said.standard), QStringLiteral("pal-m"));
    QCOMPARE(said.copy, 2u);
    QCOMPARE(said.capture, 5u);   // (the copy of the disc and the number of the capture are two things)
    QCOMPARE(QString::fromStdString(said.face), QStringLiteral("BA"));
    QCOMPARE(QString::fromStdString(said.signal), QStringLiteral("yc"));
    QVERIFY(!archive.meta().sources[0].hasInfo());
    // (what the window was changed to is what it shows now)
    QCOMPARE(w.findChild<QComboBox *>("infoFace")->currentData().toString(), QStringLiteral("BA"));
    // The values are the library's: parsed from what a person types, whatever the case.
    std::string value;
    QVERIFY(rfx::parseStandard("NTSC-VHD", value) && value == "ntsc-vhd");
    QVERIFY(rfx::parseFace("ab", value) && value == "AB");
    QVERIFY(rfx::parseSignal("Y/C", value) && value == "yc");
    QVERIFY(!rfx::parseStandard("pal-n", value) && !rfx::parseFace("C", value) && !rfx::parseSignal("rgb", value));
}
void PackerTests::mergesFilesWithoutCodingAgain()
{
    const auto size = [](const QString &path) { return QFileInfo(path).size(); };
    // Files of the predictor, a track each: the list takes the tracks of the second file, and what is written has the blocks of both as they were.
    const QString one = m_dir.filePath("one.rfx");
    const QString two = m_dir.filePath("two-alone.rfx");
    rfx::packCaptures({m_dir.filePath("a.ddd.s16")}, one, options());
    rfx::packCaptures({m_dir.filePath("c.ddd.s16")}, two, options());
    {
        MainWindow w;
        QVERIFY(w.openArchive(one));
        QCOMPARE(w.document()->archives().size(), size_t(1));
        QVERIFY(w.addCaptures({two}));
        QCOMPARE(w.document()->count(), 2);
        QCOMPARE(w.document()->archives().size(), size_t(2));
        QCOMPARE(w.document()->at(1).arch, size_t(1));
        // (both are as they were: copied)
        QVERIFY(w.document()->state(0) == EntryState::Kept && w.document()->state(1) == EntryState::Kept);
        QVERIFY(w.document()->isChanged());
        // A file twice, or a file of the other variant, is refused.
        QString error;
        QVERIFY(!w.document()->addArchive(two, &error));
        QVERIFY(!error.isEmpty());
        // The job names the two files; written to a new file, the blocks are copied (no coding: the sizes add up).
        const Job job = w.document()->toJob(m_dir.filePath("merged.rfx"), options());
        QCOMPARE(job.extraArchives.size(), 1);
        QCOMPARE(job.picks[1].arch, size_t(1));
        QVERIFY(jobFromJson(jobToJson(job)).picks[1].arch == 1);
        QVERIFY(w.process(m_dir.filePath("merged.rfx"), nullptr));
        QVERIFY(std::abs(size(m_dir.filePath("merged.rfx")) - (size(one) + size(two))) < 2000);
        const std::vector<rfx::TrackCheck> checks = rfx::verifyArchive(m_dir.filePath("merged.rfx"));
        QCOMPARE(checks.size(), size_t(2));
        QVERIFY(checks[0].ok && checks[1].ok);
    }
    // The same for the FLAC and the raw variants: the streams are copied (ffmpeg -c copy), nothing is decoded or encoded.
    for (const rfx::StoredCodec codec : {rfx::StoredCodec::Flac, rfx::StoredCodec::Raw}) {
        const bool raw = codec == rfx::StoredCodec::Raw;
        const QString a = m_dir.filePath(raw ? "raw-a.rfx" : "flac-a.rfx");
        const QString b = m_dir.filePath(raw ? "raw-b.rfx" : "flac-b.rfx");
        try {
            rfx::PackOptions o = options();
            o.rateMhz = 40.0;
            rfx::packFlac({m_dir.filePath("a.ddd.s16")}, a, o, codec);
            rfx::packFlac({m_dir.filePath("c.ddd.s16")}, b, o, codec);
        } catch (const std::exception &) {
            QSKIP("ffmpeg is not available");
        }
        MainWindow w;
        QVERIFY(w.openArchive(a));
        QVERIFY(w.addCaptures({b}));
        QCOMPARE(w.document()->count(), 2);
        QVERIFY(w.document()->codec() == (raw ? Codec::Raw : Codec::Flac));
        QString error;
        QVERIFY(!w.document()->addArchive(one, &error));   // (a file of the predictor with files of the other variant)
        const QString merged = m_dir.filePath(raw ? "raw-merged.rfx" : "flac-merged.rfx");
        QVERIFY(w.process(merged, nullptr));
        QVERIFY(rfx::isFlacArchive(merged));
        QCOMPARE(rfx::readFlacMeta(merged).size(), size_t(2));
        QVERIFY(std::abs(size(merged) - (size(a) + size(b))) < 4000);
        const std::vector<rfx::TrackCheck> checks = rfx::verifyArchive(merged);
        QCOMPARE(checks.size(), size_t(2));
        QVERIFY2(checks[0].ok && checks[1].ok, "the samples differ after a stream copy");
    }
}

void PackerTests::keepsThePass0OfWhatItPacks()
{
    // Packing with keepPass0: the lines located for the lag tables are kept as they are, for every track.
    rfx::PackOptions pack = options();
    pack.pass0Dir = m_dir.filePath("p0");
    pack.order = rfx::OrderMode::Given;
    pack.keepPass0 = true;
    const QString kept = m_dir.filePath("kept.rfx");
    rfx::packCaptures({m_dir.filePath("a.ddd.s16"), m_dir.filePath("b.ddd.s16")}, kept, pack);
    {
        const rfx::Archive a(kept);
        QCOMPARE(a.pass0().size(), size_t(2));
        // (to the bit: the dump written back from the file is the one that was read)
        QCOMPARE(rfx::formatPass0(a.pass0().at(0).fields), rfx::formatPass0(rfx::parsePass0(m_dir.filePath("p0/pass0_0.txt"))));
        QCOMPARE(rfx::formatPass0(a.pass0().at(1).fields), rfx::formatPass0(rfx::parsePass0(m_dir.filePath("p0/pass0_1.txt"))));
    }
    // Without it, none (the archive of the tests has none).
    {
        const rfx::Archive a(m_archive);
        QVERIFY(a.pass0().empty());
    }
    // A track copied into another file takes its lines with it, whether or not they are asked for again.
    const QString first = m_dir.filePath("first-only.rfx");
    rfx::assembleArchives({kept}, rfx::pickSome(kept, {0}), first, options(), std::nullopt);
    {
        const rfx::Archive a(first);
        QCOMPARE(a.meta().sources.size(), size_t(1));
        QCOMPARE(a.pass0().size(), size_t(1));
    }
    // The window: what the information says of the track, and the button of the track (the work is of that track alone).
    {
        MainWindow w;
        QVERIFY(w.openArchive(kept));
        w.pick(1);
        auto *facts = w.findChild<QLabel *>("infoFacts");
        QVERIFY(facts && facts->text().contains(QStringLiteral("TBC curve")) && facts->text().contains(QStringLiteral("kept (")));
        auto *button = w.findChild<QPushButton *>("pass0Track");
        QVERIFY(button && button->isEnabled());
        QVERIFY(w.computePass0(false, size_t(1)));
        QVERIFY(w.openArchive(m_archive));
        w.pick(0);
        QVERIFY(w.findChild<QLabel *>("infoFacts")->text().contains(QStringLiteral("not kept")));
    }
    // The lines of the tracks of a file as it is: located and kept, then left as they are.
    QCOMPARE(rfx::keepPass0(first, 40.0, true), size_t(0));
}

void PackerTests::checksTheTracksAgainstTheirChecksums()
{
    // The library: every track decodes to what was packed.
    const std::vector<rfx::TrackCheck> checks = rfx::verifyArchive(m_archive);
    QCOMPARE(checks.size(), size_t(2));
    for (const rfx::TrackCheck &c : checks) {
        QVERIFY2(c.ok && c.checked, c.detail.c_str());
    }
    const std::vector<rfx::TrackCheck> one = rfx::verifyArchive(m_archive, size_t(1));
    QCOMPARE(one.size(), size_t(1));
    QCOMPARE(one[0].track, size_t(1));
    QVERIFY(one[0].ok);
    bool refused = false;
    try {
        rfx::verifyArchive(m_archive, size_t(7));
    } catch (const std::exception &) {
        refused = true;
    }
    QVERIFY(refused);

    // A byte changed in the coded samples: said, whatever the track it is in.
    const QString damaged = m_dir.filePath("damaged.rfx");
    QVERIFY(QFile::copy(m_archive, damaged));
    {
        QFile f(damaged);
        QVERIFY(f.open(QIODevice::ReadWrite));
        const qint64 at = f.size() - 200;
        f.seek(at);
        char byte = 0;
        QVERIFY(f.read(&byte, 1) == 1);
        byte = static_cast<char>(byte ^ 0x5A);
        f.seek(at);
        QVERIFY(f.write(&byte, 1) == 1);
    }
    const std::vector<rfx::TrackCheck> bad = rfx::verifyArchive(damaged);
    QVERIFY(std::any_of(bad.begin(), bad.end(), [](const rfx::TrackCheck &c) { return !c.ok; }));

    // The window of progress is the bar and what it says, with no empty log under it until the work has something to say.
    {
        Job job;
        job.archive = m_archive;
        job.verify = true;
        ProgressDialog progress(job);
        auto *log = progress.findChild<QPlainTextEdit *>();
        QVERIFY(log);
        QVERIFY(log->isHidden());
    }

    // The window: the buttons are there once a file is open, and the work runs in the program again, so that it can be stopped.
    MainWindow w;
    auto *all = w.findChild<QPushButton *>("verifyAll");
    auto *track = w.findChild<QPushButton *>("verifyTrack");
    QVERIFY(all && track);
    QVERIFY(!w.verify(std::nullopt, true));
    QVERIFY(w.openArchive(m_archive));
    w.pick(1);
    QVERIFY(all->isEnabled() && track->isEnabled());
    QVERIFY(w.verify(std::nullopt, true));
    QVERIFY(w.verify(size_t(1), true));
    QVERIFY(w.findChild<QAction *>("verifyFile"));
    QVERIFY(w.openArchive(damaged));
    QVERIFY(!w.verify(std::nullopt, true));
}

void PackerTests::codecsAndTheFormatOfTheList()
{
    {
        // The format is said once for a new list, and cannot be changed in an opened file.
        Document doc;
        QVERIFY(doc.formatEditable());
        doc.setFormat(40e6, 10);
        QString error;
        QVERIFY(doc.addCapture(m_dir.filePath("c.ddd.s16"), &error));
        const Job job = doc.toJob(m_dir.filePath("f.rfx"), options());
        QCOMPARE(*job.picks[0].edit.rateHz, 40e6);
        QCOMPARE(*job.picks[0].edit.bits, 10u);
        doc.setCodec(Codec::Flac);
        QVERIFY(jobFromJson(jobToJson(doc.toJob("x.rfx", options()))).codec == Codec::Flac);
        QVERIFY(doc.open(m_archive, &error));
        QVERIFY(!doc.formatEditable());
        QVERIFY(doc.codec() == Codec::Rfx);
        QVERIFY(!doc.isChanged());
        doc.setCodec(Codec::Raw);
        QVERIFY(doc.isChanged());
    }
    {
        // Raw: the samples as they are, in a Matroska file (the tracks of the archive are decoded first, a new one is added; ffmpeg).
        bool ffmpeg = true;
        MainWindow w;
        QVERIFY(w.openArchive(m_archive));
        QVERIFY(w.addCaptures({m_dir.filePath("c.ddd.s16")}));
        auto *codec = w.findChild<QComboBox *>("codecBox");
        QVERIFY(codec);
        codec->setCurrentIndex(codec->findData(static_cast<int>(Codec::Raw)));
        QVERIFY(w.document()->codec() == Codec::Raw);
        QTest::qWait(20);
        auto *rate = w.findChild<QDoubleSpinBox *>("formatRate");
        QVERIFY(rate && !rate->isEnabled());
        rfx::PackOptions o = options();
        o.rateMhz = 75.0;
        ffmpeg = w.process(m_dir.filePath("raw.rfx"), &o);
        if (ffmpeg) {
            std::string stored;
            const std::vector<rfx::SourceMeta> tracks = rfx::readFlacMeta(m_dir.filePath("raw.rfx"), &stored);
            QCOMPARE(QString::fromStdString(stored), QStringLiteral("raw"));
            QCOMPARE(tracks.size(), size_t(3));
            QCOMPARE(QString::fromStdString(tracks[2].name), QStringLiteral("c.ddd.s16"));
            QVERIFY(tracks[0].fileTime.has_value());
            // The file is about as large as the captures: nothing is coded.
            QVERIFY(QFileInfo(m_dir.filePath("raw.rfx")).size() > static_cast<qint64>(3 * kSamples * 2));
        }
        if (ffmpeg) {
            // The raw file opens again, its tracks are listed with its codec, and one of them goes to the predictor.
            MainWindow again;
            QVERIFY(again.openArchive(m_dir.filePath("raw.rfx")));
            QCOMPARE(again.document()->count(), 3);
            QVERIFY(again.document()->codec() == Codec::Raw);
            again.document()->remove(1);
            again.document()->remove(2);
            again.document()->setCodec(Codec::Rfx);
            rfx::PackOptions back = options();
            QVERIFY(again.process(m_dir.filePath("back.rfx"), &back));
            rfx::Archive archive(m_dir.filePath("back.rfx"));
            QCOMPARE(archive.meta().sources.size(), size_t(1));
            QCOMPARE(QString::fromStdString(archive.meta().sources[0].name), QStringLiteral("a.ddd.s16"));
            QCOMPARE(archive.meta().sources[0].words, static_cast<uint64_t>(kSamples));
        }
    }
    {
        // FLAC: an ordinary audio file (it needs ffmpeg).
        MainWindow w;
        QVERIFY(w.addCaptures({m_dir.filePath("c.ddd.s16")}));
        w.document()->setCodec(Codec::Flac);
        rfx::PackOptions o = options();
        o.rateMhz = 40.0;
        const bool written = w.process(m_dir.filePath("flac.rfx"), &o);
        if (written) {
            QVERIFY(rfx::isFlacArchive(m_dir.filePath("flac.rfx")));
        } else {
            QSKIP("ffmpeg is not available");
        }
    }
}

void PackerTests::panelOpensAndCloses()
{
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto *info = w.findChild<QWidget *>("infoPanel");
    QVERIFY(info);
    QVERIFY(info->isVisible());
    const int open = w.width();
    auto *button = w.findChild<QPushButton *>("");
    Q_UNUSED(button);
    const auto buttons = w.findChildren<QPushButton *>();
    QPushButton *toggle = nullptr;
    for (QPushButton *b : buttons) {
        if (b->isCheckable() && b->text() == QLatin1String("Info")) {
            toggle = b;
        }
    }
    QVERIFY(toggle);
    toggle->click();
    QVERIFY(!info->isVisible());
    QVERIFY(w.width() < open);
    toggle->click();
    QVERIFY(info->isVisible());
    QCOMPARE(w.width(), open);
}

QTEST_MAIN(PackerTests)
#include "packer_tests.moc"
