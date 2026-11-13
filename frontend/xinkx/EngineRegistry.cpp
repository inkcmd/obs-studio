#include "EngineRegistry.hpp"
#include "EngineActionForm.hpp"

#include "AlertEngine.hpp"
#include "ChatGateway.hpp"
#include "DonationGateway.hpp"
#include "EventAutomation.hpp"
#include "EventCore.hpp"
#include "ModerationEngine.hpp"
#include "LocalAssistants.hpp"
#include "MarkerEngine.hpp"
#include "StudioAutomation.hpp"
#include "MultiStreamManager.hpp"
#include "OverlayRuntime.hpp"
#include "StudioWorkspace.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs.h>

#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QSet>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace xinkx {

namespace {

QJsonObject failure(const QString &message)
{
	return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), message}};
}

// Wrap the existing, audited OBS command surface. Only a small subset can be
// triggered from incoming viewer events; manual-only commands stay manual.
class StudioCommandsExtension final : public EngineExtension {
public:
	explicit StudioCommandsExtension(OBSBasic *window, QObject *parent)
		: EngineExtension(parent), main(window) {}

	QString moduleId() const override { return QStringLiteral("obs"); }
	QString moduleTitle() const override { return QStringLiteral("OBS Actions"); }
	QStringList capabilities() const override
	{
		return {QStringLiteral("scenes"), QStringLiteral("sources"),
			QStringLiteral("reactions"), QStringLiteral("timer")};
	}
	QList<EngineAction> actions() const override
	{
		return {
			{QStringLiteral("scene.create"), QStringLiteral("Создать сцену"), false,
				{}, {}, {}, {{QStringLiteral("name"), QStringLiteral("Имя сцены"),
					QStringLiteral("text"), true, 0, 100}}},
			{QStringLiteral("scene.activate"), QStringLiteral("Активировать сцену"), true,
				QStringLiteral("name"), QStringLiteral("text"), {},
				{{QStringLiteral("name"), QStringLiteral("Имя сцены"),
					QStringLiteral("text"), true, 0, 100}}},
			{QStringLiteral("overlay.add"), QStringLiteral("Добавить оверлей"), false,
				{}, {}, QJsonObject{{QStringLiteral("layer"), QStringLiteral("chat")}},
				{{QStringLiteral("layer"), QStringLiteral("Тип слоя"), QStringLiteral("choice"), true,
					0, 0, {QStringLiteral("chat"), QStringLiteral("alerts"), QStringLiteral("effects"),
						QStringLiteral("timer"), QStringLiteral("viewers"), QStringLiteral("frame"),
						QStringLiteral("all")}}}},
			{QStringLiteral("source.text.add"), QStringLiteral("Добавить текст"), false,
				{}, {}, {}, {{QStringLiteral("name"), QStringLiteral("Имя источника"),
					QStringLiteral("text"), true, 0, 100},
					{QStringLiteral("text"), QStringLiteral("Содержимое"),
					QStringLiteral("text"), true, 0, 4096}}},
			{QStringLiteral("source.visible"), QStringLiteral("Показать источник"), true,
				QStringLiteral("name"), QStringLiteral("text"),
				QJsonObject{{QStringLiteral("visible"), true}},
				{{QStringLiteral("name"), QStringLiteral("Имя источника"),
					QStringLiteral("text"), true, 0, 100},
					{QStringLiteral("visible"), QStringLiteral("Виден"),
					QStringLiteral("boolean")}}},
			{QStringLiteral("effect.trigger"), QStringLiteral("Эффект"), true,
				QStringLiteral("effect"), QStringLiteral("text"), {},
				{{QStringLiteral("effect"), QStringLiteral("Эффект"),
					QStringLiteral("choice"), true, 0, 0,
					{QStringLiteral("gg"), QStringLiteral("boom"),
						QStringLiteral("low-hp"), QStringLiteral("confetti")}}}},
			{QStringLiteral("timer.start"), QStringLiteral("Запустить таймер"), true,
				QStringLiteral("seconds"), QStringLiteral("seconds"), {},
				{{QStringLiteral("seconds"), QStringLiteral("Секунды"),
					QStringLiteral("seconds"), true, 1, 3600}}},
			{QStringLiteral("timer.pause"), QStringLiteral("Приостановить таймер"), true},
			{QStringLiteral("timer.hide"), QStringLiteral("Скрыть таймер"), true},
		};
	}

	QJsonObject execute(const QString &, const QJsonObject &arguments) override
	{
		if (!main) return failure(QStringLiteral("Окно OBS недоступно"));
		auto *automation = main->findChild<StudioAutomation *>(QStringLiteral("xinkxStudioAutomation"));
		return automation ? automation->execute(arguments)
			: failure(QStringLiteral("OBS Actions недоступен"));
	}
private:
	QPointer<OBSBasic> main;
};

} // namespace

EngineRegistry::EngineRegistry(OBSBasic *window) : QObject(window)
{
	setObjectName(QStringLiteral("xinkxEngineRegistry"));
	if (auto *core = window->findChild<EventCore *>(QStringLiteral("xinkxEventCore")))
		connect(core, &EventCore::delivered, this, &EngineRegistry::deliverToExtensions);
}

EngineRegistry::~EngineRegistry()
{
	shutdownExtensions();
}

void EngineRegistry::shutdownExtensions()
{
	if (shuttingDown) return;
	shuttingDown = true;
	for (auto it = extensions.crbegin(); it != extensions.crend(); ++it) {
		if (*it && runningExtensions.contains((*it)->moduleId())) (*it)->stop();
	}
	runningExtensions.clear();
}

void EngineRegistry::registerModule(const QString &id, const QString &name,
				    const QStringList &capabilities, QObject *object)
{
	if (!object || id.isEmpty()) return;
	for (const auto &module : modules)
		if (module.id == id) return;
	modules.push_back({id, name, capabilities, object});
	emit catalogChanged();
}

bool EngineRegistry::registerExtension(EngineExtension *extension, QString *error)
{
	static const QRegularExpression idFormat(QStringLiteral("^[a-z][a-z0-9_-]{1,23}$"));
	static const QRegularExpression actionFormat(
		QStringLiteral("^[a-z][a-z0-9_-]{1,23}\\.[a-z][a-z0-9_.-]{0,39}$"));
	const auto reject = [error](const QString &reason) {
		if (error) *error = reason;
		return false;
	};
	if (shuttingDown) return reject(QStringLiteral("Реестр движков уже останавливается"));
	if (!extension || extension->thread() != thread())
		return reject(QStringLiteral("Движок должен выполняться в UI-потоке"));
	const QString id = extension->moduleId();
	if (!idFormat.match(id).hasMatch())
		return reject(QStringLiteral("Некорректный ID движка"));
	if (extensions.size() >= 64 || extension->actions().size() > 32)
		return reject(QStringLiteral("Превышен лимит движков или команд"));
	for (const auto &entry : modules)
		if (entry.id == id) return reject(QStringLiteral("ID движка уже используется"));
	for (const auto &item : extensions)
		if (item == extension) return reject(QStringLiteral("Движок уже зарегистрирован"));
	QSet<QString> ownActions;
	for (const auto &action : extension->actions()) {
		if (!actionFormat.match(action.id).hasMatch() || action.title.trimmed().isEmpty() ||
		    action.title.size() > 100 ||
		    (!action.argumentType.isEmpty() && action.argumentType != QStringLiteral("text") &&
		     action.argumentType != QStringLiteral("seconds")) ||
		    (!action.argumentType.isEmpty() && action.argumentKey.isEmpty()) ||
		    action.argumentKey.size() > 40 ||
		    QJsonDocument(action.defaults).toJson(QJsonDocument::Compact).size() > 1024)
			return reject(QStringLiteral("Некорректная команда движка"));
		// Existing OBS Actions have historical names (scene.*, timer.*, etc.).
		if (id != QStringLiteral("obs") && !action.id.startsWith(id + QLatin1Char('.')))
			return reject(QStringLiteral("Команды должны начинаться с ID движка"));
		if (ownActions.contains(action.id)) return reject(QStringLiteral("Команда повторяется"));
		QSet<QString> keys;
		if (action.arguments.size() > 12)
			return reject(QStringLiteral("Слишком много параметров команды"));
		for (const auto &arg : action.arguments) {
			static const QRegularExpression keyFormat(QStringLiteral("^[a-z][a-zA-Z0-9_]{0,39}$"));
			if (!keyFormat.match(arg.key).hasMatch() || keys.contains(arg.key) ||
			    arg.title.size() > 80 || arg.title.trimmed().isEmpty() ||
			    !(QStringList{QStringLiteral("text"), QStringLiteral("seconds"),
				QStringLiteral("boolean"), QStringLiteral("choice")}).contains(arg.type) ||
			    (arg.type == QStringLiteral("choice") &&
				(arg.choices.isEmpty() || arg.choices.size() > 32)) ||
			    (arg.type == QStringLiteral("seconds") && (arg.minimum < 0 || arg.maximum < arg.minimum)) ||
			    (arg.type == QStringLiteral("text") && (arg.maximum < 1 || arg.maximum > 4096)))
				return reject(QStringLiteral("Некорректная схема параметров"));
			keys.insert(arg.key);
		}
		ownActions.insert(action.id);
		for (const auto &previous : extensions) {
			if (!previous) continue;
			for (const auto &used : previous->actions())
				if (used.id == action.id) return reject(QStringLiteral("Команда уже зарегистрирована"));
		}
	}
	QString startError;
	if (!extension->start(&startError)) {
		extension->stop(); // A partially initialized extension must release its resources.
		return reject(startError.isEmpty() ? QStringLiteral("Движок не смог запуститься") : startError);
	}
	extensions.append(extension);
	runningExtensions.insert(id);
	registerModule(id, extension->moduleTitle(), extension->capabilities(), extension);
	return true;
}

void EngineRegistry::deliverToExtensions(const StudioEvent &event)
{
	// EventCore already bounded, deduplicated and delivered on the GUI thread.
	if (shuttingDown) return;
	for (const auto &extension : extensions)
		if (extension && runningExtensions.contains(extension->moduleId()) &&
		    extension->eventTypes().contains(event.type)) extension->onEvent(event);
}

QJsonArray EngineRegistry::actionCatalog() const
{
	QJsonArray result;
	for (const auto &extension : extensions) {
		if (!extension) continue;
		for (const auto &action : extension->actions()) {
			QJsonArray parameters;
			for (const auto &argument : action.arguments) {
				QJsonArray choices;
				for (const auto &choice : argument.choices) choices.append(choice);
				parameters.append(QJsonObject{
					{QStringLiteral("key"), argument.key},
					{QStringLiteral("title"), argument.title},
					{QStringLiteral("type"), argument.type},
					{QStringLiteral("required"), argument.required},
					{QStringLiteral("minimum"), argument.minimum},
					{QStringLiteral("maximum"), argument.maximum},
					{QStringLiteral("choices"), choices}});
			}
			result.append(QJsonObject{{QStringLiteral("id"), action.id},
				{QStringLiteral("title"), action.title},
				{QStringLiteral("engine"), extension->moduleId()},
				{QStringLiteral("automatable"), action.automatable},
				{QStringLiteral("argumentKey"), action.argumentKey},
				{QStringLiteral("argumentType"), action.argumentType},
				{QStringLiteral("defaults"), action.defaults},
				{QStringLiteral("arguments"), parameters}});
		}
	}
	return result;
}

QStringList EngineRegistry::automatableActions() const
{
	QStringList list;
	for (const auto &entry : actionCatalog()) {
		const auto action = entry.toObject();
		if (action.value(QStringLiteral("automatable")).toBool())
			list.append(action.value(QStringLiteral("id")).toString());
	}
	return list;
}

bool EngineRegistry::prepareAction(const QJsonObject &command, bool fromRule,
	EngineExtension **target, QJsonObject *normalized, QString *error) const
{
	const auto reject = [error](const QString &message) {
		if (error) *error = message;
		return false;
	};
	if (QThread::currentThread() != thread())
		return reject(QStringLiteral("Команды должны выполняться в UI-потоке"));
	if (shuttingDown) return reject(QStringLiteral("Движки останавливаются"));
	if (QJsonDocument(command).toJson(QJsonDocument::Compact).size() > 4096)
		return reject(QStringLiteral("Слишком большой аргумент команды"));
	const QString id = command.value(QStringLiteral("command")).toString();
	for (const auto &extension : extensions) {
		if (!extension) continue;
		for (const auto &action : extension->actions()) {
			if (action.id != id) continue;
			if (!runningExtensions.contains(extension->moduleId()))
				return reject(QStringLiteral("Движок не запущен"));
			if (fromRule && !action.automatable)
				return reject(QStringLiteral("Команда запрещена в правилах"));
			QList<EngineArgument> schema = action.arguments;
			// v1 in-process extensions may still expose only one legacy argument.
			if (schema.isEmpty() && !action.argumentKey.isEmpty()) {
				schema.append({action.argumentKey, action.argumentKey, action.argumentType,
					false, action.argumentType == QStringLiteral("seconds") ? 1 : 0,
					action.argumentType == QStringLiteral("seconds") ? 3600 : 4096});
			}
			QJsonObject args = action.defaults;
			args.insert(QStringLiteral("command"), id);
			for (auto it = command.begin(); it != command.end(); ++it) {
				if (it.key() == QStringLiteral("command")) continue;
				bool declared = false;
				for (const auto &argument : schema)
					if (argument.key == it.key()) { declared = true; break; }
				if (!declared) return reject(QStringLiteral("Неизвестный параметр: ") + it.key());
				args.insert(it.key(), it.value());
			}
			for (const auto &argument : schema) {
				const QJsonValue value = args.value(argument.key);
				if (value.isUndefined() || value.isNull()) {
					if (argument.required) return reject(QStringLiteral("Не задан параметр: ") + argument.title);
					continue;
				}
				if (argument.type == QStringLiteral("text")) {
					if (!value.isString() || value.toString().size() > argument.maximum ||
					    (argument.required && value.toString().trimmed().isEmpty()))
						return reject(QStringLiteral("Неверный текстовый параметр: ") + argument.title);
				} else if (argument.type == QStringLiteral("seconds")) {
					const double seconds = value.toDouble(-1);
					if (!value.isDouble() || seconds != static_cast<int>(seconds) ||
					    seconds < argument.minimum || seconds > argument.maximum)
						return reject(QStringLiteral("Неверное число: ") + argument.title);
				} else if (argument.type == QStringLiteral("boolean")) {
					if (!value.isBool()) return reject(QStringLiteral("Ожидается true/false: ") + argument.title);
				} else if (argument.type == QStringLiteral("choice")) {
					if (!value.isString() || !argument.choices.contains(value.toString()))
						return reject(QStringLiteral("Недопустимое значение: ") + argument.title);
				}
			}
			if (normalized) *normalized = args;
			if (target) *target = extension;
			return true;
		}
	}
	return reject(QStringLiteral("Команда не зарегистрирована"));
}

bool EngineRegistry::validateAction(const QJsonObject &command, bool fromRule, QString *error) const
{
	return prepareAction(command, fromRule, nullptr, nullptr, error);
}

QJsonObject EngineRegistry::executeAction(const QJsonObject &command, bool fromRule)
{
	EngineExtension *target = nullptr;
	QJsonObject normalized;
	QString error;
	if (!prepareAction(command, fromRule, &target, &normalized, &error)) return failure(error);
	const QJsonObject result = target->execute(command.value(QStringLiteral("command")).toString(), normalized);
	if (!result.value(QStringLiteral("ok")).isBool())
		return failure(QStringLiteral("Движок вернул некорректный результат"));
	return result;
}

QJsonArray EngineRegistry::inventory() const
{
	QJsonArray entries;
	for (const auto &module : modules) {
		QJsonArray capabilities;
		for (const auto &capability : module.capabilities) capabilities.append(capability);
		QJsonObject entry{{QStringLiteral("id"), module.id},
			{QStringLiteral("name"), module.name},
			{QStringLiteral("available"), !module.object.isNull()},
			{QStringLiteral("capabilities"), capabilities}};
		for (const auto &extension : extensions)
			if (extension && extension->moduleId() == module.id) {
				entry.insert(QStringLiteral("health"), extension->health());
				entry.insert(QStringLiteral("lifecycle"),
					runningExtensions.contains(module.id) ? QStringLiteral("running") : QStringLiteral("stopped"));
				entry.insert(QStringLiteral("actions"), static_cast<int>(extension->actions().size()));
				QJsonArray subscriptions;
				for (const auto &type : extension->eventTypes()) subscriptions.append(type);
				entry.insert(QStringLiteral("subscriptions"), subscriptions);
				break;
			}
		entries.append(entry);
	}
	return entries;
}

void installEngineServices(OBSBasic *window)
{
	if (window->findChild<EngineRegistry *>(QStringLiteral("xinkxEngineRegistry"))) return;
	auto *registry = new EngineRegistry(window);
	registry->registerExtension(new StudioCommandsExtension(window, registry));
	registry->registerExtension(new MarkerEngine(registry));
}

void shutdownEngineServices(OBSBasic *window)
{
	if (!window) return;
	if (auto *registry = window->findChild<EngineRegistry *>(QStringLiteral("xinkxEngineRegistry")))
		registry->shutdownExtensions();
}

void installEngineHub(OBSBasic *window)
{
	auto *registry = window->findChild<EngineRegistry *>(QStringLiteral("xinkxEngineRegistry"));
	if (!registry) {
		installEngineServices(window);
		registry = window->findChild<EngineRegistry *>(QStringLiteral("xinkxEngineRegistry"));
	}
	if (!registry) return;
	registry->registerModule(QStringLiteral("events"), QStringLiteral("Event Core"),
		{QStringLiteral("dedup"), QStringLiteral("journal"), QStringLiteral("queued-delivery")},
		window->findChild<EventCore *>(QStringLiteral("xinkxEventCore")));
	registry->registerModule(QStringLiteral("chat"), QStringLiteral("Chat Gateway"),
		{QStringLiteral("twitch"), QStringLiteral("youtube"), QStringLiteral("kick")},
		window->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway")));
	registry->registerModule(QStringLiteral("alerts"), QStringLiteral("Alert Engine"),
		{QStringLiteral("free"), QStringLiteral("paid"), QStringLiteral("reactions")},
		window->findChild<AlertEngine *>(QStringLiteral("xinkxAlertEngine")));
	registry->registerModule(QStringLiteral("donations"), QStringLiteral("Donation Gateway"),
		{QStringLiteral("donationalerts"), QStringLiteral("json-feeds")},
		window->findChild<DonationGateway *>(QStringLiteral("xinkxDonationGateway")));
	registry->registerModule(QStringLiteral("moderation"), QStringLiteral("Local Chat Moderation"),
		{QStringLiteral("phrases"), QStringLiteral("links"), QStringLiteral("repeats"), QStringLiteral("flood")},
		window->findChild<ModerationEngine *>(QStringLiteral("xinkxModerationEngine")));
	registry->registerModule(QStringLiteral("automation"), QStringLiteral("Automation"),
		{QStringLiteral("rules"), QStringLiteral("safe-commands")},
		window->findChild<EventAutomation *>(QStringLiteral("xinkxEventAutomation")));
	registry->registerModule(QStringLiteral("overlay"), QStringLiteral("Scene Presentation"),
		{QStringLiteral("local-sse"), QStringLiteral("obs-sources")},
		window->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime")));
	registry->registerModule(QStringLiteral("stream"), QStringLiteral("libobs Outputs"),
		{QStringLiteral("rtmp"), QStringLiteral("rtmps"), QStringLiteral("portrait")},
		window->findChild<MultiStreamManager *>());
	// RecordingEngine is created lazily when the user opens recording settings;
	// the compiled feature itself is nevertheless present and discoverable.
	registry->registerModule(QStringLiteral("recording"), QStringLiteral("Record Engine"),
		{QStringLiteral("source-jobs"), QStringLiteral("scene-jobs"), QStringLiteral("independent-files")},
		registry);
	if (obs_get_latest_input_type_id("xinkx_shape"))
		registry->registerModule(QStringLiteral("shapes"), QStringLiteral("Native Shapes"),
			{QStringLiteral("obs-source"), QStringLiteral("gpu-texture")}, registry);
	registry->registerModule(QStringLiteral("ai"), QStringLiteral("Local LLM"),
		{QStringLiteral("ollama-local"), QStringLiteral("manual-tools")},
		window->findChild<LocalLLM *>(QStringLiteral("xinkxLocalLLM")));
	registry->registerModule(QStringLiteral("speech"), QStringLiteral("Speech"),
		{QStringLiteral("windows-sapi")},
		window->findChild<SpeechEngine *>(QStringLiteral("xinkxSpeechEngine")));

	auto *tabs = studioTabs(window);
	if (!tabs) return;
	auto *page = new QWidget(tabs);
	auto *layout = new QVBoxLayout(page);
	const auto title = new QLabel(QStringLiteral("XINKX Studio · состояния движков"), page);
	title->setStyleSheet(QStringLiteral("font-weight:600;font-size:14px"));
	layout->addWidget(title);
	auto *caption = new QLabel(QStringLiteral(
		"Один процесс, общие аккаунты и события, один граф сцен libobs. "
		"Показатели относятся к локальному приложению, не к проверке воспроизведения на площадке."), page);
	caption->setWordWrap(true);
	layout->addWidget(caption);
	auto *grid = new QGridLayout();
	const QStringList names{QStringLiteral("События"), QStringLiteral("Уведомления"),
		QStringLiteral("Трансляции"), QStringLiteral("Чат"),
		QStringLiteral("Элементы сцен"), QStringLiteral("LLM / голос")};
	QVector<QLabel *> badges;
	for (int index = 0; index < names.size(); ++index) {
		auto *group = new QGroupBox(names.at(index), page);
		auto *inner = new QVBoxLayout(group);
		auto *badge = new QLabel(group);
		badge->setWordWrap(true);
		badge->setTextInteractionFlags(Qt::TextSelectableByMouse);
		inner->addWidget(badge);
		badges.push_back(badge);
		grid->addWidget(group, index / 2, index % 2);
	}
	layout->addLayout(grid);
	// New engines appear here automatically; this table is driven by the registry,
	// not hardcoded group boxes or a separate engine-specific UI implementation.
	auto *catalog = new QTreeWidget(page);
	catalog->setObjectName(QStringLiteral("xinkxEngineCatalog"));
	catalog->setHeaderLabels({QStringLiteral("Движок"), QStringLiteral("Состояние"),
		QStringLiteral("Возможности"), QStringLiteral("Команды")});
	catalog->setRootIsDecorated(false);
	catalog->setMaximumHeight(240);
	layout->addWidget(catalog);

	// A command panel backed by the same typed action catalog used by rules.
	// New extensions appear here without adding another engine-specific UI.
	auto *commandsPage = new QWidget(tabs);
	auto *commandsPageLayout = new QVBoxLayout(commandsPage);
	auto *commandsGroup = new QGroupBox(QStringLiteral("Команды движков"), commandsPage);
	auto *commandsLayout = new QVBoxLayout(commandsGroup);
	auto *actionSelect = new QComboBox(commandsGroup);
	actionSelect->setObjectName(QStringLiteral("xinkxEngineActionSelect"));
	commandsLayout->addWidget(actionSelect);
	auto *actionForm = new EngineActionForm(commandsGroup);
	commandsLayout->addWidget(actionForm);
	auto *actionButton = new QPushButton(QStringLiteral("Выполнить команду"), commandsGroup);
	commandsLayout->addWidget(actionButton);
	auto *actionResult = new QLabel(commandsGroup);
	actionResult->setWordWrap(true);
	actionResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
	commandsLayout->addWidget(actionResult);
	commandsPageLayout->addWidget(commandsGroup);
	commandsPageLayout->addStretch(1);
	tabs->addTab(commandsPage, QStringLiteral("Действия"));
	const QPointer<EngineRegistry> safeRegistry(registry);
	const auto updateAction = [safeRegistry, actionSelect, actionForm, actionButton] {
		if (!safeRegistry) return;
		const QString id = actionSelect->currentData().toString();
		actionButton->setEnabled(!id.isEmpty());
		for (const auto &value : safeRegistry->actionCatalog()) {
			const auto action = value.toObject();
			if (action.value(QStringLiteral("id")).toString() == id) {
				actionForm->setAction(action);
				return;
			}
		}
		actionForm->setAction({});
	};
	const auto refreshActions = [safeRegistry, actionSelect, updateAction] {
		if (!safeRegistry) return;
		const QString previous = actionSelect->currentData().toString();
		const QSignalBlocker blocked(actionSelect);
		actionSelect->clear();
		for (const auto &value : safeRegistry->actionCatalog()) {
			const auto spec = value.toObject();
			const QString id = spec.value(QStringLiteral("id")).toString();
			actionSelect->addItem(spec.value(QStringLiteral("title")).toString() +
				QStringLiteral(" · ") + id, id);
		}
		const int index = actionSelect->findData(previous);
		if (index >= 0) actionSelect->setCurrentIndex(index);
		updateAction();
	};
	QObject::connect(actionSelect, &QComboBox::currentIndexChanged, commandsGroup,
		[updateAction](int) { updateAction(); });
	QObject::connect(actionButton, &QPushButton::clicked, commandsGroup,
		[safeRegistry, actionSelect, actionForm, actionResult, commandsGroup] {
			if (!safeRegistry) return;
			const auto command = actionForm->command();
			// Manual actions are intentionally not permitted in event automation;
			// require explicit confirmation before mutating the OBS scene.
			for (const auto &entry : safeRegistry->actionCatalog()) {
				const auto spec = entry.toObject();
				if (spec.value(QStringLiteral("id")).toString() == actionSelect->currentData().toString() &&
				    !spec.value(QStringLiteral("automatable")).toBool()) {
					if (QMessageBox::question(commandsGroup, QStringLiteral("Подтверждение"),
						QStringLiteral("Выполнить действие %1?").arg(actionSelect->currentText())) != QMessageBox::Yes)
						return;
					break;
				}
			}
			const auto result = safeRegistry->executeAction(command);
			actionResult->setText(QStringLiteral("%1: %2").arg(
				result.value(QStringLiteral("ok")).toBool() ? QStringLiteral("OK") : QStringLiteral("Ошибка"),
				result.value(QStringLiteral("message")).toString()));
		});
	QObject::connect(registry, &EngineRegistry::catalogChanged, commandsGroup, refreshActions);
	refreshActions();
	const auto refreshCatalog = [safeRegistry, catalog] {
		if (!safeRegistry) return;
		catalog->clear();
		for (const auto &value : safeRegistry->inventory()) {
			const auto item = value.toObject();
			QStringList caps;
			for (const auto &cap : item.value(QStringLiteral("capabilities")).toArray())
				caps << cap.toString();
			const auto health = item.value(QStringLiteral("health")).toObject();
			auto *row = new QTreeWidgetItem(catalog, QStringList{
			item.value(QStringLiteral("name")).toString(),
			!item.value(QStringLiteral("available")).toBool() ? QStringLiteral("Недоступен") :
				health.value(QStringLiteral("status")).toString(QStringLiteral("Готов")),
			caps.join(QStringLiteral(", ")),
			QString::number(item.value(QStringLiteral("actions")).toInt())});
			row->setToolTip(0, item.value(QStringLiteral("id")).toString());
			QStringList commands;
			for (const auto &value : safeRegistry->actionCatalog()) {
				const auto action = value.toObject();
				if (action.value(QStringLiteral("engine")) == item.value(QStringLiteral("id")))
					commands.append(action.value(QStringLiteral("id")).toString());
			}
			row->setToolTip(3, commands.join(QLatin1Char('\n')));
		}
	};
	refreshCatalog();
	layout->addStretch(1);
	const auto refresh = [window, badges] {
		if (auto *events = window->findChild<EventCore *>(QStringLiteral("xinkxEventCore"))) {
			const auto stats = events->metrics();
			badges.at(0)->setText(QStringLiteral("%1 принято · %2 повторов · %3 потерь")
				.arg(stats.accepted).arg(stats.duplicates).arg(stats.overflow));
		}
		if (auto *alerts = window->findChild<AlertEngine *>(QStringLiteral("xinkxAlertEngine"))) {
			QString text = QStringLiteral("%1 бесплатных · %2 платных")
				.arg(alerts->freeAlerts()).arg(alerts->paidAlerts());
			if (auto *runtime = window->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime")))
				text += QStringLiteral("\n") + runtime->alertSourceHealth(window);
			badges.at(1)->setText(text);
		}
		if (auto *outputs = window->findChild<MultiStreamManager *>()) {
			badges.at(2)->setText(QStringLiteral("%1 активных выходов · %2 настроено")
				.arg(outputs->activeCount()).arg(outputs->configuredCount()));
		}
		if (auto *chat = window->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"))) {
			QString status = QStringLiteral("%1 подключённых аккаунтов в конфигурации\n"
				"Проверяй состояние соединений на вкладке Чат")
				.arg(chat->chatAccounts().size());
			if (auto *moderation = window->findChild<ModerationEngine *>(QStringLiteral("xinkxModerationEngine"))) {
				status += moderation->configuration().enabled
					? QStringLiteral("\nЛокальный фильтр: %1 скрыто").arg(moderation->counters().suppressed())
					: QStringLiteral("\nЛокальный фильтр: выключен");
			}
			badges.at(3)->setText(status);
		}
		badges.at(4)->setText(obs_get_latest_input_type_id("xinkx_shape")
			? QStringLiteral("Нативные фигуры и градиенты зарегистрированы в OBS")
			: QStringLiteral("Движок фигур не зарегистрирован"));
		badges.at(5)->setText(QStringLiteral("Ollama: только localhost, по запросу\n"
			"Голос: Windows SAPI, отдельная настройка"));
	};
	refresh();
	auto *poll = new QTimer(page);
	poll->setInterval(2000);
	QObject::connect(poll, &QTimer::timeout, page, refresh);
	QObject::connect(poll, &QTimer::timeout, page, refreshCatalog);
	poll->start();
	tabs->insertTab(0, page, QStringLiteral("Обзор"));
}

} // namespace xinkx
