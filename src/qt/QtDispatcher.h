// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <QObject>
#include <functional>
#include <utility>

#include "IUiDispatcher.h"

// Qt implementation of IUiDispatcher: queues the closure onto the thread that
// owns this object (the UI thread) via QMetaObject::invokeMethod with a queued
// connection. Construct it on the UI thread.
class QtDispatcher : public QObject, public IUiDispatcher {
  Q_OBJECT
 public:
  explicit QtDispatcher(QObject* parent = nullptr) : QObject(parent) {}

  void post(std::function<void()> fn) override {
    QMetaObject::invokeMethod(
        this, [fn = std::move(fn)]() { fn(); }, Qt::QueuedConnection);
  }
};
