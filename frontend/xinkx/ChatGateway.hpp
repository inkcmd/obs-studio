#pragma once

#include "StreamAccounts.hpp"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <functional>

class OBSBasic;

namespace xinkx {

class NativeWebSocket;

struct ChatLine {
	QString platform;
	QString accountId;
	QString name;
	QString text;
	QString type = QStringLiteral("chat");
	QString amount;
	QString messageId;
	qint64 receivedAt = 0;
};

// Uses OBS's existing libcurl stack and the OAuth accounts saved with DPAPI.
// All provider requests run off the GUI thread. No additional service process.
class ChatGateway final : public QObject {
	Q_OBJECT

public:
	explicit ChatGateway(OBSBasic *mainWindow);
	~ChatGateway() override;
	void sendMessage(const QString &message, const QStringList &accountIds);
	void reloadAccounts();
	void setKickRoomId(const QString &accountId, const QString &roomId);
	QString kickRoomId(const QString &accountId) const;
	void inspectBroadcast(const QString &accountId, const QString &youtubeBroadcastId = {});
	void searchCategories(const QString &accountId, const QString &query);
	void updateBroadcast(const QString &accountId, const QString &youtubeBroadcastId,
			     const QString &title, const QString &description, const QString &categoryId);
	void createYouTubeBroadcast(const QString &accountId, const QString &title,
				    const QString &description, const QString &categoryId);
	void bindYouTubeStream(const QString &accountId, const QString &broadcastId, const QString &streamId);
	void pollViewers(const QString &accountId);
	QVector<StreamAccount> accounts() const;

signals:
	void incoming(const xinkx::ChatLine &line);
	void providerStatus(const QString &accountId, const QString &message);
	void providerWarning(const QString &accountId, const QString &message);
	void sendStatus(const QString &accountId, const QString &message, bool success);
	void broadcastInfo(const QString &accountId, const QJsonObject &details);
	void categoryResults(const QString &accountId, const QJsonArray &categories);
	void broadcastResult(const QString &accountId, const QString &message, bool success);
	void viewersReceived(const QString &accountId, int count);

private:
	struct HttpResult {
		QJsonObject json;
		int status = 0;
		bool ok = false;
		QString error;
	};
	using RequestDone = std::function<void(const HttpResult &)>;
	using TokenDone = std::function<void(const QString &)>;
	struct YouTubeState {
		QString chatId;
		QString nextPage;
		bool baseline = false;
		bool pending = false;
		qint64 nextAt = 0;
	};
	struct KickState {
		QString roomId;
		QString slug;
		QString guestId;
		QString guestToken;
		QUrl socketUrl;
		qint64 tokenExpiresAt = 0;
		qint64 nextAt = 0;
		int attempts = 0;
		bool pending = false;
		bool subscribed = false;
	};

	OBSBasic *mainWindow;
	QTimer tick;
	QHash<QString, StreamAccount> known;
	QHash<QString, YouTubeState> youtube;
	QHash<QString, NativeWebSocket *> twitch;
	QHash<QString, KickState> kick;
	QHash<QString, NativeWebSocket *> kickSockets;
	QSet<QString> refreshing;
	QHash<QString, QList<TokenDone>> waitingForToken;
	QSet<QString> sending;
	QHash<QString, qint64> nextTwitchAt;
	QHash<QString, qint64> nextViewerPoll;
	QHash<QString, int> viewerSnapshot;
	QHash<QString, qint64> botCooldowns;
	qint64 nextReloadAt = 0;
	QSet<QString> seenEvents;
	QStringList seenOrder;

	void schedule();
	void pollYouTube(const QString &id);
	void pollYouTubeMessages(const QString &id);
	void handleYouTubePage(const QString &id, const QJsonObject &page);
	void subscribeTwitch(const QString &id, const QString &sessionId);
	void handleTwitchFrame(const QString &id, const QByteArray &frame);
	void startTwitch(const QString &id);
	void startKick(const QString &id);
	void kickResolveRoom(const QString &id);
	void kickDiscover(const QString &id);
	void kickGuestToken(const QString &id, const std::function<void(const QString &)> &done);
	void handleKickFrame(const QString &id, const QByteArray &frame);
	void dispatch(ChatLine line);
	void tokenFor(const QString &id, const TokenDone &done, bool force = false);
	void request(const QString &id, const QString &url, const QByteArray &body,
		     const QByteArray &contentType, const RequestDone &done, bool authorized = true,
		     const QStringList &extraHeaders = {}, const QByteArray &verb = {});
	void refresh(const QString &id);
	void authorized(const QString &id, const QString &url, const QByteArray &body,
			 const RequestDone &done, bool retry = false);
	void jsonApi(const QString &id, const QString &url, const QByteArray &verb,
		     const QJsonObject &body, const RequestDone &done, bool retry = false);
	void saveYouTubeStream(const QString &id, const QString &streamId, const QString &broadcastId);
	void saveRefreshed(const QString &id, const QJsonObject &json);
	void status(const QString &id, const QString &message);
};

void installChatWorkspace(OBSBasic *mainWindow);

} // namespace xinkx
