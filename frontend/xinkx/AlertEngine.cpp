#include "AlertEngine.hpp"

#include <widgets/OBSBasic.hpp>

namespace xinkx {

AlertEngine::AlertEngine(OBSBasic *window) : QObject(window)
{
	setObjectName(QStringLiteral("xinkxAlertEngine"));
	if (auto *events = window->findChild<EventCore *>(QStringLiteral("xinkxEventCore")))
		connect(events, &EventCore::delivered, this, &AlertEngine::onEvent);
}

void AlertEngine::onEvent(const StudioEvent &event)
{
	if (event.type == QStringLiteral("chat")) {
		emit chatReady(event);
		return;
	}
	if (event.type != QStringLiteral("follow") && event.type != QStringLiteral("sub") &&
	    event.type != QStringLiteral("donation") && event.type != QStringLiteral("raid")) return;
	if (event.paid || event.type == QStringLiteral("donation"))
		++paidCount;
	else ++freeCount;
	emit alertReady(event);
	emit alertCountChanged();
}

} // namespace xinkx
