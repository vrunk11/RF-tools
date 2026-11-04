/******************************************************************************
 * metadatapanel.h
 * rf-analyse - what the file says of the selected source
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#pragma once

#include <QMap>
#include <QTreeWidget>

class Session;

// Three sections, which fold: the file, the samples, the coding.
class MetadataPanel : public QTreeWidget
{
    Q_OBJECT

public:
    explicit MetadataPanel(Session *session, QWidget *parent = nullptr);

    QSize sizeHint() const override { return {190, 300}; }

private:
    void rebuild();

    Session *m_session;
    QMap<QString, bool> m_expanded;
};
