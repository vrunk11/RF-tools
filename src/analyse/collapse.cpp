/******************************************************************************
 * collapse.cpp
 * rf-analyse - the mini arrow in the title of a panel, which reduces it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#include "collapse.h"

#include <QChar>
#include <QObject>
#include <QToolButton>
#include <QVBoxLayout>

QToolButton *makeCollapseButton(QWidget *parent, const std::function<void(bool)> &changed)
{
    auto *button = new QToolButton(parent);
    button->setCheckable(true);
    button->setAutoRaise(false);
    button->setFixedSize(18, 18);
    const auto look = [button](bool reduced) {
        button->setText(QString(QChar(reduced ? 0x25BE : 0x25B4)));
        button->setToolTip(reduced ? QObject::tr("Show this panel again") : QObject::tr("Reduce this panel to its title"));
    };
    look(false);
    QObject::connect(button, &QToolButton::toggled, button, [look, changed](bool reduced) {
        look(reduced);
        if (changed) {
            changed(reduced);
        }
    });
    return button;
}

QFrame *sunkenFrame(QWidget *inner, QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setFrameShadow(QFrame::Sunken);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    inner->setParent(frame);
    layout->addWidget(inner);
    return frame;
}

QFrame *makeSeparator(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setFixedHeight(2);
    line->hide();
    return line;
}
