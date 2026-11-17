#pragma once

#include "EngineExtension.hpp"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QSet>
#include <QTimer>

class OBSBasic;

namespace xinkx {

// Example of a feature which extends the platform without modifying EventCore
// or ChatGateway. It stores only local marker labels and timestamps, never
// provider message content, user IDs or donation amounts.
class MarkerEngine final : public EngineExtension {
	Q_OBJECT
public:
	explicit MarkerEngine(QObject *parent = nullptr);
	~MarkerEngine() override;

	QString moduleId() const override { return QStringLiteral("markers"); }
	QString moduleTitle() const override { return QStringLiteral("Маркеры монтажа"); }
	QStringList capabilities() const override
	{
		return {QStringLiteral("manual-markers"), QStringLiteral("event-markers"),
			QStringLiteral("csv-export"), QStringLiteral("kdenlive-sidecars")};
	}
	QList<EngineAction> actions() const override
	{
		return {{QStringLiteral("markers.add"), QStringLiteral("Добавить маркер"), true,
			QStringLiteral("label"), QStringLiteral("text"), {},
			{{QStringLiteral("label"), QStringLiteral("Название маркера"),
				QStringLiteral("text"), true, 0, 120}}}};
	}
	QStringList eventTypes() const override
	{
		return {QStringLiteral("donation"), QStringLiteral("sub"),
			QStringLiteral("raid"), QStringLiteral("follow")};
	}
	QJsonObject execute(const QString &actionId, const QJsonObject &arguments) override;
	void onEvent(const StudioEvent &event) override;
	QJsonObject health() const override;
	void stop() override;

	QJsonArray markers() const { return entries; }
	QStringList automaticTypes() const;
	bool setAutomaticTypes(const QStringList &types, QString *error = nullptr);
	bool exportCsv(const QString &fileName, QString *error = nullptr) const;
	bool clear(QString *error = nullptr);
	bool markMoment(); // OBS global hotkey: only active opted-in recording outputs.
	QString momentPrefix() const { return prefix; }
	int kdenliveType() const { return category; }
	bool alsoExportCsv() const { return sidecarCsv; }
	bool setMomentOptions(const QString &newPrefix, int newType, bool newCsv, QString *error = nullptr);

signals:
	void changed();

private:
	QString path;
	QJsonArray entries;
	QSet<QString> autoTypes;
	QElapsedTimer session;
	QTimer saveTimer;
	QString storageError;
	QString prefix = QStringLiteral("Момент");
	int category = 0;
	bool sidecarCsv = false;
	int momentCounter = 0;

	void load();
	bool flush(QString *error = nullptr);
	bool add(const QString &label, const QString &type, const QString &platform, bool requireRecording = false);
};

void installMarkerWorkspace(OBSBasic *mainWindow);

} // namespace xinkx
