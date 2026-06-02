#pragma once

#include "IUiDispatcher.h"

#include <glibmm/dispatcher.h>

#include <functional>
#include <mutex>
#include <vector>

// gtkmm implementation of IUiDispatcher: a thread-safe queue drained by a single
// Glib::Dispatcher. Must be constructed on the UI (main-loop) thread, like any
// Glib::Dispatcher. Worker threads call post(); the closures run on the UI
// thread when the GLib main loop dispatches.
class GlibDispatcher : public IUiDispatcher {
public:
    GlibDispatcher() {
        dispatcher_.connect([this]() { drain(); });
    }

    void post(std::function<void()> fn) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(fn));
        }
        dispatcher_.emit();
    }

private:
    void drain() {
        std::vector<std::function<void()>> items;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            items.swap(queue_);
        }
        for (auto& fn : items)
            fn();
    }

    Glib::Dispatcher dispatcher_;
    std::mutex       mutex_;
    std::vector<std::function<void()>> queue_;
};
