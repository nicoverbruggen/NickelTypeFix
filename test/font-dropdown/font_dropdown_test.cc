#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtGui/QFontDatabase>
#include <QtGui/QImage>
#include <QtCore/QDebug>
#include <thread>
#include "../rendering/qt_runtime.h"

#include "font_dropdown.h"

// Nickel's popup rows share the dropdown ancestor with its selected label.
class RegularElidableLabel : public QLabel {
    Q_OBJECT
public:
    explicit RegularElidableLabel(QWidget *parent) : QLabel(parent) {}
};

static void check(bool condition, const char *message) {
    if (!condition) qFatal("FAIL: %s", message);
}

static QImage snapshot(QLabel &label) {
    QImage image(label.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    label.render(&image);
    return image;
}

static int setterCalls = 0;
static void set_text(QLabel *label, const QString &text) {
    ++setterCalls;
    label->setText(text);
}

static int select_font(QLabel &label, const QString &text) {
    label.setText("previous selection");
    const int before = setterCalls;
    const int held = ntf_set_font_dropdown_text(&label, text, set_text);
    check(setterCalls == before + 1, "original setter was not called exactly once");
    return held;
}

int main(int argc, char **argv) {
    const bool ng = qgetenv("QT_HARFBUZZ") != "old";
    ntf_test_select_shaper(ng);
    QApplication app(argc, argv);
    if (qVersion() != QStringLiteral("5.2.1")) {
        qWarning("This regression reproduces Qt 5.2.1 font caching; use that runtime.");
        return 77;
    }
    check(argc == 3, "supply Vollkorn-Regular.ttf and NotoSans-Regular.ttf");
    check(QFontDatabase::addApplicationFont(argv[1]) >= 0, "fallback font did not load");
    int id = QFontDatabase::addApplicationFont(argv[2]);
    check(id >= 0, "selected font did not load");
    check(QFontDatabase::applicationFontFamilies(id).contains("Noto Sans"), "wrong selected font fixture");

    QWidget dropdown, otherParent;
    dropdown.setObjectName("fontFaceComboBox");
    QLabel selected(&dropdown), unrelated(&otherParent), otherFamily(&dropdown);
    RegularElidableLabel popup(&dropdown);
    const QString text = QStringLiteral("<font face=\"Noto Sans\">Noto Sans</font>");
    for (QLabel *label : {&selected, &unrelated, static_cast<QLabel *>(&popup), &otherFamily}) {
        label->resize(500, 80);
        label->setText(text);
    }
    otherFamily.setText("<font face=\"Vollkorn\">Vollkorn</font>");
    const QImage correct = snapshot(selected);
    snapshot(unrelated);
    snapshot(popup);
    const QImage otherCorrect = snapshot(otherFamily);

    for (int cycle = 0; cycle < 2; ++cycle) {
        check(QFontDatabase::removeApplicationFont(id), "selected font removal failed");
        const QImage removed = snapshot(selected);
        check((removed != correct) == ng, "fixture did not reproduce the old/NG difference");
        const QImage unrelatedBefore = snapshot(unrelated);
        const QImage popupBefore = snapshot(popup);
        id = QFontDatabase::addApplicationFont(argv[2]);
        check(id >= 0, "selected font reload failed");
        check(snapshot(selected) == removed, "reload unexpectedly rebuilt the label");
        check(ntf_refresh_font_dropdown("Vollkorn") == 1, "other family refresh did not match its label");
        check(snapshot(selected) == removed, "refreshing another family touched the selected label");
        check(ntf_refresh_font_dropdown(QString()) == 0, "empty family matched a label");
        int workerResult = -1;
        std::thread worker([&] { workerResult = ntf_refresh_font_dropdown("Noto Sans"); });
        worker.join();
        check(workerResult == 0, "worker thread accessed dropdown widgets");
        check(ntf_refresh_font_dropdown("Noto Sans") == 1, "repair matched the wrong set of labels");
        check(selected.text() == text, "repair changed the label text");
        check(snapshot(selected) == correct, "repair did not restore the selected font");
        check(snapshot(unrelated) == unrelatedBefore, "repair touched an unrelated label");
        check(snapshot(popup) == popupBefore, "repair touched a popup row");
        check(snapshot(otherFamily) == otherCorrect, "repair changed another font");
    }
    // A load completion for an old selection must not reset the new selection.
    selected.setText("<font face=\"Vollkorn\">Vollkorn</font>");
    const QImage newSelection = snapshot(selected);
    check(ntf_refresh_font_dropdown("Noto Sans") == 0, "old completion matched a new selection");
    check(snapshot(selected) == newSelection, "old completion changed the new selection");

    selected.setText(text);
    dropdown.show();
    app.processEvents();
    for (int cycle = 0; cycle < 2; ++cycle) {
        check(select_font(selected, text) == 1, "selection did not hold its label");
        check(ntf_set_font_dropdown_text(&popup, text, set_text) == 0, "popup label was held");
        check(ntf_set_font_dropdown_text(&unrelated, text, set_text) == 0, "unrelated label was held");
        check(QFontDatabase::removeApplicationFont(id), "held font removal failed");
        check(snapshot(selected) == correct, "font removal flashed a fallback during the hold");
        check(selected.text() == text, "hold changed the label's HTML");
        check(ntf_set_font_dropdown_text(&selected, text, set_text) == 0, "identical text replaced the hold");
        check(snapshot(selected) == correct, "identical text recaptured the missing font");
        int workerResult = -1;
        std::thread worker([&] {
            workerResult = ntf_set_font_dropdown_text(&selected, text,
                [](QLabel *, const QString &) { ++setterCalls; });
        });
        worker.join();
        check(workerResult == 0, "worker thread held a label");
        id = QFontDatabase::addApplicationFont(argv[2]);
        check(id >= 0, "held font reload failed");
        check(ntf_refresh_font_dropdown("Noto Sans") == 1, "held label was not refreshed");
        check(snapshot(selected) == correct, "completion changed the held preview pixels");
        // The filter must be gone: a subsequent removal can reproduce the original bug.
        check(QFontDatabase::removeApplicationFont(id), "post-hold removal failed");
        check((snapshot(selected) != correct) == ng, "completion left the preview frozen");
        id = QFontDatabase::addApplicationFont(argv[2]);
        check(id >= 0, "post-hold reload failed");
        ntf_refresh_font_dropdown("Noto Sans");
    }
    check(select_font(selected, text) == 1, "selection hold failed");
    selected.setText("<font face=\"Vollkorn\">Vollkorn</font>");
    check(snapshot(selected) == newSelection, "hold hid a newer selection");
    select_font(selected, text);
    selected.hide();
    selected.show();
    check(QFontDatabase::removeApplicationFont(id), "hide test removal failed");
    check((snapshot(selected) != correct) == ng, "hide left the preview frozen");
    id = QFontDatabase::addApplicationFont(argv[2]);
    check(id >= 0, "hide test reload failed");
    ntf_refresh_font_dropdown("Noto Sans");
    QLabel *temporary = new QLabel(&dropdown);
    temporary->resize(500, 80);
    temporary->show();
    check(select_font(*temporary, text) == 1, "temporary label was not held");
    delete temporary;
    check(ntf_refresh_font_dropdown("Noto Sans") == 1, "destroyed hold changed the matching labels");
    qDebug("PASS: recovery and flash prevention, two cycles each, %s shaper", ng ? "NG" : "stock");
    return 0;
}

#include "font_dropdown_test.moc"
