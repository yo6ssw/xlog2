#pragma once

#include <functional>

// Marshals a callback from a worker thread onto the UI thread. This is the one
// seam the otherwise-neutral services need from the toolkit: each backend
// provides an implementation (gtkmm via Glib::Dispatcher, Qt via a queued
// QMetaObject::invokeMethod). Services call post() from their worker; the
// closure runs later on the UI thread.
class IUiDispatcher {
public:
    virtual ~IUiDispatcher() = default;

    // Thread-safe. `fn` is queued and invoked on the UI thread; multiple posts
    // run in order.
    virtual void post(std::function<void()> fn) = 0;
};
