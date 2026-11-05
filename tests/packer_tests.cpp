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
#include "document.h"
#include "flacmode.h"
#include "job.h"
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
        if (b->isCheckable()) {
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
