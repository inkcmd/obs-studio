#pragma once

#include "EventCore.hpp"

#include <QObject>

class OBSBasic;

namespace xinkx {

// Presentation-independent notification taxonomy. Platform listeners never
// decide which OBS overlay source to paint into.
class AlertEngine final : public QObject {
	Q_OBJECT
public:
	explicit AlertEngine(OBSBasic *window);
	quint64 freeAlerts() const { return freeCount; }
	quint64 paidAlerts() const { return paidCount; }

signals:
	void chatReady(const xinkx::StudioEvent &event);
	void alertReady(const xinkx::StudioEvent &event);
	void alertCountChanged();

private:
	quint64 freeCount = 0;
	quint64 paidCount = 0;
	void onEvent(const StudioEvent &event);
};

} // namespace xinkx
