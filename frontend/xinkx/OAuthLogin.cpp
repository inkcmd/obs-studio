#include "OAuthLogin.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include <memory>

namespace xinkx {

namespace {

QByteArray randomToken(int size)
{
	QByteArray bytes;
	bytes.reserve(size);
	for (int i = 0; i < size; ++i) {
		bytes.append(static_cast<char>(QRandomGenerator::system()->generate() & 0xff));
	}
	return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString oauthError(QNetworkReply *reply, const QByteArray &body)
{
	const QJsonObject json = QJsonDocument::fromJson(body).object();
	const QString detail = json.value(QStringLiteral("error_description")).toString(
		json.value(QStringLiteral("message")).toString(json.value(QStringLiteral("error")).toString()));
	return detail.isEmpty() ? reply->errorString() : detail.left(240);
}

QJsonObject firstDataObject(const QJsonObject &result)
{
	const QJsonArray entries = result.value(QStringLiteral("data")).toArray();
	return entries.isEmpty() ? QJsonObject{} : entries.first().toObject();
}

QUrl authorizationEndpoint(const QString &platform)
{
	if (platform == QStringLiteral("twitch")) {
		return QUrl(QStringLiteral("https://id.twitch.tv/oauth2/authorize"));
	}
	if (platform == QStringLiteral("youtube")) {
		return QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"));
	}
	return QUrl(QStringLiteral("https://id.kick.com/oauth/authorize"));
}

QUrl tokenEndpoint(const QString &platform)
{
	if (platform == QStringLiteral("twitch")) {
		return QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token"));
	}
	if (platform == QStringLiteral("youtube")) {
		return QUrl(QStringLiteral("https://oauth2.googleapis.com/token"));
	}
	return QUrl(QStringLiteral("https://id.kick.com/oauth/token"));
}

QString scopes(const QString &platform)
{
	if (platform == QStringLiteral("twitch")) {
		return QStringLiteral("user:read:chat user:write:chat channel:manage:broadcast channel:read:stream_key");
	}
	if (platform == QStringLiteral("youtube")) {
		return QStringLiteral("https://www.googleapis.com/auth/youtube");
	}
	return QStringLiteral("user:read channel:read events:subscribe chat:write channel:write streamkey:read");
}

} // namespace

OAuthLogin::OAuthLogin(QObject *parent) : QObject(parent)
{
	QObject::connect(&callback, &QTcpServer::newConnection, this, [this] { receiveCallback(); });
	QObject::connect(this, &OAuthLogin::finished, this, [this] { busy = false; });
	QObject::connect(this, &OAuthLogin::failed, this, [this] { busy = false; });
}

QString OAuthLogin::redirectUri()
{
	return QStringLiteral("http://127.0.0.1:45678/");
}

bool OAuthLogin::begin(const StreamAccount &account, QString *error)
{
	if (busy) {
		if (error) {
			*error = QStringLiteral("Дождись завершения текущей операции OAuth");
		}
		return false;
	}
	if (account.clientId.trimmed().isEmpty()) {
		if (error) {
			*error = QStringLiteral("Для OAuth нужен Client ID приложения платформы");
		}
		return false;
	}
	if (account.platform == QStringLiteral("twitch") && account.clientSecret.isEmpty()) {
		if (error) {
			*error = QStringLiteral("Для Twitch Authorization Code нужен Client Secret");
		}
		return false;
	}
	callback.close();
	if (!callback.listen(QHostAddress::LocalHost, 45678)) {
		if (error) {
			*error = QStringLiteral("Локальный OAuth-порт 45678 уже занят");
		}
		return false;
	}
	current = account;
	state = QString::fromLatin1(randomToken(24));
	verifier = QString::fromLatin1(randomToken(48));
	const bool pkce = account.platform != QStringLiteral("twitch");
	QUrl url = authorizationEndpoint(account.platform);
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
	query.addQueryItem(QStringLiteral("client_id"), account.clientId);
	query.addQueryItem(QStringLiteral("redirect_uri"), redirectUri());
	query.addQueryItem(QStringLiteral("scope"), scopes(account.platform));
	query.addQueryItem(QStringLiteral("state"), state);
	if (pkce) {
		const QByteArray challenge = QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
					       .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
		query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
		query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
	}
	if (account.platform == QStringLiteral("youtube")) {
		query.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
		query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
	}
	url.setQuery(query);
	if (!QDesktopServices::openUrl(url)) {
		callback.close();
		if (error) {
			*error = QStringLiteral("Не удалось открыть браузер для OAuth");
		}
		return false;
	}
	busy = true;
	const quint64 thisAttempt = ++attempt;
	QTimer::singleShot(180000, this, [this, thisAttempt] {
		if (thisAttempt == attempt && callback.isListening()) {
			callback.close();
			emit failed(current.id, QStringLiteral("Время ожидания OAuth истекло"));
		}
	});
	return true;
}

void OAuthLogin::receiveCallback()
{
	while (QTcpSocket *socket = callback.nextPendingConnection()) {
		auto requestBuffer = std::make_shared<QByteArray>();
		QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, requestBuffer] {
			if (socket->property("xinkxOAuthHandled").toBool()) {
				return;
			}
			requestBuffer->append(socket->readAll());
			if (requestBuffer->size() > 8192) {
				socket->disconnectFromHost();
				return;
			}
			const int lineEnd = requestBuffer->indexOf("\r\n");
			if (lineEnd < 0) {
				return;
			}
			const QByteArray request = requestBuffer->left(lineEnd);
			if (!request.startsWith("GET ") || !request.contains(" HTTP/")) {
				socket->disconnectFromHost();
				return;
			}
			socket->setProperty("xinkxOAuthHandled", true);
			const QList<QByteArray> parts = request.split(' ');
			QUrl incoming(parts.size() > 1 ? QString::fromUtf8(parts[1]) : QString());
			const QUrlQuery params(incoming);
			const bool valid = incoming.path() == QStringLiteral("/") &&
					   params.queryItemValue(QStringLiteral("state")) == state;
			const QString code = params.queryItemValue(QStringLiteral("code"));
			const QString error = params.queryItemValue(QStringLiteral("error"));
			const QByteArray body = valid && !code.isEmpty()
				? QByteArray("<html><body>Connected to XINKX Studio. You may close this tab.</body></html>")
				: QByteArray("<html><body>Authorization failed. Return to XINKX Studio.</body></html>");
			socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: ") +
				      QByteArray::number(body.size()) + "\r\n\r\n" + body);
			socket->disconnectFromHost();
			callback.close();
			if (valid && !code.isEmpty()) {
				exchangeCode(code);
			} else {
				emit failed(current.id, error.isEmpty() ? QStringLiteral("OAuth state не совпал") : error);
			}
		});
	}
}

void OAuthLogin::exchangeCode(const QString &code)
{
	QUrlQuery body;
	body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
	body.addQueryItem(QStringLiteral("client_id"), current.clientId);
	body.addQueryItem(QStringLiteral("redirect_uri"), redirectUri());
	body.addQueryItem(QStringLiteral("code"), code);
	if (!current.clientSecret.isEmpty()) {
		body.addQueryItem(QStringLiteral("client_secret"), current.clientSecret);
	}
	if (current.platform != QStringLiteral("twitch")) {
		body.addQueryItem(QStringLiteral("code_verifier"), verifier);
	}
	QNetworkRequest request(tokenEndpoint(current.platform));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
	auto *reply = network.post(request, body.query(QUrl::FullyEncoded).toUtf8());
	QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
		const QByteArray response = reply->readAll();
		if (reply->error() != QNetworkReply::NoError) {
			emit failed(current.id, QStringLiteral("OAuth: %1").arg(oauthError(reply, response)));
			reply->deleteLater();
			return;
		}
		reply->deleteLater();
		const QJsonObject json = QJsonDocument::fromJson(response).object();
		current.accessToken = json.value(QStringLiteral("access_token")).toString();
		if (current.accessToken.isEmpty()) {
			emit failed(current.id, QStringLiteral("Платформа не вернула access_token"));
			return;
		}
		current.refreshToken = json.value(QStringLiteral("refresh_token")).toString(current.refreshToken);
		current.expiresAt = QDateTime::currentSecsSinceEpoch() + json.value(QStringLiteral("expires_in")).toInt(3600);
		retrieveIngest(current);
	});
}

void OAuthLogin::getJson(const QUrl &url, const std::function<void(const QJsonObject &, const QString &)> &done)
{
	QNetworkRequest request(url);
	request.setRawHeader("Authorization", QByteArray("Bearer ") + current.accessToken.toUtf8());
	request.setRawHeader("Accept", "application/json");
	if (current.platform == QStringLiteral("twitch")) {
		request.setRawHeader("Client-Id", current.clientId.toUtf8());
	}
	auto *reply = network.get(request);
	QObject::connect(reply, &QNetworkReply::finished, this, [reply, done] {
		const QByteArray response = reply->readAll();
		const QString error = reply->error() == QNetworkReply::NoError ? QString() : oauthError(reply, response);
		const QJsonObject json = QJsonDocument::fromJson(response).object();
		reply->deleteLater();
		done(json, error);
	});
}

void OAuthLogin::finishWithIngest(const QString &url, const QString &key, const QString &notice)
{
	if (!url.isEmpty() && !key.isEmpty()) {
		current.ingestUrl = url;
		current.streamKey = key;
	}
	emit finished(current, notice.isEmpty() && (current.ingestUrl.isEmpty() || current.streamKey.isEmpty())
				   ? QStringLiteral("OAuth подключён, но ключ не найден. Укажи RTMP-адрес и ключ вручную")
				   : notice);
}

void OAuthLogin::retrieveIngest(const StreamAccount &account)
{
	busy = true;
	current = account;
	if (current.accessToken.isEmpty()) {
		emit failed(current.id, QStringLiteral("Сначала подключи OAuth"));
		return;
	}
	if (current.expiresAt > 0 && current.expiresAt <= QDateTime::currentSecsSinceEpoch() + 60) {
		if (current.refreshToken.isEmpty()) {
			emit failed(current.id, QStringLiteral("Срок OAuth-токена истёк. Подключи аккаунт заново"));
			return;
		}
		QUrlQuery body;
		body.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
		body.addQueryItem(QStringLiteral("refresh_token"), current.refreshToken);
		body.addQueryItem(QStringLiteral("client_id"), current.clientId);
		if (!current.clientSecret.isEmpty()) {
			body.addQueryItem(QStringLiteral("client_secret"), current.clientSecret);
		}
		QNetworkRequest request(tokenEndpoint(current.platform));
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
		auto *reply = network.post(request, body.query(QUrl::FullyEncoded).toUtf8());
		QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
			const QByteArray response = reply->readAll();
			if (reply->error() != QNetworkReply::NoError) {
				emit failed(current.id, QStringLiteral("Обновление OAuth: %1").arg(oauthError(reply, response)));
				reply->deleteLater();
				return;
			}
			reply->deleteLater();
			const QJsonObject json = QJsonDocument::fromJson(response).object();
			const QString token = json.value(QStringLiteral("access_token")).toString();
			if (token.isEmpty()) {
				emit failed(current.id, QStringLiteral("Не удалось обновить OAuth-токен"));
				return;
			}
			current.accessToken = token;
			current.refreshToken = json.value(QStringLiteral("refresh_token")).toString(current.refreshToken);
			current.expiresAt = QDateTime::currentSecsSinceEpoch() + json.value(QStringLiteral("expires_in")).toInt(3600);
			retrieveIngest(current);
		});
		return;
	}
	if (current.platform == QStringLiteral("twitch")) {
		getJson(QUrl(QStringLiteral("https://api.twitch.tv/helix/users")), [this](const QJsonObject &users, const QString &error) {
			const QString id = firstDataObject(users)
						 .value(QStringLiteral("id")).toString();
			if (!error.isEmpty() || id.isEmpty()) {
				finishWithIngest({}, {}, error);
				return;
			}
			QUrl url(QStringLiteral("https://api.twitch.tv/helix/streams/key"));
			QUrlQuery params;
			params.addQueryItem(QStringLiteral("broadcaster_id"), id);
			url.setQuery(params);
			getJson(url, [this](const QJsonObject &data, const QString &err) {
				const QString key = firstDataObject(data)
						     .value(QStringLiteral("stream_key")).toString();
				finishWithIngest(QStringLiteral("rtmps://live.twitch.tv/app"), key, err);
			});
		});
	} else if (current.platform == QStringLiteral("youtube")) {
		getJson(QUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveStreams?part=cdn&mine=true&maxResults=50")),
			[this](const QJsonObject &result, const QString &error) {
				const QJsonArray streams = result.value(QStringLiteral("items")).toArray();
				for (const QJsonValue &item : streams) {
					const QJsonObject info = item.toObject().value(QStringLiteral("cdn")).toObject()
								       .value(QStringLiteral("ingestionInfo")).toObject();
					const QString key = info.value(QStringLiteral("streamName")).toString();
					const QString url = info.value(QStringLiteral("rtmpsIngestionAddress")).toString(
						info.value(QStringLiteral("ingestionAddress")).toString());
					if (!key.isEmpty() && !url.isEmpty()) {
						finishWithIngest(url, key);
						return;
					}
				}
				finishWithIngest({}, {}, error.isEmpty() ? QStringLiteral("Создай видеопоток в YouTube Studio") : error);
			});
	} else {
		getJson(QUrl(QStringLiteral("https://api.kick.com/public/v1/channels")), [this](const QJsonObject &result, const QString &error) {
			const QJsonObject stream = firstDataObject(result)
						  .value(QStringLiteral("stream")).toObject();
			QString ingestUrl = stream.value(QStringLiteral("url")).toString();
			QUrl ingest(ingestUrl);
			if (ingest.scheme() == QStringLiteral("rtmps") &&
			    ingest.host().endsWith(QStringLiteral("global-contribute.live-video.net"), Qt::CaseInsensitive) &&
			    (ingest.path().isEmpty() || ingest.path() == QStringLiteral("/"))) {
				ingest.setPath(QStringLiteral("/app"));
				ingestUrl = ingest.toString();
			}
			finishWithIngest(ingestUrl,
					 stream.value(QStringLiteral("key")).toString(), error);
		});
	}
}

} // namespace xinkx
