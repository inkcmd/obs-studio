#include "StudioState.hpp"

#include "AlertEngine.hpp"
#include "ChatGateway.hpp"
#include "EngineRegistry.hpp"
#include "EventCore.hpp"
#include "LocalAssistants.hpp"
#include "ModerationEngine.hpp"
#include "MultiStreamManager.hpp"
#include "OverlayRuntime.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs.h>

#include <QJsonArray>
#include <QThread>

namespace xinkx {

StudioState::StudioState(OBSBasic *window) : QObject(window), mainWindow(window)
{
	setObjectName(QStringLiteral("xinkxStudioState"));
}

QJsonObject StudioState::snapshot() const
{
	// All existing engines and the OBS frontend are owned by the GUI thread.
	// Refuse cross-thread reads rather than racing their shutdown/destruction.
	if (QThread::currentThread() != thread() || !mainWindow)
		return {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("available"), false}};

	OBSBasic *window = mainWindow.data();
	QJsonObject state{{QStringLiteral("schemaVersion"), 1},
		{QStringLiteral("available"), true}};

	if (auto *registry = window->findChild<EngineRegistry *>(QStringLiteral("xinkxEngineRegistry")))
		state.insert(QStringLiteral("engines"), registry->inventory());
	else
		state.insert(QStringLiteral("engines"), QJsonArray());

	QJsonObject events{{QStringLiteral("available"), false}};
	if (auto *core = window->findChild<EventCore *>(QStringLiteral("xinkxEventCore"))) {
		const EventMetrics metrics = core->metrics();
		events = {{QStringLiteral("available"), true},
			{QStringLiteral("accepted"), static_cast<qint64>(metrics.accepted)},
			{QStringLiteral("duplicates"), static_cast<qint64>(metrics.duplicates)},
			{QStringLiteral("dropped"), static_cast<qint64>(metrics.overflow)},
			{QStringLiteral("queued"), metrics.queued}};
	}
	state.insert(QStringLiteral("events"), events);

	QJsonObject alerts{{QStringLiteral("available"), false}};
	if (auto *engine = window->findChild<AlertEngine *>(QStringLiteral("xinkxAlertEngine"))) {
		alerts = {{QStringLiteral("available"), true},
			{QStringLiteral("free"), static_cast<qint64>(engine->freeAlerts())},
			{QStringLiteral("paid"), static_cast<qint64>(engine->paidAlerts())}};
		if (auto *overlay = window->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime")))
			alerts.insert(QStringLiteral("sourceHealth"), overlay->alertSourceHealth(window));
	}
	state.insert(QStringLiteral("alerts"), alerts);

	QJsonObject stream{{QStringLiteral("available"), false}};
	if (auto *outputs = window->findChild<MultiStreamManager *>())
		stream = {{QStringLiteral("available"), true},
			{QStringLiteral("active"), outputs->activeCount()},
			{QStringLiteral("configured"), outputs->configuredCount()}};
	state.insert(QStringLiteral("stream"), stream);

	QJsonObject chat{{QStringLiteral("available"), false}};
	if (auto *gateway = window->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway")))
		chat = {{QStringLiteral("available"), true},
			{QStringLiteral("configuredAccounts"), static_cast<int>(gateway->chatAccounts().size())}};
	state.insert(QStringLiteral("chat"), chat);

	QJsonObject moderation{{QStringLiteral("available"), false}};
	if (auto *filter = window->findChild<ModerationEngine *>(QStringLiteral("xinkxModerationEngine")))
		moderation = {{QStringLiteral("available"), true},
			{QStringLiteral("enabled"), filter->configuration().enabled},
			{QStringLiteral("suppressed"), static_cast<qint64>(filter->counters().suppressed())}};
	state.insert(QStringLiteral("moderation"), moderation);

	state.insert(QStringLiteral("shapes"), QJsonObject{{QStringLiteral("registered"),
		obs_get_latest_input_type_id("xinkx_shape") != nullptr}});
	state.insert(QStringLiteral("ai"), QJsonObject{{QStringLiteral("available"),
		window->findChild<LocalLLM *>(QStringLiteral("xinkxLocalLLM")) != nullptr}});
	state.insert(QStringLiteral("speech"), QJsonObject{{QStringLiteral("available"),
		window->findChild<SpeechEngine *>(QStringLiteral("xinkxSpeechEngine")) != nullptr}});
	return state;
}

} // namespace xinkx
