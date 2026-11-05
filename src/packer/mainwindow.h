/******************************************************************************
 * mainwindow.h
 * rf-packer - the window: the list of tracks and the panel with what is known of the one picked
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "document.h"

#include <QMainWindow>

class AppearanceMenus;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow();

    Document *document() { return &m_doc; }
    // Open an .rfx file (the list starts from its tracks) or add captures to the list. False when something could not be read.
    bool openArchive(const QString &path);
    bool addCaptures(const QStringList &paths);
    // The track whose information is shown (-1: none).
    void pick(int index);
    int picked() const { return m_picked; }
    bool saveScreenshot(const QString &path);
    // Write the list: asks where (a copy or over the archive) unless `output` is given; the work runs in the program again.
    bool process(const QString &output = QString(), const rfx::PackOptions *options = nullptr);
    // Give the captures of the opened file back, in a folder (asked unless `folder` is given): the track `only` (its number in the file), or all.
    bool unpack(std::optional<size_t> only, const QString &folder = QString());
    // Locate the lines of the tracks of the opened file from their RF and keep them in it (the pass 0 curve), asking first.
    bool computePass0(bool ask = true);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildMenus();
    void rebuildList();
    void refreshInfo();
    void refreshSummary();
    void showInfo(bool shown);
    void syncFormat();
    void newList();
    void chooseArchive();
    void chooseCaptures();
    bool confirmDiscard();
    void editName(const QString &text);
    void editNotes();
    void addSidecar();
    void removeSidecar();
    void move(int index, int delta);
    void removeTrack(int index);

    Document m_doc;
    AppearanceMenus *m_appearance = nullptr;
    int m_picked = -1;
    bool m_editing = false;
    bool m_rebuildQueued = false;
    bool m_infoShown = true;

    QPushButton *m_processButton = nullptr;
    QComboBox *m_codec = nullptr;
    QDoubleSpinBox *m_formatRate = nullptr;
    QSpinBox *m_formatBits = nullptr;
    QPushButton *m_infoButton = nullptr;
    QWidget *m_listHost = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QScrollArea *m_listScroll = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_hint = nullptr;
    QAction *m_infoAction = nullptr;
    QAction *m_unpackAll = nullptr;
    QAction *m_pass0Action = nullptr;

    QWidget *m_info = nullptr;
    QLabel *m_infoTitle = nullptr;
    QLineEdit *m_name = nullptr;
    QDoubleSpinBox *m_rate = nullptr;
    QLabel *m_facts = nullptr;
    QListWidget *m_sidecars = nullptr;
    QPushButton *m_addSidecar = nullptr;
    QPushButton *m_removeSidecar = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QPushButton *m_deleteNotes = nullptr;
};

#endif // MAINWINDOW_H
