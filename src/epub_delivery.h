#ifndef NTF_EPUB_DELIVERY_H
#define NTF_EPUB_DELIVERY_H

#include <QObject>
#include <cstring>

// Keep the queued callback and its receiver lifetime rules. Only remove the artificial gap
// between chunks of a local EPUB reply during a tracked chapter load.
static inline int ntf_epub_delivery_interval(bool loading, int interval, const QObject *receiver) {
    if (loading && interval == 100 && receiver
        && std::strcmp(receiver->metaObject()->className(), "EpubNetworkReply") == 0)
        return 0;
    return interval;
}

#endif
