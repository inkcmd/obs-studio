#pragma once

#include <QObject>
#include <QString>

class QTimer;

namespace xinkx {

// Read-only bridge to the actual OBS scene graph and active outputs.
// Editor/output commands will be added here in subsequent stages.
class StudioController final : public QObject {
	Q_OBJECT

public:
	explicit StudioController(QObject *parent = nullptr);

signals:
	void stateChanged(const QString &sceneName, const QString &selectedSource, const QString &sourceDetails,
			  bool streaming, bool recording);

private:
	void refresh();

	QString previousScene;
	QString previousSource;
	QString previousDetails;
	bool previousStreaming = false;
	bool previousRecording = false;
	bool firstRefresh = true;
};

} // namespace xinkx
