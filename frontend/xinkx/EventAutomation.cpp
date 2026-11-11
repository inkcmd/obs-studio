#include "EventAutomation.hpp"

#include "StudioAutomation.hpp"
#include "StreamAccounts.hpp"
#include "StudioWorkspace.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QUuid>

namespace xinkx {

namespace {

const QStringList allowedCommands{QStringLiteral("scene.activate"), QStringLiteral("effect.trigger"),
	QStringLiteral("timer.start"), QStringLiteral("timer.pause"), QStringLiteral("timer.hide"),
	QStringLiteral("source.visible")};

QString ruleLabel(const QJsonObject &rule)
{
	const auto action = rule.value(QStringLiteral("action")).toObject();
	return QStringLiteral("%1 %2 / %3 → %4")
		.arg(rule.value(QStringLiteral("enabled")).toBool(true) ? QStringLiteral("●") : QStringLiteral("○"),
			rule.value(QStringLiteral("platform")).toString(QStringLiteral("*")),
			rule.value(QStringLiteral("event")).toString(),
			action.value(QStringLiteral("command")).toString());
}

} // namespace

EventAutomation::EventAutomation(OBSBasic *mainWindow) : QObject(mainWindow), window(mainWindow)
{
	setObjectName(QStringLiteral("xinkxEventAutomation"));
	const QString settings = studioSettingsPath();
	if (!settings.isEmpty()) path = QFileInfo(settings).absoluteDir().filePath(QStringLiteral("event-rules.json"));
	QFile file(path);
	if (!path.isEmpty() && file.open(QIODevice::ReadOnly)) {
		const auto data = QJsonDocument::fromJson(file.readAll());
		if (data.isArray()) {
			QString error;
			if (data.array().size() <= 100) {
				bool valid = true;
				for (const auto &entry : data.array())
					if (!entry.isObject() || !validRule(entry.toObject(), &error)) valid = false;
				if (valid) savedRules = data.array();
			}
		}
	}
	if (auto *core = mainWindow->findChild<EventCore *>(QStringLiteral("xinkxEventCore")))
		connect(core, &EventCore::delivered, this, &EventAutomation::onEvent);
}

bool EventAutomation::validRule(const QJsonObject &rule, QString *error)
{
	const QString event = rule.value(QStringLiteral("event")).toString();
	const QString platform = rule.value(QStringLiteral("platform")).toString();
	const QJsonObject action = rule.value(QStringLiteral("action")).toObject();
	const QString command = action.value(QStringLiteral("command")).toString();
	const int cooldown = rule.value(QStringLiteral("cooldownMs")).toInt();
	const bool valid = !rule.value(QStringLiteral("id")).toString().isEmpty() &&
		rule.value(QStringLiteral("name")).toString().size() <= 100 &&
		(QStringList{QStringLiteral("chat"), QStringLiteral("follow"), QStringLiteral("sub"),
			QStringLiteral("donation"), QStringLiteral("raid")}).contains(event) &&
		(platform == QStringLiteral("*") ||
		 (QStringList{QStringLiteral("twitch"), QStringLiteral("youtube"), QStringLiteral("kick"),
			QStringLiteral("donationalerts"), QStringLiteral("custom")}).contains(platform)) &&
		allowedCommands.contains(command) && cooldown >= 0 && cooldown <= 3600000 &&
		rule.value(QStringLiteral("contains")).toString().size() <= 120 &&
		rule.value(QStringLiteral("enabled")).isBool();
	if (!valid && error) *error = QStringLiteral("Недопустимое условие, команда или интервал правила");
	return valid;
}

bool EventAutomation::replaceRules(const QJsonArray &rules, QString *error)
{
	if (rules.size() > 100) { if (error) *error = QStringLiteral("Максимум 100 правил"); return false; }
	for (const auto &entry : rules) {
		if (!entry.isObject() || !validRule(entry.toObject(), error)) return false;
	}
	if (path.isEmpty()) { if (error) *error = QStringLiteral("Каталог конфигурации недоступен"); return false; }
	QSaveFile file(path);
	const QByteArray contents = QJsonDocument(rules).toJson(QJsonDocument::Indented);
	if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
		if (error) *error = QStringLiteral("Не удалось сохранить правила");
		return false;
	}
	savedRules = rules;
	nextAllowed.clear();
	emit rulesChanged();
	return true;
}

void EventAutomation::onEvent(const StudioEvent &event)
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (const auto &entry : savedRules) {
		const auto rule = entry.toObject();
		if (!rule.value(QStringLiteral("enabled")).toBool()) continue;
		if (rule.value(QStringLiteral("event")).toString() != event.type) continue;
		const QString platform = rule.value(QStringLiteral("platform")).toString();
		if (platform != QStringLiteral("*") && platform.compare(event.platform, Qt::CaseInsensitive) != 0) continue;
		const QString needle = rule.value(QStringLiteral("contains")).toString();
		if (!needle.isEmpty() && !event.message.contains(needle, Qt::CaseInsensitive)) continue;
		const QString id = rule.value(QStringLiteral("id")).toString();
		if (nextAllowed.value(id) > now) continue;
		const auto command = rule.value(QStringLiteral("action")).toObject();
		if (!allowedCommands.contains(command.value(QStringLiteral("command")).toString())) continue;
		nextAllowed[id] = now + rule.value(QStringLiteral("cooldownMs")).toInt();
		if (auto *engine = window->findChild<StudioAutomation *>(QStringLiteral("xinkxStudioAutomation"))) {
			const auto result = engine->execute(command);
			emit executed(rule.value(QStringLiteral("name")).toString(),
				result.value(QStringLiteral("message")).toString(), result.value(QStringLiteral("ok")).toBool());
		}
	}
	if (nextAllowed.size() > 1024) {
		for (auto it = nextAllowed.begin(); it != nextAllowed.end();) {
			if (it.value() <= now) it = nextAllowed.erase(it);
			else ++it;
		}
	}
}

void installEventAutomationWorkspace(OBSBasic *window)
{
	auto *engine = new EventAutomation(window);
	auto *tabs = studioTabs(window);
	if (!tabs) return;
	auto *page = new QWidget(tabs);
	auto *layout = new QVBoxLayout(page);
	auto *hint = new QLabel(QStringLiteral(
		"Реальное событие → разрешённая команда OBS. Правила работают локально. "
		"Нет произвольного кода, скриптов, отправки ключей или автоматического запуска эфира."), page);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *list = new QListWidget(page);
	layout->addWidget(list);
	const auto refresh = [=] {
		list->clear();
		for (const auto &entry : engine->rules()) list->addItem(ruleLabel(entry.toObject()));
	};
	refresh();
	auto *add = new QPushButton(QStringLiteral("Добавить правило"), page);
	auto *remove = new QPushButton(QStringLiteral("Удалить выбранное"), page);
	layout->addWidget(add);
	layout->addWidget(remove);
	QObject::connect(add, &QPushButton::clicked, page, [=] {
		QDialog dialog(page);
		dialog.setWindowTitle(QStringLiteral("Новое правило"));
		auto *form = new QFormLayout(&dialog);
		auto *name = new QLineEdit(QStringLiteral("Новое правило"), &dialog);
		auto *platform = new QComboBox(&dialog);
		for (const auto &item : {"*", "twitch", "youtube", "kick", "donationalerts", "custom"})
			platform->addItem(QString::fromUtf8(item));
		auto *event = new QComboBox(&dialog);
		for (const auto &item : {"chat", "follow", "sub", "donation", "raid"}) event->addItem(QString::fromUtf8(item));
		auto *contains = new QLineEdit(&dialog);
		contains->setPlaceholderText(QStringLiteral("Необязательно; текст события"));
		auto *action = new QComboBox(&dialog);
		for (const auto &command : allowedCommands) action->addItem(command);
		auto *value = new QLineEdit(&dialog);
		value->setPlaceholderText(QStringLiteral("Имя сцены / источник / gg / boom"));
		auto *seconds = new QSpinBox(&dialog);
		seconds->setRange(1, 3600);
		seconds->setValue(60);
		auto *cooldown = new QSpinBox(&dialog);
		cooldown->setRange(0, 3600);
		cooldown->setValue(15);
		cooldown->setSuffix(QStringLiteral(" с"));
		form->addRow(QStringLiteral("Название"), name);
		form->addRow(QStringLiteral("Платформа"), platform);
		form->addRow(QStringLiteral("Событие"), event);
		form->addRow(QStringLiteral("Содержит текст"), contains);
		form->addRow(QStringLiteral("Команда"), action);
		form->addRow(QStringLiteral("Аргумент"), value);
		form->addRow(QStringLiteral("Таймер (сек)"), seconds);
		form->addRow(QStringLiteral("Задержка повторов"), cooldown);
		auto *save = new QPushButton(QStringLiteral("Сохранить правило"), &dialog);
		form->addRow(save);
		QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
			QJsonObject command{{QStringLiteral("command"), action->currentText()}};
			if (action->currentText() == QStringLiteral("effect.trigger"))
				command.insert(QStringLiteral("effect"), value->text());
			else if (action->currentText() == QStringLiteral("timer.start"))
				command.insert(QStringLiteral("seconds"), seconds->value());
			else command.insert(QStringLiteral("name"), value->text());
			if (action->currentText() == QStringLiteral("source.visible"))
				command.insert(QStringLiteral("visible"), true);
			QJsonObject rule{{QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
				{QStringLiteral("name"), name->text().left(100)},
				{QStringLiteral("platform"), platform->currentText()},
				{QStringLiteral("event"), event->currentText()},
				{QStringLiteral("contains"), contains->text()},
				{QStringLiteral("cooldownMs"), cooldown->value() * 1000},
				{QStringLiteral("enabled"), true}, {QStringLiteral("action"), command}};
			auto rules = engine->rules();
			rules.append(rule);
			QString error;
			if (engine->replaceRules(rules, &error)) dialog.accept();
			else QMessageBox::warning(&dialog, QStringLiteral("Правило"), error);
		});
		if (dialog.exec() == QDialog::Accepted) refresh();
	});
	QObject::connect(remove, &QPushButton::clicked, page, [=] {
		const int row = list->currentRow();
		if (row < 0) return;
		auto rules = engine->rules();
		rules.removeAt(row);
		if (engine->replaceRules(rules)) refresh();
	});
	auto *result = new QLabel(page);
	result->setWordWrap(true);
	layout->addWidget(result);
	QObject::connect(engine, &EventAutomation::executed, result,
		[result](const QString &name, const QString &message, bool success) {
			result->setText(QStringLiteral("%1 · %2: %3")
				.arg(success ? QStringLiteral("OK") : QStringLiteral("Ошибка"), name, message));
		});
	tabs->addTab(page, QStringLiteral("Правила"));
}

} // namespace xinkx
