#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

namespace xinkx {

class NativeWebSocket;

// Separate, optional YouTube Studio session. OAuth and ordinary broadcasts
// never depend on this unofficial browser integration.
class YouTubeStudioBridge final : public QObject {
	Q_OBJECT
public:
	explicit YouTubeStudioBridge(QObject *parent = nullptr);
	~YouTubeStudioBridge() override;
	void openLogin();
	void connectChannel(const QString &channelId);
	void searchGames(const QString &videoId, const QString &query);
	void getSelectedGame(const QString &videoId);
	void setGame(const QString &videoId, const QString &mid, const QString &title);
	bool isReadyFor(const QString &channelId) const;

signals:
	void status(const QString &message, bool ready);
	void gamesFound(const QString &query, const QJsonArray &games);
	void gameRead(const QString &videoId, const QString &name);
	void gameSaved(const QString &videoId, const QString &message, bool success);

private:
	using JsonDone = std::function<void(const QJsonObject &)>;
	QNetworkAccessManager network;
	std::unique_ptr<NativeWebSocket> socket;
	QHash<int, JsonDone> pending;
	QTimer discovery;
	QString executable;
	QString profile;
	QString expectedChannel;
	QString verifiedChannel;
	int discoveryTries = 0;
	int commandId = 0;
	bool socketReady = false;
	bool operationBusy = false;
	void notify(const QString &message, bool ready = false);
	void discover();
	void verifyChannel(int retries);
	void command(const QString &method, const QJsonObject &params, JsonDone done);
	void evaluate(const QString &expression, JsonDone done);
	void navigate(const QString &videoId, std::function<void(bool)> done);
	void waitForGameField(int retries, std::function<void(bool)> done);
	void readGame(const QString &videoId, std::function<void(const QString &)> done);
	void failOperation(const QString &videoId, const QString &reason);
	static QString quoted(const QString &value);
};

} // namespace xinkx
