#include "../../src/epub_delivery.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <vector>

class EpubNetworkReply : public QObject { Q_OBJECT };
class OtherReply : public QObject { Q_OBJECT };
class DerivedReply : public EpubNetworkReply { Q_OBJECT };

static void check(bool condition, const char *message) {
    if (!condition) qFatal("FAIL: %s", message);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    EpubNetworkReply reply;
    OtherReply other;
    DerivedReply derived;
    check(ntf_epub_delivery_interval(true, 100, &reply) == 0, "EPUB delay was retained");
    check(ntf_epub_delivery_interval(false, 100, &reply) == 100, "timer outside a load changed");
    check(ntf_epub_delivery_interval(true, 100, &other) == 100, "unrelated reply changed");
    check(ntf_epub_delivery_interval(true, 100, &derived) == 100, "unverified subclass changed");
    check(ntf_epub_delivery_interval(true, 100, nullptr) == 100, "receiverless timer changed");
    for (int interval : {-1, 0, 1, 99, 101, 1000})
        check(ntf_epub_delivery_interval(true, interval, &reply) == interval,
              "a different interval changed");

    QEventLoop loop;
    std::vector<int> order;
    QTimer::singleShot(ntf_epub_delivery_interval(true, 100, &reply), &reply, [&] {
        order.push_back(2);
        QTimer::singleShot(ntf_epub_delivery_interval(true, 100, &reply), &reply, [&] {
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
    QTimer::singleShot(ntf_epub_delivery_interval(true, 100, cancelled), cancelled,
                      [&] { ++cancelled_calls; });
    delete cancelled;
    QTimer::singleShot(0, &loop, SLOT(quit()));
    loop.exec();
    check(cancelled_calls == 0, "destroyed reply still received its callback");
    qDebug("PASS: EPUB interval guards, asynchronous delivery, callback order, and cancellation");
}

#include "epub_delivery_test.moc"
