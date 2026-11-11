#include "EngineRegistry.hpp"

#include "AlertEngine.hpp"
#include "ChatGateway.hpp"
#include "DonationGateway.hpp"
#include "EventAutomation.hpp"
#include "EventCore.hpp"
#include "LocalAssistants.hpp"
#include "MultiStreamManager.hpp"
#include "OverlayRuntime.hpp"
#include "StudioWorkspace.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs.h>

#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace xinkx {

EngineRegistry::EngineRegistry(OBSBasic *window) : QObject(window)
{
	setObjectName(QStringLiteral("xinkxEngineRegistry"));
}

void EngineRegistry::registerModule(const QString &id, const QString &name,
				    const QStringList &capabilities, QObject *object)
{
	if (!object || id.isEmpty()) return;
	for (const auto &module : modules)
		if (module.id == id) return;
	modules.push_back({id, name, capabilities, object});
}

QJsonArray EngineRegistry::inventory() const
{
	QJsonArray entries;
	for (const auto &module : modules) {
		QJsonArray capabilities;
		for (const auto &capability : module.capabilities) capabilities.append(capability);
		entries.append(QJsonObject{{QStringLiteral("id"), module.id},
			{QStringLiteral("name"), module.name},
			{QStringLiteral("available"), !module.object.isNull()},
			{QStringLiteral("capabilities"), capabilities}});
	}
	return entries;
}

void installEngineHub(OBSBasic *window)
{
	auto *registry = new EngineRegistry(window);
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
	registry->registerModule(QStringLiteral("automation"), QStringLiteral("Automation"),
		{QStringLiteral("rules"), QStringLiteral("safe-commands")},
		window->findChild<EventAutomation *>(QStringLiteral("xinkxEventAutomation")));
	registry->registerModule(QStringLiteral("overlay"), QStringLiteral("Scene Presentation"),
		{QStringLiteral("local-sse"), QStringLiteral("obs-sources")},
		window->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime")));
	registry->registerModule(QStringLiteral("stream"), QStringLiteral("libobs Outputs"),
		{QStringLiteral("rtmp"), QStringLiteral("rtmps"), QStringLiteral("portrait")},
		window->findChild<MultiStreamManager *>());
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
			badges.at(3)->setText(QStringLiteral("%1 подключённых аккаунтов в конфигурации\n"
				"Проверяй состояние соединений на вкладке Чат")
				.arg(chat->chatAccounts().size()));
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
	poll->start();
	tabs->insertTab(0, page, QStringLiteral("Обзор"));
}

} // namespace xinkx
