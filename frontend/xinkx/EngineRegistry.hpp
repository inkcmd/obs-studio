#pragma once

#include <QJsonArray>
#include <QObject>
#include <QPointer>
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
	void registerModule(const QString &id, const QString &name, const QStringList &capabilities,
			    QObject *object);
	Q_INVOKABLE QJsonArray inventory() const;

private:
	struct Module {
		QString id;
		QString name;
		QStringList capabilities;
		QPointer<QObject> object;
	};
	QVector<Module> modules;
};

void installEngineHub(OBSBasic *window);

} // namespace xinkx
