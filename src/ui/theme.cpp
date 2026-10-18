#include "ui/theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace swiftdm::ui {

void apply_dark_theme(QApplication& app) {
    if (QStyleFactory::keys().contains("Fusion")) {
        app.setStyle(QStyleFactory::create("Fusion"));
    }

    QPalette p;
    p.setColor(QPalette::Window, QColor(0x2b, 0x2b, 0x2b));
    p.setColor(QPalette::WindowText, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::Base, QColor(0x24, 0x24, 0x24));
    p.setColor(QPalette::AlternateBase, QColor(0x2b, 0x2b, 0x2b));
    p.setColor(QPalette::ToolTipBase, QColor(0x1e, 0x1e, 0x1e));
    p.setColor(QPalette::ToolTipText, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::Text, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::Button, QColor(0x38, 0x38, 0x38));
    p.setColor(QPalette::ButtonText, QColor(0xe8, 0xe8, 0xe8));
    p.setColor(QPalette::BrightText, QColor(0xff, 0x5a, 0x5a));
    p.setColor(QPalette::Link, QColor(0x4d, 0xa3, 0xff));
    p.setColor(QPalette::Highlight, QColor(0x2f, 0x6f, 0xb5));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8a, 0x8a));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x7c, 0x7c, 0x7c));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x7c, 0x7c, 0x7c));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x7c, 0x7c, 0x7c));
    app.setPalette(p);
}

}  // namespace swiftdm::ui
