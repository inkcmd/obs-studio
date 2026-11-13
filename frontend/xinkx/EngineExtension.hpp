#pragma once

#include "EventCore.hpp"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace xinkx {

// In-process extension point. An extension is compiled into XINKX Studio and
// registered with EngineRegistry. This is not an ABI for arbitrary DLL loading.
// Future features implement this contract instead of modifying every existing
// gateway, dock and automation engine.
struct EngineAction {
	QString id;       // Globally unique, namespaced: "markers.add".
	QString title;
	bool automatable = false; // False blocks event-rule invocation.
	// A basic declarative argument form for rule editors; no hardcoded command IDs.
	QString argumentKey;
	QString argumentType; // Empty, "text", or "seconds".
	QJsonObject defaults;
};

class EngineExtension : public QObject {
	Q_OBJECT
public:
	explicit EngineExtension(QObject *parent = nullptr) : QObject(parent) {}
	~EngineExtension() override = default;

	virtual QString moduleId() const = 0;
	virtual QString moduleTitle() const = 0;
	virtual QStringList capabilities() const = 0;
	virtual QList<EngineAction> actions() const = 0;
	// Empty means no event payload is delivered to this extension.
	virtual QStringList eventTypes() const { return {}; }
	virtual QJsonObject execute(const QString &actionId, const QJsonObject &arguments) = 0;
	virtual void onEvent(const StudioEvent &) {} // Already accepted, live events only.
	virtual QJsonObject health() const { return {{QStringLiteral("status"), QStringLiteral("ready")}}; }
};

} // namespace xinkx
