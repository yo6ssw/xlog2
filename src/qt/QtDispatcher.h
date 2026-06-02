#pragma once

#include "IUiDispatcher.h"

#include <QObject>

#include <functional>
#include <utility>

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
