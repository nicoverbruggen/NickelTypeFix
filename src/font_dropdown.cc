#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QEvent>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <memory>

#include "font_dropdown.h"

namespace {
bool selected_label(QLabel *label) {
    if (!label || label->inherits("RegularElidableLabel")) return false;
    for (QObject *parent = label->parent(); parent; parent = parent->parent())
        if (parent->objectName() == QStringLiteral("fontFaceComboBox")) return true;
    return false;
}

// Nickel updates the label while the preview font is still registered.
// Keep the new label's pixels through cleanup, without changing its text or font.
class PreviewHold : public QObject {
public:
    explicit PreviewHold(QLabel *label)
        : QObject(label), label_(label), text_(label->text()), pixels_(label->size()) {
        if (pixels_.isNull()) return;
        pixels_.fill(Qt::transparent);
        label->render(&pixels_);
        label->installEventFilter(this);
    }

    QLabel *label() const { return label_; }
    bool valid() const { return !pixels_.isNull(); }

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Hide || event->type() == QEvent::Resize ||
            event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange ||
            event->type() == QEvent::StyleChange) {
            pixels_ = QPixmap();
        }
        if (event->type() != QEvent::Paint || pixels_.isNull() ||
            label_->text() != text_ || label_->size() != pixels_.size()) return false;
        QPainter painter(label_);
        painter.drawPixmap(0, 0, pixels_);
        return true;
    }

private:
    QLabel *label_;                 // owns this filter
    QString text_;
    QPixmap pixels_;
};

QList<QPointer<PreviewHold>> holds;

void release_hold(QLabel *label) {
    for (int i = holds.size() - 1; i >= 0; --i) {
        if (holds[i] && holds[i]->label() != label) continue;
        delete holds.takeAt(i).data();
    }
}
} // namespace

int ntf_set_font_dropdown_text(QLabel *label, const QString &text,
                              void (*original)(QLabel *, const QString &)) {
    QPointer<QLabel> changed;
    try {
        if (qApp && QThread::currentThread() == qApp->thread() && selected_label(label) &&
            label->isVisible() && label->text() != text &&
            text.startsWith(QStringLiteral("<font face=\"")))
            changed = label;
    } catch (...) {
        // Inspection must not stop Nickel from setting its text.
    }
    original(label, text);
    if (!changed) return 0;
    try {
        if (changed->text() != text) return 0;
        release_hold(changed);
        std::unique_ptr<PreviewHold> hold(new PreviewHold(changed));
        if (!hold->valid()) return -1;
        holds.append(QPointer<PreviewHold>(hold.get()));
        hold.release();
        return 1;
    } catch (...) {
        return -1;
    }
}

int ntf_refresh_font_dropdown(const QString &family) {
    if (!qApp || QThread::currentThread() != qApp->thread() || family.isEmpty()) return 0;

    // This is the selected-font label's HTML on Nickel 4.x. Matching the complete text
    // keeps a late font change from resetting a label which now names a different family.
    const QString html = QStringLiteral("<font face=\"%1\">%1</font>").arg(family);
    const QString escaped = QStringLiteral("<font face=\"%1\">%1</font>").arg(family.toHtmlEscaped());
    QList<QPointer<QLabel>> labels;
    for (QWidget *widget : QApplication::allWidgets()) {
        QLabel *label = qobject_cast<QLabel *>(widget);
        if (!selected_label(label)) continue;
        if (label->text() != html && label->text() != escaped) continue;
        labels.append(QPointer<QLabel>(label));
    }

    int refreshed = 0;
    for (const QPointer<QLabel> &label : labels) {
        if (!label) continue;
        const QString text = label->text();
        if (text != html && text != escaped) continue;
        release_hold(label);
        // setText with the same string does nothing. Rebuild the rich-text document so its
        // QFont no longer retains the fallback chosen while menu cleanup had the font removed.
        // Both calls run synchronously, so Qt can coalesce their scheduled repaint.
        label->clear();
        if (!label) continue;
        label->setText(text);
        ++refreshed;
    }
    return refreshed;
}
