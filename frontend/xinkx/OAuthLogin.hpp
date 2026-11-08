#pragma once

#include "StreamAccounts.hpp"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTcpServer>

#include <functional>

class QNetworkReply;
class QUrlQuery;

namespace xinkx {

class OAuthLogin final : public QObject {
	Q_OBJECT

public:
	explicit OAuthLogin(QObject *parent = nullptr);
	bool begin(const StreamAccount &account, QString *error);
	void retrieveIngest(const StreamAccount &account);
	void revoke(const StreamAccount &account);
	void cancel();
	bool isBusy() const { return busy; }
	QString authorizationUrl() const { return pendingAuthorizationUrl; }
	static QString redirectUri(const QString &platform);

signals:
	void finished(const StreamAccount &account, const QString &notice, bool success);
	void failed(const QString &accountId, const QString &message);
	void cancelled(const QString &accountId);

private:
	struct Response {
		QJsonObject json;
		int status = 0;
		bool ok = false;
		QString error(const QString &operation) const;
	};
	using Done = std::function<void(const Response &)>;

	StreamAccount current;
	QTcpServer callback;
	QNetworkAccessManager network;
	QString state;
	QString verifier;
	QString pendingAuthorizationUrl;
	quint64 attempt = 0;
	bool busy = false;
	bool refreshed = false;

	void receiveCallback();
	void request(const QUrl &url, const QUrlQuery *form, bool authorized, const Done &done);
	void exchangeCode(const QString &code);
	void refresh(const std::function<void()> &done);
	void requestToken(QUrlQuery form, bool replacing, const std::function<void()> &done);
	void getJson(const QUrl &url, const Done &done);
	void identify();
	void fetchIngest();
	void finishWithIngest(const QString &url, const QString &key, const QString &notice);
	void revokeToken(const QString &token, bool refreshToken, const Done &done);
	void complete(const QString &notice, bool success = true);
	void fail(const QString &notice);
};

} // namespace xinkx
