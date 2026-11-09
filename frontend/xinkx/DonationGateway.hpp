#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <QTcpServer>
#include <QTimer>

#include <functional>

class OBSBasic;

namespace xinkx {

class DonationGateway final : public QObject {
	Q_OBJECT
public:
	explicit DonationGateway(QObject *parent = nullptr);
	QString clientId() const { return id; }
	bool connected() const { return !token.isEmpty(); }
	QString callbackUrl() const { return QStringLiteral("http://127.0.0.1:8767/oauth/callback"); }
	void authorize(const QString &clientId, const QString &clientSecret);
	void disconnectAccount();
	void refreshNow();

signals:
	void statusChanged(const QString &message);
	void donationReceived(const QString &name, const QString &amount, const QString &message);

private:
	using Result = std::function<void(const QJsonObject &, int)>;
	QTcpServer callback;
	QTimer pollTimer;
	QString id;
	QString secret;
	QString token;
	QString refreshToken;
	QString nonce;
	qint64 expiry = 0;
	qint64 lastId = 0;
	bool baseline = false;
	bool pending = false;
	bool refreshing = false;
	quint64 generation = 0;
	void save();
	void request(const QString &url, const QStringList &headers, const QByteArray &body,
		     const QByteArray &mime, const Result &done);
	void exchange(const QString &grantType, const QString &value);
	void fetch();
};

void installDonationWorkspace(OBSBasic *mainWindow);

} // namespace xinkx
