#pragma once

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QEvent>
#include <QObject>
#include <QWidget>

namespace xinkx {

// Scrolling a long settings panel must never silently change an unrelated field.
class WheelFocusGuard final : public QObject {
public:
	explicit WheelFocusGuard(QObject *parent) : QObject(parent) {}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::Wheel) {
			const auto *widget = qobject_cast<QWidget *>(watched);
			if (widget && !widget->hasFocus()) {
				event->ignore();
				return true;
			}
		}
		return QObject::eventFilter(watched, event);
	}
};

inline void guardWheelFields(QWidget *root)
{
	auto *guard = new WheelFocusGuard(root);
	for (auto *field : root->findChildren<QAbstractSpinBox *>()) {
		field->setFocusPolicy(Qt::ClickFocus);
		field->installEventFilter(guard);
	}
	for (auto *field : root->findChildren<QComboBox *>()) {
		field->setFocusPolicy(Qt::ClickFocus);
		field->installEventFilter(guard);
	}
}

} // namespace xinkx
