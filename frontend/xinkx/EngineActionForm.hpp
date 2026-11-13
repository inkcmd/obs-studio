#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QWidget>

class QFormLayout;

namespace xinkx {

// Shared, declarative parameter editor used by manual commands and event rules.
// Adding an EngineAction with a typed argument schema needs no new widget code.
class EngineActionForm final : public QWidget {
public:
	explicit EngineActionForm(QWidget *parent = nullptr);
	void setAction(const QJsonObject &action);
	QJsonObject command() const;

private:
	QFormLayout *form = nullptr;
	QJsonObject actionSpec;
	QJsonArray arguments;
	QHash<QString, QWidget *> editors;
};

} // namespace xinkx
