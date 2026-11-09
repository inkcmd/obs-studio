#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTcpServer>
#include <QTimer>

class QTcpSocket;
class QDockWidget;
class OBSBasic;

namespace xinkx {

// One in-process event bridge shared by overlays, reactions and future chat/alerts.
// The only listener is loopback; no Node, Kubernetes or external overlay service.
class OverlayRuntime final : public QObject {
	Q_OBJECT

public:
	explicit OverlayRuntime(QObject *parent = nullptr);
	bool available() const { return listener.isListening(); }
	QString error() const;
	QString sourceUrl() const;
	void reaction(const QString &type);
	void testAlert(const QString &type);
	void publishChat(const QString &platform, const QString &name, const QString &message);
	void publishAlert(const QString &type, const QString &platform, const QString &name,
			  const QString &amount, const QString &message);
	void startTimer(int seconds);
	void pauseTimer();
	void resetTimer();
	void setPosition(const QString &position);
	void setPaused(bool paused);
	bool addToScene(OBSBasic *mainWindow, QString *message);

signals:
	void eventDispatched(const QString &label);
	void timerChanged(int seconds, bool running);

private:
	QTcpServer listener;
	QSet<QTcpSocket *> clients;
	QTimer tick;
	QHash<QString, qint64> cooldowns;
	qint64 lastReactionAt = 0;
	qint64 deadlineMs = 0;
	int remaining = 0;
	bool timerVisible = false;
	bool effectsPaused = false;
	QString position = QStringLiteral("top-right");

	void acceptConnections();
	void handleRequest(QTcpSocket *socket, const QByteArray &header);
	void sendEvent(QTcpSocket *socket, const QByteArray &name, const QJsonObject &payload);
	void broadcast(const QByteArray &name, const QJsonObject &payload);
	void stateChanged();
	void dispatch(QJsonObject event);
	int secondsRemaining() const;
};

void installOverlayWorkspace(OBSBasic *mainWindow, QDockWidget *sourceInspector);

} // namespace xinkx
