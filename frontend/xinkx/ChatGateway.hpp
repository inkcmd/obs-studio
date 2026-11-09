#pragma once

#include "StreamAccounts.hpp"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

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
	QVector<StreamAccount> accounts() const;

signals:
	void incoming(const xinkx::ChatLine &line);
	void providerStatus(const QString &accountId, const QString &message);
	void sendStatus(const QString &accountId, const QString &message, bool success);

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

	OBSBasic *mainWindow;
	QTimer tick;
	QHash<QString, StreamAccount> known;
	QHash<QString, YouTubeState> youtube;
	QHash<QString, NativeWebSocket *> twitch;
	QSet<QString> refreshing;
	QHash<QString, QList<TokenDone>> waitingForToken;
	QSet<QString> sending;
	QHash<QString, qint64> nextTwitchAt;
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
	void dispatch(ChatLine line);
	void tokenFor(const QString &id, const TokenDone &done, bool force = false);
	void request(const QString &id, const QString &url, const QByteArray &body,
		     const QByteArray &contentType, const RequestDone &done, bool authorized = true);
	void refresh(const QString &id);
	void authorized(const QString &id, const QString &url, const QByteArray &body,
			 const RequestDone &done, bool retry = false);
	void saveRefreshed(const QString &id, const QJsonObject &json);
	void status(const QString &id, const QString &message);
};

void installChatWorkspace(OBSBasic *mainWindow);

} // namespace xinkx
