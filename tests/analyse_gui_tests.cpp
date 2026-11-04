/******************************************************************************
 * analyse_gui_tests.cpp
 * rf-analyse - the session, the views and the window, driven with the mouse and
 * the wheel on an offscreen screen
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "appearancemenus.h"
#include "capturedialog.h"
#include "deviationview.h"
#include "ffmpeg.h"
#include "mainwindow.h"
#include "metadatapanel.h"
#include "packer.h"
#include "session.h"
#include "spectrumview.h"
#include "statspanel.h"
#include "theme.h"
#include "tracklist.h"
#include "waveformview.h"

#include <QApplication>
#include <QTimer>
#include <QProcess>
#include <QIcon>
#include <QMenu>
#include <QMimeData>
#include <QDropEvent>
#include <QUrl>
#include <QMenuBar>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QStandardItemModel>
#include <QSignalSpy>
#include <QSlider>
#include <QTableWidget>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <vector>
#include <cstring>
#include <string>

namespace {

constexpr size_t kSamples = 600000;

// Tones and noise as 16-bit samples shifted left by 6 bits, `lag` samples into the
// signal.
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

// Wait until the worker has caught up with the state of the session.
bool settle(Session &session)
{
    QElapsedTimer t;
    t.start();
    while (!session.upToDate() && t.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(5);
    }
    QCoreApplication::processEvents();
    return session.upToDate();
}

void wheel(QWidget *w, int delta, Qt::KeyboardModifiers modifiers)
{
    const QPointF at(w->width() / 2.0, w->height() / 2.0);
    QWheelEvent e(at, w->mapToGlobal(at), QPoint(), QPoint(0, delta), Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

} // namespace

class AnalyseGuiTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void sessionOpens();
    void sessionNavigation();
    void sessionResults();
    void windowFollowsTheMouse();
    void navigationAndTracks();
    void viewsHaveWhatToShow();
    void refusesWhatIsNotAnArchive();
    void opensARawCapture();
    void asksWhatACaptureDoesNotSay();
    void opensSeveralFiles();
    void tracksOfDifferentLengths();
    void hasThemesAndIcons();
    void listOfTracksCanBePutAway();
    void blocksCanBeTakenAway();
    void graphsFollowTheSizeAndShowValues();
    void compactModeAndSpectrumControls();
    void decodesCompressedCaptures();
    void speedFromTheRf();

private:
    QTemporaryDir m_dir;
    QString m_archive;
};

void AnalyseGuiTests::initTestCase()
{
    // What the dialog remembers goes to settings of the test's own.
    QCoreApplication::setOrganizationName(QStringLiteral("RF-tools-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("rf-analyse-tests"));
    QSettings().clear();
    QVERIFY(m_dir.isValid());
    write(m_dir.filePath("a.ddd.s16"), capture(0, 1));
    write(m_dir.filePath("b.ddd.s16"), capture(5, 2));
    write(m_dir.filePath("a.ddd.yaml"), "{\n  \"sample_rate_hz\": 40000000,\n  \"started\": \"2026-10-27T07:33:55\",\n  \"finished\": \"2026-10-27T07:34:05\",\n  \"duration_seconds\": 10.793\n}\n");
    write(m_dir.filePath("b.ddd.yaml"), "{\n  \"sample_rate_hz\": 40000000,\n  \"started\": \"2026-10-27T07:33:55\",\n  \"finished\": \"2026-10-27T07:34:05\",\n  \"duration_seconds\": 10.793\n}\n");
    QDir().mkpath(m_dir.filePath("p0"));
    write(m_dir.filePath("p0/pass0_0.txt"), pass0Dump(500));
    write(m_dir.filePath("p0/pass0_1.txt"), pass0Dump(495));
    rfx::PackOptions pack;
    pack.threads = 2;
    pack.order = rfx::OrderMode::Given;
    pack.segment = 65536;
    pack.pass0Dir = m_dir.filePath("p0");
    m_archive = m_dir.filePath("two.rfx");
    rfx::packCaptures({m_dir.filePath("a.ddd.s16"), m_dir.filePath("b.ddd.s16")}, m_archive, pack);
}

void AnalyseGuiTests::sessionOpens()
{
    Session s;
    QSignalSpy opened(&s, &Session::opened);
    QString error;
    QVERIFY(s.open(m_archive, &error));
    QCOMPARE(opened.count(), 1);
    QCOMPARE(s.trackCount(), 2);
    QCOMPARE(s.totalSamples(), qint64(kSamples));
    QVERIFY(s.rateKnown());
    QCOMPARE(s.rateHz(), 40e6);
    QCOMPARE(s.displayBits(), 10);
    QVERIFY(s.visible(0) && s.visible(1));
    QCOMPARE(s.reference(0), -1);
    QCOMPARE(s.reference(1), 0);
    QVERIFY(s.lagTable(0) == nullptr);
    QVERIFY(s.lagTable(1) != nullptr);
    QVERIFY(s.color(0) != s.color(1));
    QCOMPARE(s.windowLength(), Session::kDefaultWindow);
    QVERIFY(!s.selection().range);
    QVERIFY(settle(s));
    s.close();
    QVERIFY(!s.isOpen());
}

void AnalyseGuiTests::sessionNavigation()
{
    Session s;
    QString error;
    QVERIFY(s.open(m_archive, &error));

    s.setCursor(100000);
    QCOMPARE(s.selection().center, qint64(100000));
    QCOMPARE(s.selection().length, qint64(s.fftSize()));
    // The bar outside the window does not move it, unless it is asked to follow.
    QCOMPARE(s.windowStart(), qint64(0));
    s.setCursor(100000, true);
    QVERIFY(s.windowStart() <= 100000 && 100000 <= s.windowStart() + s.windowLength());

    // A zone.
    s.setRange(120000, 130000);
    QVERIFY(s.selection().range);
    QCOMPARE(s.selection().first, qint64(120000));
    QCOMPARE(s.selection().last, qint64(130000));
    QCOMPARE(s.selection().center, qint64(125000));
    // Too short to be one: a bar.
    s.setRange(120000, 120010);
    QVERIFY(!s.selection().range);
    // The bar clears a zone.
    s.setRange(120000, 130000);
    s.setCursor(5000);
    QVERIFY(!s.selection().range);

    // The window stays inside the capture and zoom inside its limits.
    s.setWindowStart(-5000);
    QCOMPARE(s.windowStart(), qint64(0));
    s.setWindowStart(static_cast<qint64>(kSamples) * 2);
    QCOMPARE(s.windowStart(), static_cast<qint64>(kSamples) - s.windowLength());
    for (int i = 0; i < 40; ++i) {
        s.zoomBy(0.5, s.selection().center);
    }
    QCOMPARE(s.windowLength(), Session::kShortestWindow);
    for (int i = 0; i < 40; ++i) {
        s.zoomBy(2.0, s.selection().center);
    }
    QVERIFY(s.windowLength() <= Session::kLongestWindow && s.windowLength() <= static_cast<qint64>(kSamples));
    s.resetZoom();
    QCOMPARE(s.windowLength(), Session::kDefaultWindow);

    // Paging moves the window and the bar together.
    s.setWindowStart(0);
    s.setCursor(1000);
    s.pageBy(s.windowLength());
    QCOMPARE(s.windowStart(), s.windowLength());
    QCOMPARE(s.selection().center, qint64(1000) + s.windowLength());
    s.goToStart();
    QCOMPARE(s.windowStart(), qint64(0));
    QVERIFY(s.selection().center < s.windowLength());
    s.goToEnd();
    QCOMPARE(s.windowStart(), static_cast<qint64>(kSamples) - s.windowLength());
    QVERIFY(s.selection().center >= s.windowStart() && s.selection().center < static_cast<qint64>(kSamples));

    // A zoom keeps the bar where it is on the screen.
    s.setWindowStart(200000);
    s.setCursor(200000 + s.windowLength() / 4);
    const qint64 before = s.windowLength();
    s.zoomBy(0.5, s.selection().center);
    QCOMPARE(s.windowLength(), before / 2);
    const double relative = static_cast<double>(s.selection().center - s.windowStart()) / static_cast<double>(s.windowLength());
    QVERIFY(std::fabs(relative - 0.25) < 0.01);
}

void AnalyseGuiTests::sessionResults()
{
    Session s;
    QSignalSpy ready(&s, &Session::resultReady);
    QString error;
    QVERIFY(s.open(m_archive, &error));
    QVERIFY(settle(s));
    QVERIFY(ready.count() >= 1);
    const AnalysisResult &r = s.result();
    QVERIFY(r.error.isEmpty());
    QCOMPARE(int(r.wave.size()), 2);
    QCOMPARE(qint64(r.wave[0].size()), s.windowLength());
    QCOMPARE(qint64(r.wave[1].size()), s.windowLength());
    QVERIFY(!r.spectra[1].db.empty());
    // A signal well above its noise: the numbers say so.
    QVERIFY(r.stats[0].snrDb > 10.0);
    QVERIFY(r.stats[0].meanAmplitude > 10.0);
    QVERIFY(r.stats[0].noiseFloor > 0.0);

    // The waveform is the capture: the first sample of the window, in the units of
    // the converter.
    const QByteArray a = capture(0, 1);
    int16_t first = 0;
    std::memcpy(&first, a.constData() + 2 * s.windowStart(), 2);
    QVERIFY(std::fabs(r.wave[0][0] - first / 64.0) < 1e-3);

    // A track that is not shown is not worked out.
    s.setVisible(1, false);
    QVERIFY(settle(s));
    QVERIFY(s.result().wave[1].empty());
    QVERIFY(!s.result().wave[0].empty());
    s.setVisible(1, true);

    // A zone is worked out over its own length.
    s.setRange(100000, 140000);
    QVERIFY(settle(s));
    QVERIFY(s.result().range);
    QCOMPARE(s.result().analysedLength, qint64(40000));
    QCOMPARE(s.result().analysedCenter, qint64(120000));

    // The FFT size is the one asked for.
    s.setFftSize(1024);
    QVERIFY(settle(s));
    QCOMPARE(int(s.result().spectra[0].fftSize), 1024);
}

void AnalyseGuiTests::windowFollowsTheMouse()
{
    MainWindow w;
    w.resize(1100, 800);
    QVERIFY(w.openFile(m_archive));
    w.show();
    auto *session = w.findChild<Session *>();
    QVERIFY(session);
    QVERIFY(settle(*session));
    auto *wave = w.findChild<WaveformView *>("combinedWaveform");
    QVERIFY(wave && wave->isVisible());
    wave->resize(wave->width(), wave->height());

    // A click puts the bar where it is.
    const QPoint where(wave->width() / 4, wave->height() / 2);
    QTest::mouseClick(wave, Qt::LeftButton, Qt::NoModifier, where);
    const int plotWidth = wave->width() - WaveformView::kPlotLeft;
    const qint64 expected = session->windowStart() + static_cast<qint64>(static_cast<double>(where.x() - WaveformView::kPlotLeft) / plotWidth * static_cast<double>(session->windowLength()));
    QVERIFY(std::llabs(session->selection().center - expected) <= session->windowLength() / plotWidth + 2);
    QVERIFY(!session->selection().range);

    // A drag selects a zone.
    QTest::mousePress(wave, Qt::LeftButton, Qt::NoModifier, QPoint(wave->width() / 5, 20));
    QTest::mouseMove(wave, QPoint(wave->width() / 2, 25));
    QTest::mouseRelease(wave, Qt::LeftButton, Qt::NoModifier, QPoint(wave->width() / 2, 25));
    QVERIFY(session->selection().range);
    QVERIFY(session->selection().last > session->selection().first);

    // The wheel scrolls in time, and ctrl and the wheel zoom.
    const qint64 start = session->windowStart();
    wheel(wave, -120, Qt::NoModifier);
    QVERIFY(session->windowStart() > start);
    wheel(wave, 120, Qt::NoModifier);
    QCOMPARE(session->windowStart(), start);
    const qint64 length = session->windowLength();
    wheel(wave, 120, Qt::ControlModifier);
    QCOMPARE(session->windowLength(), length / 2);
    wheel(wave, -120, Qt::ControlModifier);
    QCOMPARE(session->windowLength(), length);

    // Shift and a drag pan.
    session->setWindowStart(100000);
    const qint64 pannedFrom = session->windowStart();
    QTest::mousePress(wave, Qt::LeftButton, Qt::ShiftModifier, QPoint(wave->width() / 2, 30));
    QTest::mouseMove(wave, QPoint(wave->width() / 2 - 100, 30));
    QTest::mouseRelease(wave, Qt::LeftButton, Qt::ShiftModifier, QPoint(wave->width() / 2 - 100, 30));
    QVERIFY(session->windowStart() > pannedFrom);
    QVERIFY(settle(*session));
}

void AnalyseGuiTests::navigationAndTracks()
{
    MainWindow w;
    w.resize(1100, 800);
    QVERIFY(w.openFile(m_archive));
    w.show();
    auto *session = w.findChild<Session *>();
    QVERIFY(settle(*session));

    // The navigation bar moves the bar inside the window, one to one.
    auto *slider = w.findChild<QSlider *>("navigationSlider");
    QVERIFY(slider);
    slider->setValue(250);
    QVERIFY(std::llabs(session->selection().center - (session->windowStart() + session->windowLength() / 4)) <= 1);
    QCOMPARE(session->windowStart(), qint64(0));
    session->setCursor(session->windowStart() + session->windowLength() / 2);
    QCOMPARE(slider->value(), 500);

    // A click on a track selects it and shows its metadata; a click on the selected
    // one hides them, and another shows them again.
    auto *list = w.findChild<TrackList *>("trackList");
    auto *panel = w.findChild<MetadataPanel *>("metadataPanel");
    QVERIFY(list && panel);
    QVERIFY(panel->isVisible());
    QCOMPARE(list->count(), 2);
    const QRect row1 = list->visualItemRect(list->item(1));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row1.center().x() + 30, row1.center().y()));
    QCOMPARE(session->selected(), 1);
    QVERIFY(panel->isVisible());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row1.center().x() + 30, row1.center().y()));
    QVERIFY(!panel->isVisible());
    QCOMPARE(session->selected(), 1);
    const QRect row0 = list->visualItemRect(list->item(0));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row0.center().x() + 30, row0.center().y()));
    QCOMPARE(session->selected(), 0);
    QVERIFY(panel->isVisible());

    // The check box shows or hides the track, and nothing else.
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(12, row1.center().y()));
    QVERIFY(!session->visible(1));
    QCOMPARE(session->selected(), 0);
    QVERIFY(panel->isVisible());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(12, row1.center().y()));
    QVERIFY(session->visible(1));
    QVERIFY(settle(*session));
}

void AnalyseGuiTests::viewsHaveWhatToShow()
{
    MainWindow w;
    w.resize(1100, 900);
    QVERIFY(w.openFile(m_archive));
    w.show();
    auto *session = w.findChild<Session *>();
    QVERIFY(settle(*session));

    // The statistics list the tracks shown, with numbers once they are worked out.
    auto *stats = w.findChild<StatsPanel *>("combinedStats");
    QVERIFY(stats);
    auto *table = stats->findChild<QTableWidget *>();
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 2);
    QVERIFY(table->item(0, 2)->text().endsWith("dB"));
    session->setVisible(1, false);
    QCOMPARE(table->rowCount(), 1);
    session->setVisible(1, true);

    // The speed deviation is drawn for every track of the archive shown: the reference
    // as the line of no deviation, the other as its speed against it.
    auto *deviation = w.findChild<DeviationBlock *>("combinedDeviation")->findChild<DeviationView *>();
    QVERIFY(deviation);
    QCOMPARE(deviation->curves(), (QVector<int>{0, 1}));
    session->setVisible(1, false);
    QCOMPARE(deviation->curves(), QVector<int>{0});
    session->setVisible(0, false);
    QVERIFY(deviation->curves().isEmpty());
    session->setVisible(0, true);
    session->setVisible(1, true);
    QVERIFY(session->isReference(0) && !session->isReference(1));
    QCOMPARE(session->speedAt(0, 100000), 0.0);
    QVERIFY(std::isfinite(session->speedAt(1, 100000)));

    // The metadata are those of the selected track.
    session->setSelected(1);
    auto *panel = w.findChild<MetadataPanel *>("metadataPanel");
    QVERIFY(panel && panel->topLevelItemCount() == 4);

    // The individual view, a track alone and every lane, and back.
    w.setView(QStringLiteral("individual"));
    QVERIFY(settle(*session));
    QCOMPARE(w.findChildren<SpectrumView *>().size() >= 2, true);
    w.setView(QStringLiteral("lanes"));
    QVERIFY(settle(*session));
    w.setView(QStringLiteral("combined"));
    QVERIFY(settle(*session));

    const QString png = m_dir.filePath("window.png");
    QVERIFY(w.saveScreenshot(png));
    QVERIFY(QFile::exists(png) && QFileInfo(png).size() > 1000);

    session->close();
    QVERIFY(!session->isOpen());
}

void AnalyseGuiTests::refusesWhatIsNotAnArchive()
{
    Session s;
    write(m_dir.filePath("not.rfx"), "not an archive");
    QString error;
    QVERIFY(!s.open(m_dir.filePath("not.rfx"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!s.isOpen());
    QVERIFY(!s.open(m_dir.filePath("missing.rfx"), &error));
}

void AnalyseGuiTests::opensARawCapture()
{
    // The first track of the archive, as it was captured: a raw capture with a .yaml beside.
    const QString raw = m_dir.filePath("a.ddd.s16");
    rfx::CaptureSpec spec;
    spec.rateHz = 40e6;
    spec.name = "a.ddd.s16";

    Session s;
    QString error;
    QVERIFY(s.openCapture(raw, spec, &error));
    QVERIFY(!s.inArchive(0) && !s.hasArchive() && !s.isReference(0));
    QCOMPARE(s.trackCount(), 1);
    QCOMPARE(s.totalSamples(), qint64(kSamples));
    QCOMPARE(s.rateHz(), 40e6);
    QCOMPARE(s.bits(0), 10);
    QVERIFY(s.lagTable(0) == nullptr);
    QCOMPARE(s.reference(0), -1);
    QVERIFY(settle(s));
    QVERIFY(s.result().error.isEmpty());
    QVERIFY(s.result().stats[0].snrDb > 10.0);
    s.close();

    // A different path to show: the copy that was made to be read.
    QVERIFY(s.openCapture(raw, spec, &error, QStringLiteral("elsewhere/original.flac")));
    QCOMPARE(s.path(), QStringLiteral("elsewhere/original.flac"));
    s.close();

    // Not a capture in that format: nothing to show.
    spec.format = rfx::SampleFormat::Lds;
    write(m_dir.filePath("tiny.lds"), QByteArray(2, 1));
    QVERIFY(!s.openCapture(m_dir.filePath("tiny.lds"), spec, &error));
    QVERIFY(!error.isEmpty());

    // The window: the one track, its metadata without what only an archive has, and
    // no curve of deviation.
    MainWindow w;
    w.resize(1100, 800);
    spec.format = rfx::SampleFormat::S16;
    QVERIFY(w.openCapture(raw, spec));
    w.show();
    auto *session = w.findChild<Session *>();
    QVERIFY(settle(*session));
    auto *list = w.findChild<TrackList *>("trackList");
    QCOMPARE(list->count(), 1);
    auto *panel = w.findChild<MetadataPanel *>("metadataPanel");
    QCOMPARE(panel->topLevelItemCount(), 3);
    QCOMPARE(panel->topLevelItem(0)->childCount(), 3);
    // The section Capture: when it was made (from the .yaml beside it), the date of its file, and no note.
    {
        QTreeWidgetItem *capture = panel->topLevelItem(1);
        QCOMPARE(capture->text(0), QStringLiteral("Capture"));
        QCOMPARE(capture->childCount(), 3);
        const auto value = [&](int row) { return panel->itemWidget(capture->child(row), 0)->toolTip(); };
        QVERIFY2(value(0).contains("2026-10-27 07:33:55") && value(0).contains("07:34:05") && value(0).contains("10.8 s"), qPrintable(value(0)));
        QVERIFY(!value(1).isEmpty() && value(1) != QStringLiteral("not recorded"));
        QCOMPARE(value(2), QStringLiteral("none"));
    }
    auto *deviation = w.findChild<DeviationBlock *>("combinedDeviation")->findChild<DeviationView *>();
    QVERIFY(deviation->curves().isEmpty());
    w.setView(QStringLiteral("lanes"));
    QVERIFY(settle(*session));
    QVERIFY(w.saveScreenshot(m_dir.filePath("capture.png")));
}

void AnalyseGuiTests::asksWhatACaptureDoesNotSay()
{
    // A packed capture with a .yaml beside it, an 8-bit one, and a compressed one, in one
    // table: the format from the extension, the rate from the .yaml, and the bits that
    // format can carry.
    write(m_dir.filePath("x.lds"), QByteArray(50, 1));
    write(m_dir.filePath("x.yaml"), "{\n  \"sample_rate_hz\": 30000000\n}\n");
    write(m_dir.filePath("y.r8"), QByteArray(50, 1));
    CaptureDialog d({m_dir.filePath("x.lds"), m_dir.filePath("y.r8"), m_dir.filePath("z.flac")});
    QCOMPARE(d.rowCount(), 3);
    auto *table = d.findChild<QTableWidget *>();
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 3);

    QVERIFY(!d.compressed(0) && !d.compressed(1) && d.compressed(2));
    const auto rateOf = [&](int row) { return qobject_cast<QDoubleSpinBox *>(table->cellWidget(row, 2)); };
    const auto bitsOf = [&](int row) { return qobject_cast<QComboBox *>(table->cellWidget(row, 3)); };
    const auto formatOf = [&](int row) { return qobject_cast<QComboBox *>(table->cellWidget(row, 1)); };

    // .lds: the rate of the .yaml, and the ten bits that format carries.
    QCOMPARE(rateOf(0)->value(), 30.0);
    const rfx::CaptureSpec lds = d.spec(0);
    QCOMPARE(int(lds.format), int(rfx::SampleFormat::Lds));
    QCOMPARE(lds.rateHz, 30e6);
    QVERIFY(lds.bits.has_value() && *lds.bits == 10);
    QVERIFY(!bitsOf(0)->isEnabled());
    QCOMPARE(lds.name, std::string("x.lds"));

    // An unsigned 8-bit capture: at most eight bits, found when not given.
    QCOMPARE(int(d.spec(1).format), int(rfx::SampleFormat::U8));
    QVERIFY(!d.spec(1).bits.has_value());
    auto *model = qobject_cast<QStandardItemModel *>(bitsOf(1)->model());
    QVERIFY(!model->item(bitsOf(1)->findData(12))->isEnabled());
    QVERIFY(model->item(bitsOf(1)->findData(8))->isEnabled());

    // A compressed capture is decoded to 16-bit samples: the format is not a choice.
    QCOMPARE(int(d.spec(2).format), int(rfx::SampleFormat::S16));
    QVERIFY(!formatOf(2)->isEnabled());
    QVERIFY(formatOf(1)->isEnabled());

    // What a drop keeps: the captures and the archives that exist, not a .yaml, a .json or a folder.
    write(m_dir.filePath("keep.S16"), QByteArray(4, 1));
    write(m_dir.filePath("note.yaml"), "a: 1");
    write(m_dir.filePath("data.json"), "{}");
    QVERIFY(isRfFile(m_dir.filePath("keep.S16")) && isRfFile(m_archive));
    QVERIFY(!isRfFile(m_dir.filePath("note.yaml")) && !isRfFile(m_dir.filePath("data.json")));
    QVERIFY(!isRfFile(m_dir.filePath("missing.s16")) && !isRfFile(m_dir.path()));
    // A folder gives its RF files by name, not the .yaml and .json beside them.
    const QStringList inFolder = rfFilesInFolder(m_dir.path());
    QVERIFY(inFolder.contains(m_archive) && inFolder.contains(m_dir.filePath("keep.S16")));
    QVERIFY(!inFolder.contains(m_dir.filePath("note.yaml")) && !inFolder.contains(m_dir.filePath("data.json")));
    QVERIFY(rfFilesInFolder(m_dir.filePath("missing")).isEmpty());
    QVERIFY(isCompressedCapture("a/b.LDF") && isCompressedCapture("c.flac") && !isCompressedCapture("c.s16"));
    QVERIFY(isArchiveFile("d.RFX") && !isArchiveFile("d.s16"));
    QVERIFY(captureFilters().first().contains("*.lds") && captureFilters().first().contains("*.flac") && captureFilters().first().contains("*.rfx"));
}

void AnalyseGuiTests::opensSeveralFiles()
{
    // Two archives and a capture together: the tracks one after the other, each
    // archive's references among its own.
    const QString raw = m_dir.filePath("b.ddd.s16");
    Session::Source one;
    one.path = m_archive;
    Session::Source two = one;
    Session::Source capture;
    capture.path = raw;
    capture.capture = true;
    capture.spec.rateHz = 40e6;

    Session s;
    QString error;
    QVERIFY(s.open(std::vector<Session::Source>{one, two, capture}, &error));
    QCOMPARE(s.sourceCount(), 3);
    QCOMPARE(s.trackCount(), 5);
    QVERIFY(s.hasArchive());
    QVERIFY(s.inArchive(0) && s.inArchive(3) && !s.inArchive(4));
    QCOMPARE(s.firstTrackOfSource(1), 0);
    QCOMPARE(s.firstTrackOfSource(3), 2);
    QCOMPARE(s.firstTrackOfSource(4), 4);
    // The reference of the second track of each archive is the first of the same one.
    QCOMPARE(s.reference(1), 0);
    QCOMPARE(s.reference(3), 2);
    QCOMPARE(s.reference(0), -1);
    QCOMPARE(s.reference(4), -1);
    QVERIFY(s.isReference(0) && s.isReference(2) && !s.isReference(3) && !s.isReference(4));
    QVERIFY(s.lagTable(1) != nullptr && s.lagTable(3) != nullptr && s.lagTable(4) == nullptr);
    QCOMPARE(s.sourcePath(4), raw);
    QCOMPARE(s.paths().size(), 3);
    QVERIFY(s.fileSize() > 0);
    QCOMPARE(s.storedBytes(4), uint64_t(kSamples * 2));
    QVERIFY(settle(s));
    QCOMPARE(int(s.result().wave.size()), 5);
    QVERIFY(s.result().error.isEmpty());
    QVERIFY(!s.result().wave[4].empty());

    // One that cannot be read leaves what is open as it was.
    Session::Source bad;
    bad.path = m_dir.filePath("missing.rfx");
    QVERIFY(!s.open(std::vector<Session::Source>{one, bad}, &error));
    QVERIFY(error.contains("missing.rfx"));
    QCOMPARE(s.trackCount(), 5);
    QVERIFY(!s.open(std::vector<Session::Source>{}, &error));
    s.close();

    // The window: all the tracks in the list, the metadata of a capture among them, and
    // the title that says there are several files.
    MainWindow w;
    w.resize(1100, 900);
    QVERIFY(w.openSources({one, capture}));
    w.show();
    auto *session = w.findChild<Session *>();
    QVERIFY(settle(*session));
    QCOMPARE(w.findChild<TrackList *>("trackList")->count(), 3);
    QVERIFY(w.windowTitle().contains("other file"));
    auto *panel = w.findChild<MetadataPanel *>("metadataPanel");
    session->setSelected(2);
    QCOMPARE(panel->topLevelItemCount(), 3);
    session->setSelected(1);
    QCOMPARE(panel->topLevelItemCount(), 4);
    auto *deviation = w.findChild<DeviationBlock *>("combinedDeviation")->findChild<DeviationView *>();
    QCOMPARE(deviation->curves(), (QVector<int>{0, 1}));
    w.setView(QStringLiteral("lanes"));
    QVERIFY(settle(*session));
    QVERIFY(w.saveScreenshot(m_dir.filePath("several.png")));
}

void AnalyseGuiTests::tracksOfDifferentLengths()
{
    // A capture a third as long as the archive: the timeline is the longest one, the
    // short one stops where it ends and says so past it.
    const QString shortRaw = m_dir.filePath("short.s16");
    write(shortRaw, capture(0, 7).left(static_cast<qsizetype>(kSamples / 3 * 2)));
    Session::Source one;
    one.path = m_archive;
    Session::Source cut;
    cut.path = shortRaw;
    cut.capture = true;
    cut.spec.rateHz = 40e6;

    Session s;
    QString error;
    QVERIFY(s.open(std::vector<Session::Source>{one, cut}, &error));
    QCOMPARE(s.totalSamples(), qint64(kSamples));
    QCOMPARE(s.trackCount(), 3);
    QCOMPARE(static_cast<qint64>(s.track(2).words), qint64(kSamples / 3));

    s.goToEnd();
    QVERIFY(settle(s));
    QVERIFY(s.result().error.isEmpty());
    QVERIFY(s.result().pastEnd.size() > 2 && s.result().pastEnd[2]);
    QVERIFY(!s.result().pastEnd[0]);
    QVERIFY(!s.result().wave[0].empty());

    s.goToStart();
    QVERIFY(settle(s));
    QVERIFY(!s.result().pastEnd[2]);
    QVERIFY(!s.result().wave[2].empty());
}

void AnalyseGuiTests::hasThemesAndIcons()
{
    // The icons of ld-analyse are in the program, and the window has View > Theme and View > Style.
    MainWindow w;
    for (const char *icon : {"start-frame", "prev-frame", "next-frame", "end-frame", "zoom-in", "zoom-out", "zoom-original"}) {
        QVERIFY2(!QIcon(QStringLiteral(":/icons/Graphics/%1.png").arg(QLatin1String(icon))).isNull(), icon);
    }
    QMenu *view = nullptr;
    for (QAction *a : w.menuBar()->actions()) {
        if (a->text().remove(QLatin1Char('&')) == QLatin1String("View")) {
            view = a->menu();
        }
    }
    QVERIFY(view);
    QStringList names;
    for (QAction *a : view->actions()) {
        names << a->text();
    }
    QVERIFY(names.contains(QStringLiteral("Theme")) && names.contains(QStringLiteral("Style")));
    // A fixed dark palette is dark.
    QVERIFY(isDarkPalette(themePalette(ThemeMode::DarkGrey)));
    QVERIFY(!isDarkPalette(themePalette(ThemeMode::SoftLight)));
}

void AnalyseGuiTests::listOfTracksCanBePutAway()
{
    Session::Source capture;
    capture.path = m_dir.filePath("b.ddd.s16");
    capture.capture = true;
    capture.spec.rateHz = 40e6;
    Session::Source archive;
    archive.path = m_archive;

    // One track: the list is put away and the info stays, with the arrow to bring it back.
    MainWindow w;
    w.resize(1100, 800);
    QVERIFY(w.openSources({capture}));
    w.show();
    auto *list = w.findChild<TrackList *>("trackList");
    auto *toggle = w.findChild<QToolButton *>("listToggle");
    QVERIFY(list && toggle);
    QVERIFY(!list->isVisible());
    QVERIFY(w.findChild<MetadataPanel *>("metadataPanel")->isVisible());
    toggle->click();
    QVERIFY(list->isVisible());
    toggle->click();
    QVERIFY(!list->isVisible());

    // Files added to the one that is open: its tracks stay, the list comes back.
    QVERIFY(w.openSources({archive}, true));
    auto *session = w.findChild<Session *>();
    QCOMPARE(session->trackCount(), 3);
    QVERIFY(list->isVisible());
    QCOMPARE(session->sourceCount(), 2);
    QVERIFY(!session->inArchive(0) && session->inArchive(1));

    // A drop adds to what is open, it does not replace it; what is open already is not added again.
    const QString copy = m_dir.filePath("copy.rfx");
    QVERIFY(QFile::copy(m_archive, copy));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(copy), QUrl::fromLocalFile(m_archive), QUrl::fromLocalFile(copy), QUrl::fromLocalFile(m_dir.filePath("nothing.yaml"))});
    // (what a drop does with files open is asked; here it is said)
    w.setOpenMode(MainWindow::OpenMode::Add);
    w.dropFiles(&mime);
    QCOMPARE(session->trackCount(), 5);
    QCOMPARE(session->sourceCount(), 3);
    // Or it replaces them.
    w.setOpenMode(MainWindow::OpenMode::Replace);
    w.dropFiles(&mime);
    QCOMPARE(session->trackCount(), 5);   // (all of it is open already: nothing happens)
    const QString copy2 = m_dir.filePath("copy2.rfx");
    QVERIFY(QFile::copy(m_archive, copy2));
    QMimeData other;
    other.setUrls({QUrl::fromLocalFile(copy2)});
    w.dropFiles(&other);
    QCOMPARE(session->trackCount(), 2);
    QCOMPARE(session->sourceCount(), 1);
}

void AnalyseGuiTests::blocksCanBeTakenAway()
{
    MainWindow w;
    w.resize(1100, 800);
    Session::Source capture;
    capture.path = m_dir.filePath("b.ddd.s16");
    capture.capture = true;
    capture.spec.rateHz = 40e6;
    QVERIFY(w.openSources({capture}));
    w.show();
    QMenu *view = nullptr;
    for (QAction *a : w.menuBar()->actions()) {
        if (a->text().remove(QLatin1Char('&')) == QLatin1String("View")) {
            view = a->menu();
        }
    }
    QVERIFY(view);
    const auto action = [&](const QString &name) -> QAction * {
        for (QAction *a : view->actions()) {
            if (a->text().remove(QLatin1Char('&')) == name) {
                return a;
            }
        }
        return nullptr;
    };
    // All shown to begin with.
    for (const QString &name : {QStringLiteral("RF waveform"), QStringLiteral("Spectrum"), QStringLiteral("Speed deviation"), QStringLiteral("Statistics")}) {
        QVERIFY2(action(name) && action(name)->isCheckable() && action(name)->isChecked(), qPrintable(name));
    }
    auto *spectrum = w.findChild<SpectrumView *>("combinedSpectrum");
    auto *wave = w.findChild<WaveformView *>("combinedWaveform");
    auto *stats = w.findChild<StatsPanel *>("combinedStats");
    QVERIFY(spectrum->isVisible() && wave->isVisible() && stats->isVisible());
    action("Spectrum")->setChecked(false);
    QVERIFY(!spectrum->isVisible() && wave->isVisible());
    action("Statistics")->setChecked(false);
    QVERIFY(!stats->isVisible());
    // Back as they were (the choice is kept in the settings).
    action("Spectrum")->setChecked(true);
    action("Statistics")->setChecked(true);
    QVERIFY(spectrum->isVisible() && stats->isVisible());
}

void AnalyseGuiTests::graphsFollowTheSizeAndShowValues()
{
    Session::Source capture;
    capture.path = m_dir.filePath("b.ddd.s16");
    capture.capture = true;
    capture.spec.rateHz = 40e6;
    MainWindow w;
    w.resize(1000, 800);
    QVERIFY(w.openSources({capture}));
    w.show();
    auto *session = w.findChild<Session *>();
    QVERIFY(settle(*session));
    QMenu *view = nullptr;
    for (QAction *a : w.menuBar()->actions()) {
        if (a->text().remove(QLatin1Char('&')) == QLatin1String("View")) {
            view = a->menu();
        }
    }
    QVERIFY(view);
    QAction *gain = nullptr;
    for (QAction *a : view->actions()) {
        if (a->text() == QLatin1String("Change gain on resize")) {
            gain = a;
        }
    }
    QVERIFY(gain && gain->isCheckable() && gain->isChecked());
    QAction *normalise = nullptr;
    for (QAction *a : view->actions()) {
        if (a->text() == QLatin1String("Normalise FFT view")) {
            normalise = a;
        }
    }
    QVERIFY(normalise && normalise->isCheckable() && normalise->isChecked());

    // As it starts, a taller graph gets more gain so that the signal fills the same part of it: the number goes up, and
    // the time window does not change with the width.
    auto *amplitude = w.findChild<QDoubleSpinBox *>("waveAmplitude");
    QVERIFY(amplitude);
    QTest::qWait(100);
    const double gainBefore = amplitude->value();
    const qint64 timeBefore = session->windowLength();
    w.resize(1000, 950);
    QTest::qWait(150);
    QVERIFY(amplitude->value() > gainBefore);
    w.resize(1300, 950);
    QTest::qWait(150);
    QCOMPARE(session->windowLength(), timeBefore);
    w.resize(1000, 800);
    QTest::qWait(150);

    // Not following: the gain stays (the axis goes further out), and a wider graph shows more time.
    gain->setChecked(false);
    const double kept = amplitude->value();
    w.resize(1000, 950);
    QTest::qWait(150);
    QCOMPARE(amplitude->value(), kept);
    const qint64 before = session->windowLength();
    w.resize(1300, 950);
    QTest::qWait(150);
    QVERIFY(session->windowLength() > before);
    gain->setChecked(true);

    // The spectrum normalised: its amplitude is what the normalisation comes to (and changes with the size); setting the
    // amplitude by hand puts the normalisation off.
    auto *fft = w.findChild<QDoubleSpinBox *>("fftAmplitude");
    QVERIFY(fft);
    QTest::qWait(200);
    const double autoGain = fft->value();
    QVERIFY(autoGain != 1.0);
    fft->setValue(autoGain * 2.0);
    QVERIFY(!normalise->isChecked());
    normalise->setChecked(true);
    QTest::qWait(200);
    QVERIFY(std::fabs(fft->value() - autoGain) < 0.02 * autoGain);

    // Going to All lanes and back to one track does not touch the amplitude (the new graphs are not a resize).
    w.setView(QStringLiteral("individual"));
    QVERIFY(settle(*session));
    QTest::qWait(200);
    const double gainKept = amplitude->value();
    const qint64 timeKept = session->windowLength();
    w.setView(QStringLiteral("lanes"));
    QVERIFY(settle(*session));
    QTest::qWait(200);
    w.setView(QStringLiteral("individual"));
    QVERIFY(settle(*session));
    QTest::qWait(200);
    QCOMPARE(amplitude->value(), gainKept);
    QCOMPARE(session->windowLength(), timeKept);
    w.setView(QStringLiteral("combined"));
    QTest::qWait(100);

    // The mouse over a graph draws the values under it (nothing to crash on, and a picture of it).
    auto *wave = w.findChild<WaveformView *>("combinedWaveform");
    QTest::mouseMove(wave, QPoint(120, 60));
    QTest::qWait(60);
    QVERIFY(!wave->grab().isNull());
    auto *spectrum = w.findChild<SpectrumView *>("combinedSpectrum");
    QTest::mouseMove(spectrum, QPoint(120, 60));
    QTest::qWait(60);
    QVERIFY(!spectrum->grab().isNull());
}

void AnalyseGuiTests::compactModeAndSpectrumControls()
{
    Session::Source capture;
    capture.path = m_dir.filePath("b.ddd.s16");
    capture.capture = true;
    capture.spec.rateHz = 40e6;
    MainWindow w;
    w.resize(1100, 800);
    QVERIFY(w.openSources({capture, capture}));
    w.show();
    auto *session = w.findChild<Session *>();
    QVERIFY(settle(*session));
    auto *stats = w.findChild<StatsPanel *>("combinedStats");
    auto *list = w.findChild<TrackList *>("trackList");
    auto *info = w.findChild<MetadataPanel *>("metadataPanel");
    QVERIFY(stats && list && info);

    QMenu *view = nullptr;
    for (QAction *a : w.menuBar()->actions()) {
        if (a->text().remove(QLatin1Char('&')) == QLatin1String("View")) {
            view = a->menu();
        }
    }
    QVERIFY(view);
    QAction *compact = nullptr;
    for (QAction *a : view->actions()) {
        if (a->text().remove(QLatin1Char('&')) == QLatin1String("Open in compact mode")) {
            compact = a;
        }
    }
    QVERIFY(compact && compact->isCheckable() && !compact->isChecked());
    QVERIFY(stats->isVisible() && list->isVisible());

    // Compact: the waveform and the spectrum only, the lists put away.
    compact->setChecked(true);
    QTest::qWait(100);
    QVERIFY(!stats->isVisible() && !list->isVisible() && !info->isVisible());
    QVERIFY(w.findChild<WaveformView *>("combinedWaveform")->isVisible());
    QVERIFY(w.findChild<SpectrumView *>("combinedSpectrum")->isVisible());

    // The controls of the spectrum are with it in the individual view too.
    w.setView(QStringLiteral("individual"));
    QVERIFY(settle(*session));
    QTest::qWait(100);
    auto *fftGain = w.findChild<QDoubleSpinBox *>("fftAmplitude");
    QVERIFY(fftGain && fftGain->isVisible());

    // A file opened in it starts without the list and the page of metadata, and they can be brought back.
    QVERIFY(w.openSources({capture, capture}));
    QVERIFY(settle(*session));
    QVERIFY(!list->isVisible() && !info->isVisible());
    w.findChild<QToolButton *>("listToggle")->click();
    QVERIFY(list->isVisible());
    w.findChild<QToolButton *>("listToggle")->click();

    // In the individual view too, turning the normalisation on puts the amplitude of the spectrum to what it comes to.
    QAction *normalise = nullptr;
    for (QAction *a : view->actions()) {
        if (a->text() == QLatin1String("Normalise FFT view")) {
            normalise = a;
        }
    }
    QVERIFY(normalise);
    w.setView(QStringLiteral("individual"));
    QVERIFY(settle(*session));
    QTest::qWait(150);
    fftGain->setValue(fftGain->value() * 3.0);   // by hand: not normalised any more
    QVERIFY(!normalise->isChecked());
    const double manual = fftGain->value();
    normalise->setChecked(true);
    QTest::qWait(250);
    QVERIFY(std::fabs(fftGain->value() - manual) > 0.01 * manual);

    // Out of it, as it was.
    compact->setChecked(false);
    w.setView(QStringLiteral("combined"));
    QTest::qWait(100);
    QVERIFY(stats->isVisible() && list->isVisible());
}

// The speed measured from the RF: a made-up NTSC capture whose lines are 0.1% shorter than the nominal ones (a disc going 1000 ppm
// too fast) has that speed with the box ticked, with or without a reference, and none without it.
void AnalyseGuiTests::speedFromTheRf()
{
    const double fs = 40e6;
    const double fast = 1.001;
    const double hlineUs = 227.5e6 / (315.0e6 / 88.0) / 2.0 / fast;
    const double hz = 1.7e6 / 140.0;
    const double blank = 8.1e6;
    const double sync = blank - 40.0 * hz;
    const size_t halfLines = 1050 * 3;
    const size_t samples = static_cast<size_t>(static_cast<double>(halfLines) * hlineUs * fs / 1e6) + 4096;
    std::vector<float> frequency(samples, static_cast<float>(blank));
    auto pulse = [&](double startUs, double lengthUs) {
        const size_t a = static_cast<size_t>(startUs * fs / 1e6);
        const size_t b = std::min(samples, static_cast<size_t>((startUs + lengthUs) * fs / 1e6));
        for (size_t i = a; i < b; ++i) {
            frequency[i] = static_cast<float>(sync);
        }
    };
    for (size_t h = 0; h < halfLines; ++h) {
        const size_t inFrame = h % 1050;
        const double at = static_cast<double>(h) * hlineUs;
        const size_t vbiStart = inFrame < 525 ? 0 : 525;
        if (inFrame >= vbiStart && inFrame < vbiStart + 18) {
            const size_t k = inFrame - vbiStart;
            pulse(at, (k >= 6 && k < 12) ? 27.1 : 2.3);
        } else if (inFrame % 2 == 0) {
            pulse(at, 4.7);
        }
    }
    QByteArray pcm(static_cast<qsizetype>(samples * 2), 0);
    double phase = 0.0;
    uint32_t x = 7;
    for (size_t i = 0; i < samples; ++i) {
        phase = std::fmod(phase + 2.0 * M_PI * static_cast<double>(frequency[i]) / fs, 2.0 * M_PI);
        x = x * 1664525u + 1013904223u;
        const int16_t s = static_cast<int16_t>(std::lround(9000.0 * std::sin(phase) + (static_cast<double>(x >> 8) / 16777216.0 - 0.5) * 60.0));
        std::memcpy(pcm.data() + 2 * i, &s, 2);
    }
    const QString path = m_dir.filePath("fast.ddd.s16");
    write(path, pcm);

    Session s;
    Session *session = &s;
    {
        rfx::CaptureSpec spec;
        spec.format = rfx::SampleFormat::S16;
        spec.rateHz = fs;
        spec.bits = 16;
        QString error;
        QVERIFY(s.openCapture(path, spec, &error));
    }
    QVERIFY(!session->rfSpeed());
    QVERIFY(!session->hasSpeed(0));
    session->setRfSpeed(true);
    QElapsedTimer waited;
    waited.start();
    while ((session->rfSpeedBusy() || !session->hasSpeed(0)) && waited.elapsed() < 60000) {
        QTest::qWait(20);
    }
    QVERIFY(session->hasSpeed(0));
    const double ppm = session->speedAt(0, 2000000);
    QVERIFY2(std::fabs(ppm - 1000.0) < 25.0, qPrintable(QString::number(ppm)));
    QVERIFY(!session->speedIsZero(0));
    if (qEnvironmentVariableIsSet("RF_TEST_PICTURE")) {
        DeviationView view(session);
        view.resize(800, 220);
        view.grab().save(qEnvironmentVariable("RF_TEST_PICTURE"));
    }
    session->setRfSpeed(false);
    QVERIFY(!session->hasSpeed(0));
    session->close();
}

void AnalyseGuiTests::decodesCompressedCaptures()
{
    // A FLAC as ld-decode writes it: the rate in kHz in its header (40000 for 40 MHz).
    const QString flac = m_dir.filePath("c.flac");
    QProcess make;
    make.start(rfx::ffmpegProgram(), {"-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=40000:duration=3", "-sample_fmt", "s16", "-c:a", "flac", flac});
    if (!make.waitForStarted() || !make.waitForFinished(30000) || make.exitCode() != 0) {
        QSKIP("no ffmpeg to make a compressed capture with");
    }
    const auto temporary = [] { return QDir::temp().entryList({"rf-analyse-decoded-*"}, QDir::Dirs | QDir::NoDotAndDotDot).size(); };
    const int before = temporary();
    MainWindow w;
    w.resize(1000, 700);
    // Opened without asking how to read it, and without decoding it to a file: the header gives the rate and the length,
    // and the samples are decoded where they are looked at.
    QVERIFY(w.openFile(flac));
    w.show();
    auto *session = w.findChild<Session *>();
    QVERIFY(settle(*session));
    QCOMPARE(session->trackCount(), 1);
    QCOMPARE(session->rateHz(), 40e6);
    QCOMPARE(static_cast<qint64>(session->track(0).words), qint64(40000 * 3));
    QVERIFY(session->result().error.isEmpty());
    QVERIFY(!session->result().wave.empty() && !session->result().wave[0].empty());
    // The signal is there (a sine of the rate of the header, loud), and the bar can be taken to the end and back.
    double peak = 0.0;
    for (const float v : session->result().wave[0]) {
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
    }
    QVERIFY(peak > 10.0);
    session->goToEnd();
    QVERIFY(settle(*session));
    QVERIFY(session->result().error.isEmpty() && !session->result().pastEnd[0]);
    session->goToStart();
    QVERIFY(settle(*session));
    // The rough spectrum of the capture is made in the background (a few reads over the whole of it) and is there to
    // show while the exact one is late.
    QElapsedTimer waited;
    waited.start();
    while (!session->coarseSpectrum(0) && waited.elapsed() < 30000) {
        QTest::qWait(100);
    }
    const auto rough = session->coarseSpectrum(0);
    QVERIFY(rough && !rough->db.empty());
    // (a sine at 1000 Hz of a header at 40000 Hz: its peak is at that frequency of the header, in the time of the capture)
    QVERIFY(session->result().spectra[0].db.size() == rough->db.size());

    // Nothing was written in a folder of the temporary files.
    QCOMPARE(temporary(), before);
}

QTEST_MAIN(AnalyseGuiTests)
#include "analyse_gui_tests.moc"
