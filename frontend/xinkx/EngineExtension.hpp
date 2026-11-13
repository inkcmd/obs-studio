#pragma once

#include "EventCore.hpp"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace xinkx {

// Typed, declarative parameters are used by the command dispatcher and both
// manual and event-rule editors. Default values live in EngineAction::defaults.
struct EngineArgument {
	QString key;
	QString title;
	QString type; // "text", "seconds", "boolean", "choice".
	bool required = false;
	int minimum = 0;
	int maximum = 3600; // For seconds, or maximum text length.
	QStringList choices; // Nonempty for "choice".
};

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
	QList<EngineArgument> arguments;
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
	// Called once after validation and before accepting commands/events. New
	// engines may allocate resources here and release them in stop(). Existing
	// engines can keep their current startup/shutdown strategy.
	virtual bool start(QString *error = nullptr) { Q_UNUSED(error); return true; }
	virtual void stop() {}
	virtual QJsonObject execute(const QString &actionId, const QJsonObject &arguments) = 0;
	virtual void onEvent(const StudioEvent &) {} // Already accepted, live events only.
	virtual QJsonObject health() const { return {{QStringLiteral("status"), QStringLiteral("ready")}}; }
};

} // namespace xinkx
