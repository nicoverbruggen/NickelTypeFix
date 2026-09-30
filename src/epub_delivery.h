#ifndef NTF_EPUB_DELIVERY_H
#define NTF_EPUB_DELIVERY_H

#include <QObject>
#include <cstring>

// Keep the queued callback and its receiver lifetime rules. Only remove the artificial gap
// between chunks of a local EPUB reply during a tracked chapter load.
// Nickel also schedules unrelated timers on worker threads. Reject those before the
// thread guard can claim a thread or warn. A matching reply must still pass the guard
// before reading the chapter-load state, which belongs to the GUI thread.
template<typename OnQtThread, typename WindowOpen>
static inline int ntf_epub_delivery_interval(bool enabled, int interval, const QObject *receiver,
                                            OnQtThread on_qt_thread, WindowOpen window_open) {
    if (enabled && interval == 100 && receiver
        && std::strcmp(receiver->metaObject()->className(), "EpubNetworkReply") == 0
        && on_qt_thread() && window_open())
        return 0;
    return interval;
}

#endif
