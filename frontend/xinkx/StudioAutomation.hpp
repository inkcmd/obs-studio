#pragma once

#include <QJsonObject>
#include <QObject>

class OBSBasic;

namespace xinkx {

// UI-thread command surface for local LLM tools. No network listener is exposed.
// A future authenticated localhost bridge or obs-websocket vendor endpoint can
// call execute() without granting models access to arbitrary OBS scripts.
class StudioAutomation final : public QObject {
	Q_OBJECT
public:
	explicit StudioAutomation(OBSBasic *mainWindow);
	Q_INVOKABLE QJsonObject execute(const QJsonObject &command);
	Q_INVOKABLE QJsonObject describe() const;

private:
	OBSBasic *window;
};

} // namespace xinkx
