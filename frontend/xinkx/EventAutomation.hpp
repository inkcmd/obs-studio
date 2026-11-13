#pragma once

#include "EventCore.hpp"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class OBSBasic;

namespace xinkx {

// Rules run only against accepted, live events. No replay, raw scripts,
// HTTP callbacks, shell commands or broadcast start/stop actions are allowed.
class EventAutomation final : public QObject {
	Q_OBJECT
public:
	explicit EventAutomation(OBSBasic *window);
	QJsonArray rules() const { return savedRules; }
	bool replaceRules(const QJsonArray &rules, QString *error = nullptr);
	bool validRule(const QJsonObject &rule, QString *error = nullptr) const;

signals:
	void executed(const QString &ruleName, const QString &result, bool success);
	void rulesChanged();

private:
	OBSBasic *window;
	QJsonArray savedRules;
	QHash<QString, qint64> nextAllowed;
	QString path;
	void onEvent(const StudioEvent &event);
};

void installEventAutomationWorkspace(OBSBasic *window);

} // namespace xinkx
