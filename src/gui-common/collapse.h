/******************************************************************************
 * collapse.h
 * rf-analyse - the mini arrow in the title of a panel, which reduces it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 RF-tools contributors
 *
 * This file is part of RF-tools.
 ******************************************************************************/

#ifndef COLLAPSE_H
#define COLLAPSE_H

#include <QWidget>

#include <functional>

#include <QFrame>
#include <QToolButton>

// A small outlined button with an up arrow (reduce) or a down arrow (expand again). Clicking it
// calls `changed` with whether the panel is now reduced; the caller hides what is to be hidden.
QToolButton *makeCollapseButton(QWidget *parent, const std::function<void(bool)> &changed);

// A thin line under the title of a panel that is reduced to it.
QFrame *makeSeparator(QWidget *parent);

// `inner` in a sunken frame, as the graphs and tables of the panels are.
QFrame *sunkenFrame(QWidget *inner, QWidget *parent);

#endif // COLLAPSE_H
