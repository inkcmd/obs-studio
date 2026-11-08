#include "OAuthLogin.hpp"
#include "PlatformApi.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include <memory>

namespace xinkx {

using namespace Qt::StringLiterals;

namespace {

QString randomToken(int size)
{
	QByteArray bytes;
	bytes.reserve(size);
	for (int i = 0; i < size; ++i) {
		bytes.append(static_cast<char>(QRandomGenerator::system()->generate() & 0xff));
	}
	return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QJsonObject firstObject(const QJsonObject &response, const QString &field)
{
	const auto entries = response.value(field).toArray();
	return entries.isEmpty() ? QJsonObject{} : entries.first().toObject();
}

QByteArray encodeForm(const QUrlQuery &query)
{
	QByteArray result;
	for (const auto &[name, value] : query.queryItems(QUrl::FullyDecoded)) {
		if (!result.isEmpty()) {
			result += '&';
		}
		result += QUrl::toPercentEncoding(name) + '=' + QUrl::toPercentEncoding(value);
	}
	return result;
}

} // namespace

OAuthLogin::OAuthLogin(QObject *parent) : QObject(parent)
{
	connect(&callback, &QTcpServer::newConnection, this, &OAuthLogin::receiveCallback);
}

QString OAuthLogin::redirectUri()
{
	return u"http://127.0.0.1:45678/"_s;
}

QString OAuthLogin::Response::error(const QString &operation) const
{
	if (!status) {
		return u"%1: соединение не удалось или истекло время ожидания"_s.arg(operation);
	}
	return u"%1: HTTP %2"_s.arg(operation).arg(status);
}

bool OAuthLogin::begin(const StreamAccount &account, QString *error)
{
	const auto *api = platformApi(account.platform);
	if (busy || !api || account.oauth.clientId.trimmed().isEmpty() ||
	    (api->requiresSecret && account.oauth.clientSecret.isEmpty())) {
		*error = busy ? u"Дождись завершения текущей операции OAuth"_s
			      : u"Проверь Client ID и обязательный для площадки Client Secret"_s;
		return false;
	}
	if (!callback.listen(QHostAddress::LocalHost, 45678)) {
		*error = u"Не удалось открыть локальный OAuth-порт 45678"_s;
		return false;
	}

	current = account;
	state = randomToken(24);
	verifier = api->pkce ? randomToken(48) : QString{};
	busy = true;
	refreshed = false;
	const auto thisAttempt = ++attempt;

	QUrl url = api->authorize;
	QUrlQuery query;
	query.addQueryItem(u"response_type"_s, u"code"_s);
	query.addQueryItem(u"client_id"_s, account.oauth.clientId);
	if (account.platform == u"kick"_s) {
		// Kick's authorize frontend rewrites the first 127.0.0.1 in the query.
		query.addQueryItem(u"redirect"_s, u"127.0.0.1"_s);
	}
	query.addQueryItem(u"redirect_uri"_s, redirectUri());
	query.addQueryItem(u"scope"_s, api->scopes);
	query.addQueryItem(u"state"_s, state);
	if (api->pkce) {
		const auto digest = QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256);
		query.addQueryItem(u"code_challenge"_s, QString::fromLatin1(
			digest.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
		query.addQueryItem(u"code_challenge_method"_s, u"S256"_s);
	}
	if (account.platform == u"youtube"_s) {
		query.addQueryItem(u"access_type"_s, u"offline"_s);
		query.addQueryItem(u"prompt"_s, u"consent select_account"_s);
	} else if (account.platform == u"twitch"_s) {
		query.addQueryItem(u"force_verify"_s, u"true"_s);
	}
	url.setQuery(query);
	if (!QDesktopServices::openUrl(url)) {
		callback.close();
		busy = false;
		*error = u"Не удалось открыть браузер для OAuth"_s;
		return false;
	}
	QTimer::singleShot(180000, this, [this, thisAttempt] {
		if (attempt == thisAttempt && callback.isListening()) {
			fail(u"Время ожидания OAuth истекло"_s);
		}
	});
	return true;
}

void OAuthLogin::receiveCallback()
{
	while (auto *socket = callback.nextPendingConnection()) {
		const auto thisAttempt = attempt;
		const auto buffer = std::make_shared<QByteArray>();
		connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
		QTimer::singleShot(5000, socket, [socket] { socket->disconnectFromHost(); });
		auto read = [this, socket, buffer, thisAttempt] {
			if (attempt != thisAttempt || !callback.isListening()) {
				socket->disconnectFromHost();
				return;
			}
			buffer->append(socket->readAll());
			const auto lineEnd = buffer->indexOf("\r\n");
			if (buffer->size() > 8192) {
				socket->disconnectFromHost();
				return;
			}
			if (lineEnd < 0) {
				return;
			}
			const auto parts = buffer->left(lineEnd).split(' ');
			const auto url = parts.size() == 3 && parts[0] == "GET"
						 ? QUrl::fromEncoded(parts[1], QUrl::StrictMode)
						 : QUrl{};
			const QUrlQuery params(url);
			const bool valid = url.isRelative() && url.path() == u"/"_s &&
					   params.allQueryItemValues(u"state"_s).size() == 1 &&
					   params.queryItemValue(u"state"_s) == state;
			const auto code = params.queryItemValue(u"code"_s, QUrl::FullyDecoded);
			const auto providerError = params.queryItemValue(u"error"_s);
			const bool accepted = valid && !code.isEmpty() && providerError.isEmpty();
			const QByteArray body = accepted ? "Return to XINKX Studio to complete authorization."
							  : "Authorization request was rejected.";
			socket->write((accepted ? QByteArray("HTTP/1.1 200 OK\r\n") : QByteArray("HTTP/1.1 400 Bad Request\r\n")) +
				      "Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n"
				      "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) +
				      "\r\n\r\n" + body);
			socket->disconnectFromHost();
			if (!valid) {
				return;
			}
			callback.close();
			if (accepted) {
				exchangeCode(code);
			} else {
				fail(u"Площадка отклонила авторизацию или не вернула код"_s);
			}
		};
		connect(socket, &QTcpSocket::readyRead, this, read);
		if (socket->bytesAvailable()) {
			read();
		}
	}
}

void OAuthLogin::request(const QUrl &url, const QUrlQuery *form, bool authorized, const Done &done)
{
	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	req.setRawHeader("Accept", "application/json");
	if (authorized) {
		req.setRawHeader("Authorization", QByteArray("Bearer ") + current.oauth.accessToken.toUtf8());
		if (current.platform == u"twitch"_s) {
			req.setRawHeader("Client-Id", current.oauth.clientId.toUtf8());
		}
	}
	QNetworkReply *reply;
	if (form) {
		req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_s);
		reply = network.post(req, encodeForm(*form));
	} else {
		reply = network.get(req);
	}
	connect(reply, &QNetworkReply::finished, this, [reply, done] {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const bool ok = status >= 200 && status < 300 && reply->error() == QNetworkReply::NoError;
		const auto json = QJsonDocument::fromJson(reply->readAll()).object();
		reply->deleteLater();
		done({json, status, ok});
	});
}

void OAuthLogin::exchangeCode(const QString &code)
{
	QUrlQuery form;
	form.addQueryItem(u"grant_type"_s, u"authorization_code"_s);
	form.addQueryItem(u"redirect_uri"_s, redirectUri());
	form.addQueryItem(u"code"_s, code);
	if (!verifier.isEmpty()) {
		form.addQueryItem(u"code_verifier"_s, verifier);
	}
	requestToken(form, true, [this] { identify(); });
}

void OAuthLogin::requestToken(QUrlQuery form, bool replacing, const std::function<void()> &done)
{
	form.addQueryItem(u"client_id"_s, current.oauth.clientId);
	if (!current.oauth.clientSecret.isEmpty()) {
		form.addQueryItem(u"client_secret"_s, current.oauth.clientSecret);
	}
	request(platformApi(current.platform)->token, &form, false, [this, replacing, done](const Response &response) {
		if (!response.ok) {
			const auto error = response.json.value(u"error"_s).toString();
			if (!replacing && (error == u"invalid_grant"_s || response.status == 401 ||
					   (current.platform == u"twitch"_s && response.status == 400))) {
				current.clearAuthorization();
				complete(u"Обновление отклонено. Подключи аккаунт заново"_s, false);
			} else {
				fail(response.error(replacing ? u"Авторизация"_s : u"Обновление OAuth"_s));
			}
			return;
		}
		const auto token = response.json.value(u"access_token"_s).toString();
		if (token.isEmpty()) {
			fail(u"Платформа не вернула OAuth-токен"_s);
			return;
		}
		if (replacing) {
			current.clearAuthorization();
		}
		current.oauth.accessToken = token;
		current.oauth.refreshToken = response.json.value(u"refresh_token"_s).toString(current.oauth.refreshToken);
		const qint64 expires = response.json.value(u"expires_in"_s).toVariant().toLongLong();
		current.oauth.expiresAt = expires > 0 ? QDateTime::currentSecsSinceEpoch() + expires : 0;
		done();
	});
}

void OAuthLogin::refresh(const std::function<void()> &done)
{
	refreshed = true;
	if (current.oauth.refreshToken.isEmpty()) {
		current.clearAuthorization();
		complete(u"Сессия OAuth истекла. Подключи аккаунт заново"_s, false);
		return;
	}
	QUrlQuery form;
	form.addQueryItem(u"grant_type"_s, u"refresh_token"_s);
	form.addQueryItem(u"refresh_token"_s, current.oauth.refreshToken);
	requestToken(form, false, done);
}

void OAuthLogin::getJson(const QUrl &url, const Done &done)
{
	request(url, nullptr, true, [this, url, done](const Response &response) {
		if (response.status == 401) {
			if (!refreshed) {
				refresh([this, url, done] { getJson(url, done); });
			} else {
				current.clearAuthorization();
				complete(u"Токен отклонён. Подключи аккаунт заново"_s, false);
			}
			return;
		}
		done(response);
	});
}

void OAuthLogin::retrieveIngest(const StreamAccount &account)
{
	if (busy || !platformApi(account.platform)) {
		return;
	}
	current = account;
	busy = true;
	refreshed = false;
	if (current.oauth.accessToken.isEmpty() && current.oauth.refreshToken.isEmpty()) {
		fail(u"Сначала подключи OAuth"_s);
	} else if (current.oauth.requiresAuthorization()) {
		refresh([this] { identify(); });
	} else {
		identify();
	}
}

void OAuthLogin::identify()
{
	getJson(platformApi(current.platform)->identity, [this](const Response &response) {
		if (!response.ok) {
			complete(response.error(u"Получение имени аккаунта"_s), false);
			return;
		}
		const bool youtube = current.platform == u"youtube"_s;
		const auto user = firstObject(response.json, youtube ? u"items"_s : u"data"_s);
		const auto id = user.value(current.platform == u"kick"_s ? u"user_id"_s : u"id"_s).toVariant().toString();
		if (id.isEmpty()) {
			complete(u"Платформа не вернула идентификатор аккаунта"_s, false);
			return;
		}
		if (!current.providerId.isEmpty() && current.providerId != id && current.destination.fromOAuth) {
			current.destination.ingestUrl.clear();
			current.destination.streamKey.clear();
			current.destination.fromOAuth = false;
		}
		current.providerId = id;
		current.providerName = youtube ? user.value(u"snippet"_s).toObject().value(u"title"_s).toString()
					       : user.value(current.platform == u"kick"_s ? u"name"_s : u"display_name"_s).toString();
		fetchIngest();
	});
}

void OAuthLogin::fetchIngest()
{
	if (current.platform == u"twitch"_s) {
		QUrl url(u"https://api.twitch.tv/helix/streams/key"_s);
		QUrlQuery query;
		query.addQueryItem(u"broadcaster_id"_s, current.providerId);
		url.setQuery(query);
		getJson(url, [this](const Response &response) {
			finishWithIngest(u"rtmps://live.twitch.tv/app"_s,
					 firstObject(response.json, u"data"_s).value(u"stream_key"_s).toString(),
					 response.ok ? QString{} : response.error(u"Ключ Twitch"_s));
		});
	} else if (current.platform == u"youtube"_s) {
		QUrl url(u"https://www.googleapis.com/youtube/v3/liveStreams"_s);
		QUrlQuery query;
		query.addQueryItem(u"part"_s, u"id,cdn"_s);
		if (current.youtubeStreamId.isEmpty()) {
			query.addQueryItem(u"mine"_s, u"true"_s);
			query.addQueryItem(u"maxResults"_s, u"2"_s);
		} else {
			query.addQueryItem(u"id"_s, current.youtubeStreamId);
		}
		url.setQuery(query);
		getJson(url, [this](const Response &response) {
			const auto items = response.json.value(u"items"_s).toArray();
			if (!response.ok || items.size() != 1 || response.json.contains(u"nextPageToken"_s)) {
				complete(response.ok ? u"Укажи ID нужного видеопотока YouTube или введи RTMP вручную"_s
						     : response.error(u"Видеопоток YouTube"_s), false);
				return;
			}
			const auto info = items.first().toObject().value(u"cdn"_s).toObject().value(u"ingestionInfo"_s).toObject();
			finishWithIngest(info.value(u"rtmpsIngestionAddress"_s).toString(info.value(u"ingestionAddress"_s).toString()),
					 info.value(u"streamName"_s).toString(), {});
		});
	} else {
		QUrl url(u"https://api.kick.com/public/v1/channels"_s);
		QUrlQuery query;
		query.addQueryItem(u"broadcaster_user_id"_s, current.providerId);
		url.setQuery(query);
		getJson(url, [this](const Response &response) {
			const auto stream = firstObject(response.json, u"data"_s).value(u"stream"_s).toObject();
			auto url = QUrl(stream.value(u"url"_s).toString());
			if (url.scheme() == u"rtmps"_s && url.host().endsWith(u".global-contribute.live-video.net"_s) &&
			    (url.path().isEmpty() || url.path() == u"/"_s)) {
				url.setPath(u"/app"_s);
			}
			finishWithIngest(url.toString(), stream.value(u"key"_s).toString(),
					 response.ok ? QString{} : response.error(u"Ключ Kick"_s));
		});
	}
}

void OAuthLogin::finishWithIngest(const QString &url, const QString &key, const QString &notice)
{
	if (validIngestUrl(url) && !key.isEmpty()) {
		current.destination.ingestUrl = url;
		current.destination.streamKey = key;
		current.destination.fromOAuth = true;
		complete(u"Аккаунт подключён, параметры трансляции получены"_s);
	} else {
		complete(notice.isEmpty() ? u"Ключ недоступен. Укажи параметры RTMP(S) вручную"_s : notice, false);
	}
}

void OAuthLogin::revokeToken(const QString &token, bool refreshToken, const Done &done)
{
	auto url = platformApi(current.platform)->revoke;
	QUrlQuery form;
	form.addQueryItem(u"token"_s, token);
	if (current.platform == u"kick"_s) {
		form.addQueryItem(u"token_hint_type"_s, refreshToken ? u"refresh_token"_s : u"access_token"_s);
		url.setQuery(form);
		form.clear();
	} else if (current.platform == u"twitch"_s) {
		form.addQueryItem(u"client_id"_s, current.oauth.clientId);
	}
	request(url, &form, false, done);
}

void OAuthLogin::revoke(const StreamAccount &account)
{
	if (busy || !platformApi(account.platform)) {
		return;
	}
	current = account;
	busy = true;
	const auto done = [this](const Response &response) {
		if (response.ok || (response.status == 400 && response.json.value(u"error"_s) == u"invalid_token"_s)) {
			current.clearAuthorization();
			complete(u"OAuth отключён"_s);
		} else {
			fail(response.error(u"Отзыв OAuth"_s));
		}
	};
	const auto access = current.oauth.accessToken;
	const auto refreshToken = current.oauth.refreshToken;
	if (!refreshToken.isEmpty() && current.platform == u"kick"_s) {
		revokeToken(refreshToken, true, [this, access, done](const Response &response) {
			if (response.ok && !access.isEmpty()) {
				revokeToken(access, false, done);
			} else {
				done(response);
			}
		});
	} else if (!refreshToken.isEmpty() && current.platform == u"youtube"_s) {
		revokeToken(refreshToken, true, done);
	} else if (!access.isEmpty()) {
		revokeToken(access, false, done);
	} else {
		current.clearAuthorization();
		complete(u"Локальная сессия OAuth отключена"_s);
	}
}

void OAuthLogin::complete(const QString &notice, bool success)
{
	busy = false;
	emit finished(current, notice, success);
}

void OAuthLogin::fail(const QString &notice)
{
	callback.close();
	busy = false;
	emit failed(current.id, notice);
}

} // namespace xinkx
