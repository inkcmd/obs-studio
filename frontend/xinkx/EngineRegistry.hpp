#pragma once

#include "EngineExtension.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVector>

class OBSBasic;

namespace xinkx {

// A small in-process module discovery surface. Future extensions can query
// capabilities without depending on specific dock widgets or startup order.
// This is NOT a stable binary plugin ABI or a dynamic loader.
class EngineRegistry final : public QObject {
	Q_OBJECT
public:
	explicit EngineRegistry(OBSBasic *window);
	~EngineRegistry() override;
	void registerModule(const QString &id, const QString &name, const QStringList &capabilities,
			    QObject *object);
	// Extension registration rejects duplicate engine/action IDs and invalid names.
	// Ownership remains with the extension's QObject parent.
	bool registerExtension(EngineExtension *extension, QString *error = nullptr);
	Q_INVOKABLE QJsonArray inventory() const;
	Q_INVOKABLE QJsonArray actionCatalog() const;
	QStringList automatableActions() const;
	// The only action dispatcher for local XINKX modules. Event automation may
	// invoke only actions explicitly marked automatable by their owner.
	QJsonObject executeAction(const QJsonObject &command, bool fromRule = false);
	bool validateAction(const QJsonObject &command, bool fromRule, QString *error = nullptr) const;
	// Idempotent, called before OBS destroys scene data and on registry cleanup.
	void shutdownExtensions();

signals:
	void catalogChanged();

private:
	struct Module {
		QString id;
		QString name;
		QStringList capabilities;
		QPointer<QObject> object;
	};
	QVector<Module> modules;
	QVector<QPointer<EngineExtension>> extensions;
	QSet<QString> runningExtensions;
	bool shuttingDown = false;
	bool prepareAction(const QJsonObject &command, bool fromRule,
		EngineExtension **target, QJsonObject *normalized, QString *error) const;
	void deliverToExtensions(const StudioEvent &event);
};

// Called after StudioAutomation and EventCore exist, before EventAutomation.
void installEngineServices(OBSBasic *window);
void shutdownEngineServices(OBSBasic *window);
void installEngineHub(OBSBasic *window);

} // namespace xinkx
