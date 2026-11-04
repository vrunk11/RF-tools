/******************************************************************************
 * tracklist.h
 * rf-analyse - the tracks of the file: whether each is shown, which one is
 * selected
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QListWidget>

class Session;

// A row is a check box, the colour of the track, its name, and its duration and
// what it takes in the file. A click on a track selects it and shows the metadata
// panel; a click on the selected one hides the panel.
class TrackList : public QListWidget
{
    Q_OBJECT

public:
    explicit TrackList(Session *session, QWidget *parent = nullptr);

    // Whether the metadata panel is out, for the arrow on the selected track.
    void setPanelShown(bool shown);

    QSize sizeHint() const override { return {170, 200}; }

signals:
    // The user clicked the row `row`; `wasSelected` is whether it already was.
    void rowClicked(int row, bool wasSelected);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void rebuild();
    void refreshChecks();

    Session *m_session;
    bool m_panelShown = true;
    bool m_updating = false;
    friend class TrackDelegate;
};
