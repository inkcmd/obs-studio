#include "EngineActionForm.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QJsonValue>
#include <QLineEdit>
#include <QSpinBox>

namespace xinkx {

EngineActionForm::EngineActionForm(QWidget *parent) : QWidget(parent)
{
	form = new QFormLayout(this);
	form->setContentsMargins(0, 0, 0, 0);
}

void EngineActionForm::setAction(const QJsonObject &action)
{
	editors.clear();
	while (form->rowCount() > 0) form->removeRow(0);
	actionSpec = action;
	arguments = action.value(QStringLiteral("arguments")).toArray();
	// Compatibility with v1 extensions that declared a single argument only.
	if (arguments.isEmpty() && !action.value(QStringLiteral("argumentKey")).toString().isEmpty()) {
		arguments.append(QJsonObject{
			{QStringLiteral("key"), action.value(QStringLiteral("argumentKey"))},
			{QStringLiteral("title"), action.value(QStringLiteral("argumentKey"))},
			{QStringLiteral("type"), action.value(QStringLiteral("argumentType"))},
			{QStringLiteral("minimum"), action.value(QStringLiteral("argumentType")).toString() ==
				QStringLiteral("seconds") ? 1 : 0},
			{QStringLiteral("maximum"), action.value(QStringLiteral("argumentType")).toString() ==
				QStringLiteral("seconds") ? 3600 : 4096}});
	}
	const auto defaults = action.value(QStringLiteral("defaults")).toObject();
	for (const auto &item : arguments) {
		const auto parameter = item.toObject();
		const QString key = parameter.value(QStringLiteral("key")).toString();
		const QString type = parameter.value(QStringLiteral("type")).toString();
		const QString title = parameter.value(QStringLiteral("title")).toString(key);
		const auto initial = defaults.value(key);
		QWidget *editor = nullptr;
		if (type == QStringLiteral("seconds")) {
			auto *spin = new QSpinBox(this);
			const int minimum = parameter.value(QStringLiteral("minimum")).toInt(1);
			const int maximum = parameter.value(QStringLiteral("maximum")).toInt(3600);
			spin->setRange(minimum, maximum);
			spin->setValue(initial.isDouble() ? initial.toInt() : minimum);
			editor = spin;
		} else if (type == QStringLiteral("boolean")) {
			auto *check = new QCheckBox(this);
			check->setChecked(initial.toBool(false));
			editor = check;
		} else if (type == QStringLiteral("choice")) {
			auto *combo = new QComboBox(this);
			for (const auto &option : parameter.value(QStringLiteral("choices")).toArray())
				combo->addItem(option.toString());
			const int match = combo->findText(initial.toString());
			if (match >= 0) combo->setCurrentIndex(match);
			editor = combo;
		} else {
			auto *text = new QLineEdit(this);
			text->setMaxLength(parameter.value(QStringLiteral("maximum")).toInt(4096));
			text->setText(initial.toString());
			editor = text;
		}
		form->addRow(title, editor);
		editors.insert(key, editor);
	}
}

QJsonObject EngineActionForm::command() const
{
	QJsonObject command = actionSpec.value(QStringLiteral("defaults")).toObject();
	command.insert(QStringLiteral("command"), actionSpec.value(QStringLiteral("id")));
	for (const auto &item : arguments) {
		const auto parameter = item.toObject();
		const QString key = parameter.value(QStringLiteral("key")).toString();
		const QString type = parameter.value(QStringLiteral("type")).toString();
		QWidget *editor = editors.value(key);
		if (!editor) continue;
		if (type == QStringLiteral("seconds"))
			command.insert(key, static_cast<QSpinBox *>(editor)->value());
		else if (type == QStringLiteral("boolean"))
			command.insert(key, static_cast<QCheckBox *>(editor)->isChecked());
		else if (type == QStringLiteral("choice"))
			command.insert(key, static_cast<QComboBox *>(editor)->currentText());
		else
			command.insert(key, static_cast<QLineEdit *>(editor)->text());
	}
	return command;
}

} // namespace xinkx
