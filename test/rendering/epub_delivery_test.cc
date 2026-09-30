#include "../../src/epub_delivery.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <thread>
#include <vector>

class EpubNetworkReply : public QObject { Q_OBJECT };
class OtherReply : public QObject { Q_OBJECT };
class DerivedReply : public EpubNetworkReply { Q_OBJECT };

static void check(bool condition, const char *message) {
    if (!condition) qFatal("FAIL: %s", message);
}

// Existing delivery tests use an established GUI thread and vary the load window.
static int delivery_interval(bool loading, int interval, const QObject *receiver) {
    return ntf_epub_delivery_interval(true, interval, receiver,
        [] { return true; }, [loading] { return loading; });
}

static void test_thread_filter(QCoreApplication &app) {
    int guard_calls = 0, warnings = 0, window_reads = 0;
    auto guard = [&] {
        ++guard_calls;
        bool on_gui = QThread::currentThread() == app.thread();
        if (!on_gui) ++warnings;
        return on_gui;
    };
    auto window = [&] { ++window_reads; return true; };
    std::thread worker([&] {
        EpubNetworkReply reply;
        OtherReply other;
        DerivedReply derived;
        for (int interval : {-1, 0, 1, 99, 101, 1000})
            check(ntf_epub_delivery_interval(true, interval, &reply, guard, window) == interval,
                  "unrelated worker interval changed");
        for (const QObject *receiver : {static_cast<const QObject *>(&other),
                                        static_cast<const QObject *>(&derived),
                                        static_cast<const QObject *>(nullptr)})
            check(ntf_epub_delivery_interval(true, 100, receiver, guard, window) == 100,
                  "unrelated worker receiver changed");
        check(ntf_epub_delivery_interval(false, 100, &reply, guard, window) == 100,
              "disabled fix changed a worker timer");
        check(guard_calls == 0 && warnings == 0 && window_reads == 0,
              "unrelated worker timer reached the thread guard or chapter state");
        check(ntf_epub_delivery_interval(true, 100, &reply, guard, window) == 100,
              "wrong-thread EPUB timer was accelerated");
        check(guard_calls == 1 && warnings == 1 && window_reads == 0,
              "wrong-thread EPUB timer was hidden or read GUI state");
        // Even a closed load window must not hide a real thread violation or be read
        // from the worker. Thread validation has to precede either window callback.
        auto closed_window = [&] { ++window_reads; return false; };
        check(ntf_epub_delivery_interval(true, 100, &reply, guard, closed_window) == 100,
              "wrong-thread EPUB timer outside a load changed");
        check(guard_calls == 2 && warnings == 2 && window_reads == 0,
              "closed window hid a wrong-thread EPUB timer");
    });
    worker.join();
    EpubNetworkReply reply;
    check(ntf_epub_delivery_interval(true, 100, &reply, guard, window) == 0,
          "worker warnings disabled later GUI delivery");
    check(guard_calls == 3 && warnings == 2 && window_reads == 1,
          "GUI delivery did not check the thread before reading chapter state");
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    test_thread_filter(app);
    EpubNetworkReply reply;
    OtherReply other;
    DerivedReply derived;
    check(delivery_interval(true, 100, &reply) == 0, "EPUB delay was retained");
    check(delivery_interval(false, 100, &reply) == 100, "timer outside a load changed");
    check(delivery_interval(true, 100, &other) == 100, "unrelated reply changed");
    check(delivery_interval(true, 100, &derived) == 100, "unverified subclass changed");
    check(delivery_interval(true, 100, nullptr) == 100, "receiverless timer changed");
    for (int interval : {-1, 0, 1, 99, 101, 1000})
        check(delivery_interval(true, interval, &reply) == interval,
              "a different interval changed");

    QEventLoop loop;
    std::vector<int> order;
    QTimer::singleShot(delivery_interval(true, 100, &reply), &reply, [&] {
        order.push_back(2);
        QTimer::singleShot(delivery_interval(true, 100, &reply), &reply, [&] {
            order.push_back(4);
            loop.quit();
        });
        order.push_back(3);
    });
    order.push_back(1);
    check(order.size() == 1, "delivery became synchronous");
    QTimer::singleShot(1000, &loop, SLOT(quit()));
    loop.exec();
    check(order == std::vector<int>({1, 2, 3, 4}), "chunk callbacks ran out of order");

    int cancelled_calls = 0;
    EpubNetworkReply *cancelled = new EpubNetworkReply;
    QTimer::singleShot(delivery_interval(true, 100, cancelled), cancelled,
                      [&] { ++cancelled_calls; });
    delete cancelled;
    QTimer::singleShot(0, &loop, SLOT(quit()));
    loop.exec();
    check(cancelled_calls == 0, "destroyed reply still received its callback");
    qDebug("PASS: EPUB worker-thread filtering, thread violations, interval guards, asynchronous delivery, callback order, and cancellation");
}

#include "epub_delivery_test.moc"
