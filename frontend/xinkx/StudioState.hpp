#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>

class OBSBasic;

namespace xinkx {

// Read-only, in-process view of the current studio session. This is the first
// Studio State contract; it does not take ownership of libobs or engine objects.
// Do not add account tokens, RTMP keys, chat messages or event payloads here.
class StudioState final : public QObject {
	Q_OBJECT
public:
	explicit StudioState(OBSBasic *window);
	// Call on the Qt GUI thread. A new JSON value is assembled for every read:
	// no cached raw OBS pointers or duplicated mutable application state.
	Q_INVOKABLE QJsonObject snapshot() const;

private:
	QPointer<OBSBasic> mainWindow;
};

} // namespace xinkx
