#pragma once

#include "StreamAccounts.hpp"

#include <QNetworkAccessManager>
#include <QObject>
#include <QJsonObject>
#include <QTcpServer>
#include <QUrl>

#include <functional>

namespace xinkx {

// Local desktop OAuth authorization-code flow; tokens never pass through a
// third-party service. Twitch uses the configured app secret, Google/Kick PKCE.
class OAuthLogin final : public QObject {
	Q_OBJECT

public:
	explicit OAuthLogin(QObject *parent = nullptr);
	bool begin(const StreamAccount &account, QString *error);
	void retrieveIngest(const StreamAccount &account);
	bool isBusy() const { return busy; }
	static QString redirectUri();

signals:
	void finished(const StreamAccount &account, const QString &notice);
	void failed(const QString &accountId, const QString &message);

private:
	StreamAccount current;
	QTcpServer callback;
	QNetworkAccessManager network;
	QString state;
	QString verifier;
	quint64 attempt = 0;
	bool busy = false;
	void receiveCallback();
	void exchangeCode(const QString &code);
	void getJson(const QUrl &url, const std::function<void(const QJsonObject &, const QString &)> &done);
	void finishWithIngest(const QString &url, const QString &key, const QString &notice = {});
};

} // namespace xinkx
