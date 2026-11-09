#pragma once

#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QStyleOption>
#include <QStylePainter>
#include <QVBoxLayout>
#include <QWidget>

namespace swiftdm::ui {

// Spacing between the labeled sections of a dialog.
constexpr int kSectionSpacing = 8;

// A group box with a bold title and normal-weight content. The dialogs that
// group their fields into sections share this widget so the headings look the
// same everywhere.
class SectionBox : public QGroupBox {
  public:
    explicit SectionBox(const QString& title, QWidget* parent = nullptr)
        : QGroupBox(title, parent) {}

  protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionGroupBox option;
        initStyleOption(&option);
        QFont title_font = font();
        title_font.setBold(true);
        // The title rectangle is sized from the option metrics, while the text
        // is drawn with the painter font; both must use the bold title font.
        option.fontMetrics = QFontMetrics(title_font);
        QStylePainter painter(this);
        painter.setFont(title_font);
        painter.drawComplexControl(QStyle::CC_GroupBox, option);
    }
};

// Adds an empty section and returns the layout of its body.
inline QVBoxLayout* add_section(QVBoxLayout* page, QWidget* parent,
                                const QString& title) {
    auto* box = new SectionBox(title, parent);
    auto* body = new QVBoxLayout(box);
    body->setContentsMargins(10, 4, 10, 8);
    body->setSpacing(6);
    page->addWidget(box);
    return body;
}

// Adds a label/field section with the shared alignment and spacing.
inline QFormLayout* add_form_section(QVBoxLayout* page, QWidget* parent,
                                     const QString& title) {
    auto* box = new SectionBox(title, parent);
    auto* form = new QFormLayout(box);
    form->setContentsMargins(10, 4, 10, 8);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(6);
    page->addWidget(box);
    return form;
}

// Adds a read-only value row to a form section and returns the label so the
// caller can set an object name (the offscreen tests look the fields up by it).
inline QLabel* add_value_row(QFormLayout* form, const QString& label, const QString& value,
                             const char* object_name) {
    auto* widget = new QLabel(value, form->parentWidget());
    widget->setObjectName(QString::fromLatin1(object_name));
    widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    widget->setWordWrap(false);
    form->addRow(label, widget);
    return widget;
}

// Variant that creates the value label empty and hands it back through
// `value`, for the fields a window keeps updating in place.
inline QLabel* add_value_row(QFormLayout* form, const QString& label, QLabel*& value) {
    value = new QLabel(form->parentWidget());
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(label, value);
    return value;
}

// Adds a read-only line edit row (selectable text, tooltip carrying the full
// value when the field is too narrow to show it).
inline QLineEdit* add_read_only_row(QFormLayout* form, const QString& label,
                                    const QString& value, const char* object_name) {
    auto* edit = new QLineEdit(value, form->parentWidget());
    edit->setObjectName(QString::fromLatin1(object_name));
    edit->setReadOnly(true);
    edit->setCursorPosition(0);
    if (!value.isEmpty()) edit->setToolTip(value);
    form->addRow(label, edit);
    return edit;
}

}  // namespace swiftdm::ui
