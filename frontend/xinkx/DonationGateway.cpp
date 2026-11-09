#include "DonationGateway.hpp"

#include "StreamAccounts.hpp"

#include <utility/RemoteTextThread.hpp>

#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace xinkx {

DonationGateway::DonationGateway(QObject *parent) : QObject(parent)
{
	const auto config = studioSettingsPath();
	if (!config.isEmpty()) {
		QSettings settings(config, QSettings::IniFormat);
		id = settings.value(QStringLiteral("donations/clientId")).toString();
		secret = unprotectStudioSecret(settings.value(QStringLiteral("donations/clientSecret")).toString());
		token = unprotectStudioSecret(settings.value(QStringLiteral("donations/accessToken")).toString());
		refreshToken = unprotectStudioSecret(settings.value(QStringLiteral("donations/refreshToken")).toString());
		expiry = settings.value(QStringLiteral("donations/expiry")).toLongLong();
		lastId = settings.value(QStringLiteral("donations/checkpoint")).toLongLong();
		baseline = lastId > 0;
	}
	connect(&callback, &QTcpServer::newConnection, this, [this] {
		while (auto *socket = callback.nextPendingConnection()) {
			auto input = std::make_shared<QByteArray>();
			connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
			connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input] {
				input->append(socket->readAll());
				if (input->size() > 8192) { socket->disconnectFromHost(); return; }
				if (!input->contains("\r\n\r\n")) return;
				const auto first = input->left(input->indexOf("\r\n"));
				const auto request = first.split(' ');
				QString code;
				bool accepted = false;
				if (request.size() >= 3 && request[0] == "GET") {
					QUrl url(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(request[1]));
					if (url.path() == QStringLiteral("/oauth/callback")) {
						const QUrlQuery query(url);
						accepted = !nonce.isEmpty() && query.queryItemValue(QStringLiteral("state")) == nonce;
						if (accepted) code = query.queryItemValue(QStringLiteral("code"));
					}
				}
				const QByteArray body = accepted && !code.isEmpty()
					? QByteArray("OAuth received. Return to XINKX Studio.")
					: QByteArray("Authorization failed or expired.");
				socket->write(QByteArray("HTTP/1.1 ") + (accepted && !code.isEmpty() ? "200 OK" : "403 Forbidden") +
					"\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: " +
					QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
				socket->disconnectFromHost();
				if (accepted && !code.isEmpty()) {
					nonce.clear();
					callback.close();
					exchange(QStringLiteral("authorization_code"), code);
				}
			});
		}
	});
	pollTimer.setInterval(15000);
	connect(&pollTimer, &QTimer::timeout, this, &DonationGateway::fetch);
	pollTimer.start();
	if (!token.isEmpty()) QTimer::singleShot(500, this, &DonationGateway::fetch);
}

void DonationGateway::save()
{
	if (studioSettingsPath().isEmpty()) return;
	const auto encryptedSecret = protectStudioSecret(secret);
	const auto encryptedToken = protectStudioSecret(token);
	const auto encryptedRefresh = protectStudioSecret(refreshToken);
	if ((!secret.isEmpty() && encryptedSecret.isEmpty()) ||
	    (!token.isEmpty() && encryptedToken.isEmpty()) ||
	    (!refreshToken.isEmpty() && encryptedRefresh.isEmpty())) {
		emit statusChanged(QStringLiteral("Не удалось защитить токены Windows DPAPI"));
		return;
	}
	QSettings settings(studioSettingsPath(), QSettings::IniFormat);
	settings.setValue(QStringLiteral("donations/clientId"), id);
	settings.setValue(QStringLiteral("donations/clientSecret"), encryptedSecret);
	settings.setValue(QStringLiteral("donations/accessToken"), encryptedToken);
	settings.setValue(QStringLiteral("donations/refreshToken"), encryptedRefresh);
	settings.setValue(QStringLiteral("donations/expiry"), expiry);
	settings.setValue(QStringLiteral("donations/checkpoint"), lastId);
	settings.sync();
}

void DonationGateway::request(const QString &url, const QStringList &headers, const QByteArray &body,
			      const QByteArray &mime, const Result &done)
{
	struct Response { std::string payload; std::string error; long status = 0; };
	const auto result = std::make_shared<Response>();
	std::vector<std::string> extra{"Accept: application/json"};
	for (const auto &header : headers) extra.push_back(header.toStdString());
	auto *thread = QThread::create([result, address = url.toStdString(), extra, body, mime] {
		const auto payload = body.toStdString();
		const auto media = mime.toStdString();
		GetRemoteFile(address.c_str(), result->payload, result->error, &result->status,
			body.isNull() ? nullptr : media.c_str(), body.isNull() ? "GET" : "POST",
			body.isNull() ? nullptr : payload.c_str(), extra, nullptr, 20, false,
			static_cast<int>(payload.size()));
	});
	connect(thread, &QThread::finished, this, [result, done] {
		done(QJsonDocument::fromJson(QByteArray::fromStdString(result->payload)).object(),
			static_cast<int>(result->status));
	});
	connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	thread->start();
}

void DonationGateway::authorize(const QString &clientId, const QString &clientSecret)
{
	const QString cleaned = clientId.trimmed();
	if (cleaned.isEmpty() || clientSecret.trimmed().isEmpty()) {
		emit statusChanged(QStringLiteral("Укажи Client ID и Client Secret приложения DonationAlerts")); return;
	}
	if (callback.isListening()) callback.close();
	if (!callback.listen(QHostAddress::LocalHost, 8767)) {
		emit statusChanged(QStringLiteral("Невозможно открыть localhost:8767: ") + callback.errorString()); return;
	}
	++generation;
	pending = false;
	refreshing = false;
	id = cleaned;
	secret = clientSecret.trimmed();
	nonce = QUuid::createUuid().toString(QUuid::WithoutBraces) + QUuid::createUuid().toString(QUuid::WithoutBraces);
	QUrl url(QStringLiteral("https://www.donationalerts.com/oauth/authorize"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("client_id"), id);
	query.addQueryItem(QStringLiteral("redirect_uri"), callbackUrl());
	query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
	query.addQueryItem(QStringLiteral("scope"), QStringLiteral("oauth-user-show oauth-donation-index oauth-donation-subscribe"));
	query.addQueryItem(QStringLiteral("state"), nonce);
	url.setQuery(query);
	if (!QDesktopServices::openUrl(url)) {
		callback.close();
		emit statusChanged(QStringLiteral("Браузер OAuth не открылся"));
		return;
	}
	emit statusChanged(QStringLiteral("Открылся DonationAlerts OAuth; ждём callback на 127.0.0.1:8767"));
	QTimer::singleShot(180000, this, [this, challenge = nonce] {
		if (challenge == nonce && !challenge.isEmpty()) {
			nonce.clear();
			callback.close();
			emit statusChanged(QStringLiteral("Время ожидания DonationAlerts OAuth истекло"));
		}
	});
}

void DonationGateway::exchange(const QString &grantType, const QString &value)
{
	if (refreshing || id.isEmpty() || secret.isEmpty()) return;
	const auto session = generation;
	refreshing = true;
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("grant_type"), grantType);
	query.addQueryItem(QStringLiteral("client_id"), id);
	query.addQueryItem(QStringLiteral("client_secret"), secret);
	query.addQueryItem(QStringLiteral("redirect_uri"), callbackUrl());
	query.addQueryItem(grantType == QStringLiteral("authorization_code")
		? QStringLiteral("code") : QStringLiteral("refresh_token"), value);
	request(QStringLiteral("https://www.donationalerts.com/oauth/token"), {},
		query.toString(QUrl::FullyEncoded).toUtf8(), QByteArray("application/x-www-form-urlencoded"),
		[this, session, grantType](const QJsonObject &json, int status) {
			if (session != generation) return;
			refreshing = false;
			if (status < 200 || status >= 300 || json.value(QStringLiteral("access_token")).toString().isEmpty()) {
				emit statusChanged(QStringLiteral("DonationAlerts OAuth: HTTP %1").arg(status)); return;
			}
			token = json.value(QStringLiteral("access_token")).toString();
			refreshToken = json.value(QStringLiteral("refresh_token")).toString(refreshToken);
			expiry = QDateTime::currentSecsSinceEpoch() + json.value(QStringLiteral("expires_in")).toInt(3600);
			save();
			if (grantType == QStringLiteral("authorization_code")) {
				baseline = false;
				lastId = 0;
			}
			emit statusChanged(QStringLiteral("DonationAlerts: OAuth подключён, ожидаем новые донаты"));
			fetch();
		});
}

void DonationGateway::fetch()
{
	if (pending || refreshing || token.isEmpty()) return;
	if (expiry > 0 && expiry < QDateTime::currentSecsSinceEpoch() + 90) {
		if (!refreshToken.isEmpty()) exchange(QStringLiteral("refresh_token"), refreshToken);
		return;
	}
	pending = true;
	const auto session = generation;
	request(QStringLiteral("https://www.donationalerts.com/api/v1/alerts/donations?page=1"),
		{QStringLiteral("Authorization: Bearer ") + token}, {}, {},
		[this, session](const QJsonObject &json, int code) {
			if (session != generation) return;
			pending = false;
			if (code == 401 && !refreshToken.isEmpty()) {
				exchange(QStringLiteral("refresh_token"), refreshToken); return;
			}
			if (code < 200 || code >= 300) {
				emit statusChanged(QStringLiteral("DonationAlerts: запрос донатов HTTP %1").arg(code)); return;
			}
			const auto entries = json.value(QStringLiteral("data")).toArray();
			qint64 maxId = lastId;
			QVector<QJsonObject> recent;
			for (const auto &entry : entries) {
				const auto item = entry.toObject();
				const qint64 value = item.value(QStringLiteral("id")).toVariant().toLongLong();
				if (value > maxId) maxId = value;
				if (baseline && value > lastId) recent.push_back(item);
			}
			if (!baseline) { baseline = true; lastId = maxId; save(); return; }
			const bool advanced = maxId > lastId;
			lastId = maxId;
			if (advanced) {
				QSettings settings(studioSettingsPath(), QSettings::IniFormat);
				settings.setValue(QStringLiteral("donations/checkpoint"), lastId);
			}
			std::reverse(recent.begin(), recent.end());
			for (const auto &item : recent) {
				const QString name = item.value(QStringLiteral("username")).toString(QStringLiteral("Зритель"));
				const QString amount = item.value(QStringLiteral("amount")).toVariant().toString() +
					QStringLiteral(" ") + item.value(QStringLiteral("currency")).toString();
				const QString message = item.value(QStringLiteral("message")).toString();
				emit donationReceived(name.left(80), amount.left(60), message.left(240));
			}
		});
}

void DonationGateway::refreshNow()
{
	if (!token.isEmpty()) fetch();
	else emit statusChanged(QStringLiteral("DonationAlerts: сначала авторизуй аккаунт"));
}

void DonationGateway::disconnectAccount()
{
	++generation;
	pending = false;
	refreshing = false;
	callback.close();
	nonce.clear();
	token.clear();
	refreshToken.clear();
	expiry = 0;
	baseline = false;
	lastId = 0;
	save();
	emit statusChanged(QStringLiteral("DonationAlerts: токены удалены"));
}

} // namespace xinkx
