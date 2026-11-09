#include "ChatGateway.hpp"

#include "NativeWebSocket.hpp"
#include "OverlayRuntime.hpp"
#include "PlatformApi.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/RemoteTextThread.hpp>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xinkx {

namespace {

QString bounded(const QString &value, int length = 240)
{
	QString text = value.left(length);
	text.remove(QLatin1Char('\0'));
	return text;
}

QString endpointError(const QJsonObject &json, int status, const QString &transport)
{
	const auto error = json.value(QStringLiteral("error"));
	const QString detail = error.isObject() ? error.toObject().value(QStringLiteral("message")).toString()
						 : error.toString();
	const QString message = json.value(QStringLiteral("message")).toString();
	if (!detail.isEmpty() || !message.isEmpty()) {
		return QStringLiteral("HTTP %1: %2").arg(status).arg(bounded(!detail.isEmpty() ? detail : message, 150));
	}
	return status ? QStringLiteral("HTTP %1").arg(status) : bounded(transport, 150);
}

QString apiUrl(const QString &root, const QList<QPair<QString, QString>> &params)
{
	QUrl url(root);
	QUrlQuery query;
	for (const auto &param : params) query.addQueryItem(param.first, param.second);
	url.setQuery(query);
	return url.toString(QUrl::FullyEncoded);
}

} // namespace

ChatGateway::ChatGateway(OBSBasic *window) : QObject(window), mainWindow(window)
{
	reloadAccounts();
	connect(&tick, &QTimer::timeout, this, &ChatGateway::schedule);
	tick.setInterval(1000);
	tick.start();
}

ChatGateway::~ChatGateway()
{
	for (auto *socket : twitch) delete socket;
	twitch.clear();
}

QVector<StreamAccount> ChatGateway::accounts() const
{
	QVector<StreamAccount> result;
	for (const auto &account : loadStreamAccounts(false)) {
		if (account.platform == QStringLiteral("twitch") || account.platform == QStringLiteral("youtube") ||
		    account.platform == QStringLiteral("kick")) result.push_back(account);
	}
	return result;
}

void ChatGateway::status(const QString &id, const QString &message)
{
	emit providerStatus(id, bounded(message, 180));
}

void ChatGateway::reloadAccounts()
{
	QHash<QString, StreamAccount> updated;
	for (const auto &account : loadStreamAccounts()) {
		if (account.platform == QStringLiteral("twitch") || account.platform == QStringLiteral("youtube") ||
		    account.platform == QStringLiteral("kick")) {
			updated.insert(account.id, account);
		}
	}
	for (auto it = twitch.begin(); it != twitch.end();) {
		const auto account = updated.value(it.key());
		if (account.id.isEmpty() || account.oauth.accessToken.isEmpty() ||
		    account.providerId != known.value(it.key()).providerId ||
		    account.oauth.accessToken != known.value(it.key()).oauth.accessToken ||
		    account.oauth.clientId != known.value(it.key()).oauth.clientId) {
			delete it.value();
			it = twitch.erase(it);
		} else ++it;
	}
	for (auto it = youtube.begin(); it != youtube.end();) {
		if (!updated.contains(it.key()) ||
		    updated[it.key()].providerId != known.value(it.key()).providerId) it = youtube.erase(it);
		else ++it;
	}
	known = updated;
	for (const auto &account : updated) {
		if (account.platform == QStringLiteral("youtube") && !youtube.contains(account.id)) {
			youtube.insert(account.id, {});
		}
		if (account.platform == QStringLiteral("kick")) {
			status(account.id, QStringLiteral("Отправка работает; получение Kick требует отдельного канала событий"));
		}
	}
	nextReloadAt = QDateTime::currentMSecsSinceEpoch() + 30000;
}

void ChatGateway::schedule()
{
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (now >= nextReloadAt) reloadAccounts();
	for (auto it = youtube.begin(); it != youtube.end(); ++it) {
		if (!it->pending && now >= it->nextAt) pollYouTube(it.key());
	}
	for (const auto &account : known) {
		if (account.platform == QStringLiteral("twitch") && !twitch.contains(account.id) &&
		    now >= nextTwitchAt.value(account.id)) startTwitch(account.id);
	}
}

void ChatGateway::request(const QString &id, const QString &url, const QByteArray &body,
			  const QByteArray &contentType, const RequestDone &done, bool useToken)
{
	if (!known.contains(id)) return;
	std::vector<std::string> headers{"Accept: application/json"};
	if (useToken) {
		const auto &account = known[id];
		headers.push_back("Authorization: Bearer " + account.oauth.accessToken.toStdString());
		if (account.platform == QStringLiteral("twitch")) {
			headers.push_back("Client-Id: " + account.oauth.clientId.toStdString());
		}
	}
	const auto address = url.toStdString();
	const auto payload = body.toStdString();
	const auto mime = contentType.toStdString();
	struct Transfer {
		std::string response;
		std::string error;
		long status = 0;
		bool ok = false;
	};
	const auto transfer = std::make_shared<Transfer>();
	auto *worker = QThread::create([transfer, address, payload, mime, headers = std::move(headers)] {
		const bool post = !mime.empty();
		transfer->ok = GetRemoteFile(address.c_str(), transfer->response, transfer->error, &transfer->status,
					     post ? mime.c_str() : nullptr, post ? "POST" : "GET",
					     post ? payload.c_str() : nullptr, headers, nullptr, 20, false,
					     static_cast<int>(payload.size()));
	});
	connect(worker, &QThread::finished, this, [this, transfer, id, done] {
		if (!known.contains(id)) return;
		const int code = static_cast<int>(transfer->status);
		const auto json = QJsonDocument::fromJson(QByteArray::fromStdString(transfer->response)).object();
		const bool success = transfer->ok && code >= 200 && code < 300;
		const auto error = success ? QString{} : endpointError(json, code, QString::fromStdString(transfer->error));
		done({json, code, success, error});
	});
	connect(worker, &QThread::finished, worker, &QObject::deleteLater);
	worker->start();
}

void ChatGateway::tokenFor(const QString &id, const TokenDone &done, bool force)
{
	const auto account = known.value(id);
	if (account.id.isEmpty()) return;
	if (!force && !account.oauth.accessToken.isEmpty() &&
	    (!account.oauth.expiresAt || account.oauth.expiresAt > QDateTime::currentSecsSinceEpoch() + 90)) {
		done(account.oauth.accessToken);
		return;
	}
	waitingForToken[id].append(done);
	if (!refreshing.contains(id)) refresh(id);
}

void ChatGateway::refresh(const QString &id)
{
	const auto account = known.value(id);
	if (account.oauth.refreshToken.isEmpty() || !platformApi(account.platform)) {
		const auto callbacks = waitingForToken.take(id);
		for (const auto &done : callbacks) done({});
		status(id, QStringLiteral("OAuth истёк. Переподключи аккаунт в настройках трансляции"));
		return;
	}
	refreshing.insert(id);
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
	query.addQueryItem(QStringLiteral("refresh_token"), account.oauth.refreshToken);
	query.addQueryItem(QStringLiteral("client_id"), account.oauth.clientId);
	if (!account.oauth.clientSecret.isEmpty()) {
		query.addQueryItem(QStringLiteral("client_secret"), account.oauth.clientSecret);
	}
	request(id, platformApi(account.platform)->token.toString(), query.toString(QUrl::FullyEncoded).toUtf8(),
		QByteArray("application/x-www-form-urlencoded"), [this, id](const HttpResult &result) {
			refreshing.remove(id);
			if (result.ok && !result.json.value(QStringLiteral("access_token")).toString().isEmpty()) {
				saveRefreshed(id, result.json);
			} else {
				status(id, QStringLiteral("Не удалось обновить OAuth: ") + result.error);
				known[id].oauth.accessToken.clear();
			}
			const auto callbacks = waitingForToken.take(id);
			const QString token = known.value(id).oauth.accessToken;
			for (const auto &done : callbacks) done(token);
		}, false);
}

void ChatGateway::saveRefreshed(const QString &id, const QJsonObject &json)
{
	auto account = known.value(id);
	const auto oldRefresh = account.oauth.refreshToken;
	account.oauth.accessToken = json.value(QStringLiteral("access_token")).toString();
	account.oauth.refreshToken = json.value(QStringLiteral("refresh_token")).toString(oldRefresh);
	const auto expires = json.value(QStringLiteral("expires_in")).toVariant().toLongLong();
	account.oauth.expiresAt = expires > 0 ? QDateTime::currentSecsSinceEpoch() + expires : 0;
	auto stored = loadStreamAccounts();
	for (auto &entry : stored) {
		if (entry.id != id) continue;
		// Never overwrite a newer OAuth login or changes to the account editor.
		if (entry.oauth.refreshToken != oldRefresh) return;
		entry.oauth = account.oauth;
		if (saveStreamAccounts(stored)) known[id].oauth = account.oauth;
		else status(id, QStringLiteral("Не удалось сохранить обновлённые OAuth-токены"));
		return;
	}
}

void ChatGateway::authorized(const QString &id, const QString &url, const QByteArray &body,
			     const RequestDone &done, bool retry)
{
	tokenFor(id, [this, id, url, body, done, retry](const QString &token) {
		if (token.isEmpty()) {
			done({{}, 0, false, QStringLiteral("Требуется OAuth")});
			return;
		}
		request(id, url, body, body.isNull() ? QByteArray{} : QByteArray("application/json"),
			[this, id, url, body, done, retry](const HttpResult &response) {
				if (response.status == 401 && !retry) {
					tokenFor(id, [this, id, url, body, done](const QString &newToken) {
						if (newToken.isEmpty()) done({{}, 401, false, QStringLiteral("OAuth отклонён")});
						else authorized(id, url, body, done, true);
					}, true);
					return;
				}
				done(response);
			});
	});
}

void ChatGateway::pollYouTube(const QString &id)
{
	if (!known.contains(id)) return;
	auto &state = youtube[id];
	if (state.pending) return;
	state.pending = true;
	state.nextAt = QDateTime::currentMSecsSinceEpoch() + 60000;
	if (!state.chatId.isEmpty()) {
		pollYouTubeMessages(id);
		return;
	}
	status(id, QStringLiteral("YouTube: ищем активную трансляцию"));
	const QString url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"),
		{{QStringLiteral("part"), QStringLiteral("snippet,status")},
		 {QStringLiteral("broadcastStatus"), QStringLiteral("active")},
		 {QStringLiteral("maxResults"), QStringLiteral("50")}});
	authorized(id, url, {}, [this, id](const HttpResult &result) {
		if (!youtube.contains(id)) return;
		if (!result.ok) {
			youtube[id].pending = false;
			status(id, QStringLiteral("YouTube: ") + result.error);
			return;
		}
		QString chatId;
		for (const auto &value : result.json.value(QStringLiteral("items")).toArray()) {
			chatId = value.toObject().value(QStringLiteral("snippet")).toObject()
					 .value(QStringLiteral("liveChatId")).toString();
			if (!chatId.isEmpty()) break;
		}
		if (chatId.isEmpty()) {
			youtube[id].pending = false;
			status(id, QStringLiteral("YouTube: активного эфира с чатом пока нет"));
			return;
		}
		youtube[id].chatId = chatId;
		youtube[id].nextPage.clear();
		youtube[id].baseline = false;
		pollYouTubeMessages(id);
	});
}

void ChatGateway::pollYouTubeMessages(const QString &id)
{
	if (!youtube.contains(id)) return;
	const auto state = youtube.value(id);
	QList<QPair<QString, QString>> parameters{
		{QStringLiteral("liveChatId"), state.chatId},
		{QStringLiteral("part"), QStringLiteral("id,snippet,authorDetails")},
		{QStringLiteral("maxResults"), QStringLiteral("200")},
	};
	if (!state.nextPage.isEmpty()) parameters.append({QStringLiteral("pageToken"), state.nextPage});
	const auto url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"), parameters);
	authorized(id, url, {}, [this, id](const HttpResult &result) {
		if (!youtube.contains(id)) return;
		auto &state = youtube[id];
		state.pending = false;
		if (!result.ok) {
			state.nextAt = QDateTime::currentMSecsSinceEpoch() + 60000;
			status(id, QStringLiteral("YouTube чат: ") + result.error);
			if (result.status == 404 || result.status == 403) {
				state.chatId.clear();
				state.nextPage.clear();
				state.baseline = false;
			}
			return;
		}
		if (state.baseline) handleYouTubePage(id, result.json);
		else state.baseline = true; // Do not flood the scene with pre-existing chat history.
		state.nextPage = result.json.value(QStringLiteral("nextPageToken")).toString(state.nextPage);
		if (result.json.contains(QStringLiteral("offlineAt"))) {
			state.chatId.clear(); state.nextPage.clear(); state.baseline = false;
			state.nextAt = QDateTime::currentMSecsSinceEpoch() + 60000;
			status(id, QStringLiteral("YouTube: эфир завершён"));
			return;
		}
		const int interval = result.json.value(QStringLiteral("pollingIntervalMillis")).toInt(10000);
		state.nextAt = QDateTime::currentMSecsSinceEpoch() + std::clamp(interval, 5000, 120000);
		status(id, QStringLiteral("YouTube: чат · обновление %1 с").arg(std::clamp(interval, 5000, 120000) / 1000));
	});
}

void ChatGateway::handleYouTubePage(const QString &id, const QJsonObject &page)
{
	for (const auto &value : page.value(QStringLiteral("items")).toArray()) {
		const auto item = value.toObject();
		const auto snippet = item.value(QStringLiteral("snippet")).toObject();
		const auto type = snippet.value(QStringLiteral("type")).toString();
		if (type != QStringLiteral("textMessageEvent") && type != QStringLiteral("superChatEvent") &&
		    type != QStringLiteral("superStickerEvent")) continue;
		ChatLine line;
		line.platform = QStringLiteral("youtube");
		line.accountId = id;
		line.messageId = item.value(QStringLiteral("id")).toString();
		line.name = item.value(QStringLiteral("authorDetails")).toObject()
				.value(QStringLiteral("displayName")).toString(QStringLiteral("Зритель"));
		line.text = snippet.value(QStringLiteral("displayMessage")).toString(
				snippet.value(QStringLiteral("textMessageDetails")).toObject()
				.value(QStringLiteral("messageText")).toString());
		if (type != QStringLiteral("textMessageEvent")) {
			line.type = QStringLiteral("donation");
			const auto details = snippet.value(type == QStringLiteral("superChatEvent")
					? QStringLiteral("superChatDetails") : QStringLiteral("superStickerDetails")).toObject();
			line.amount = details.value(QStringLiteral("amountDisplayString")).toString();
			line.text = details.value(QStringLiteral("userComment")).toString(line.text);
		}
		dispatch(std::move(line));
	}
}

void ChatGateway::startTwitch(const QString &id)
{
	const auto account = known.value(id);
	if (account.oauth.accessToken.isEmpty() && account.oauth.refreshToken.isEmpty()) {
		status(id, QStringLiteral("Twitch: подключи OAuth"));
		nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 30000;
		return;
	}
	if (account.providerId.isEmpty()) {
		status(id, QStringLiteral("Twitch: обнови данные аккаунта"));
		nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 30000;
		return;
	}
	status(id, QStringLiteral("Twitch: подключаем EventSub"));
	twitch[id] = new NativeWebSocket(this, QUrl(QStringLiteral("wss://eventsub.wss.twitch.tv/ws?keepalive_timeout_seconds=30")),
		[this, id](const QByteArray &frame) { handleTwitchFrame(id, frame); },
		[this, id](const QString &reason) {
			status(id, QStringLiteral("Twitch: ") + reason);
			nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 15000;
			delete twitch.take(id);
		});
}

void ChatGateway::subscribeTwitch(const QString &id, const QString &sessionId)
{
	const QString userId = known.value(id).providerId;
	if (userId.isEmpty() || sessionId.isEmpty()) return;
	const auto subscribe = [this, id, sessionId](const QString &type, const QString &version,
						    const QJsonObject &condition) {
		const QJsonObject body{
			{QStringLiteral("type"), type}, {QStringLiteral("version"), version},
			{QStringLiteral("condition"), condition},
			{QStringLiteral("transport"), QJsonObject{
				{QStringLiteral("method"), QStringLiteral("websocket")},
				{QStringLiteral("session_id"), sessionId}}},
		};
		authorized(id, QStringLiteral("https://api.twitch.tv/helix/eventsub/subscriptions"),
			QJsonDocument(body).toJson(QJsonDocument::Compact),
			[this, id, type](const HttpResult &result) {
				if (!result.ok) {
					status(id, QStringLiteral("Twitch %1: %2").arg(type, result.error));
					if (type == QStringLiteral("channel.chat.message")) {
						nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 60000;
						delete twitch.take(id);
					}
				}
				else if (type == QStringLiteral("channel.chat.message")) {
					status(id, QStringLiteral("Twitch: чат подключён · EventSub"));
				}
			});
	};
	subscribe(QStringLiteral("channel.chat.message"), QStringLiteral("1"),
		{{QStringLiteral("broadcaster_user_id"), userId}, {QStringLiteral("user_id"), userId}});
	subscribe(QStringLiteral("channel.follow"), QStringLiteral("2"),
		{{QStringLiteral("broadcaster_user_id"), userId}, {QStringLiteral("moderator_user_id"), userId}});
	subscribe(QStringLiteral("channel.subscribe"), QStringLiteral("1"), {{QStringLiteral("broadcaster_user_id"), userId}});
	subscribe(QStringLiteral("channel.subscription.message"), QStringLiteral("1"),
		{{QStringLiteral("broadcaster_user_id"), userId}});
	subscribe(QStringLiteral("channel.subscription.gift"), QStringLiteral("1"),
		{{QStringLiteral("broadcaster_user_id"), userId}});
	subscribe(QStringLiteral("channel.raid"), QStringLiteral("1"), {{QStringLiteral("to_broadcaster_user_id"), userId}});
	subscribe(QStringLiteral("channel.cheer"), QStringLiteral("1"), {{QStringLiteral("broadcaster_user_id"), userId}});
}

void ChatGateway::handleTwitchFrame(const QString &id, const QByteArray &frame)
{
	if (!known.contains(id) || !twitch.contains(id)) return;
	const auto root = QJsonDocument::fromJson(frame).object();
	const auto metadata = root.value(QStringLiteral("metadata")).toObject();
	const auto messageType = metadata.value(QStringLiteral("message_type")).toString();
	const auto payload = root.value(QStringLiteral("payload")).toObject();
	if (messageType == QStringLiteral("session_welcome")) {
		subscribeTwitch(id, payload.value(QStringLiteral("session")).toObject()
					 .value(QStringLiteral("id")).toString());
		return;
	}
	if (messageType == QStringLiteral("session_reconnect") || messageType == QStringLiteral("revocation")) {
		status(id, QStringLiteral("Twitch: переподключение EventSub"));
		delete twitch.take(id);
		nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 1000;
		return;
	}
	if (messageType != QStringLiteral("notification")) return;
	const auto subtype = payload.value(QStringLiteral("subscription")).toObject()
				 .value(QStringLiteral("type")).toString();
	const auto event = payload.value(QStringLiteral("event")).toObject();
	ChatLine line;
	line.platform = QStringLiteral("twitch");
	line.accountId = id;
	line.messageId = metadata.value(QStringLiteral("message_id")).toString();
	line.name = event.value(QStringLiteral("chatter_user_name")).toString(
			event.value(QStringLiteral("user_name")).toString(QStringLiteral("Зритель")));
	if (subtype == QStringLiteral("channel.chat.message")) {
		line.text = event.value(QStringLiteral("message")).toObject().value(QStringLiteral("text")).toString();
	} else if (subtype == QStringLiteral("channel.follow")) {
		line.type = QStringLiteral("follow");
	} else if (subtype == QStringLiteral("channel.subscribe") ||
		   subtype == QStringLiteral("channel.subscription.message") ||
		   subtype == QStringLiteral("channel.subscription.gift")) {
		line.type = QStringLiteral("sub");
		line.text = event.value(QStringLiteral("message")).toObject().value(QStringLiteral("text")).toString();
	} else if (subtype == QStringLiteral("channel.raid")) {
		line.type = QStringLiteral("raid");
		line.name = event.value(QStringLiteral("from_broadcaster_user_name")).toString(line.name);
		line.text = QStringLiteral("Зрителей: %1").arg(event.value(QStringLiteral("viewers")).toInt());
	} else if (subtype == QStringLiteral("channel.cheer")) {
		line.type = QStringLiteral("donation");
		line.amount = QStringLiteral("%1 Bits").arg(event.value(QStringLiteral("bits")).toInt());
		line.text = event.value(QStringLiteral("message")).toString();
	} else return;
	dispatch(std::move(line));
}

void ChatGateway::dispatch(ChatLine line)
{
	line.text = bounded(line.text);
	line.name = bounded(line.name, 80);
	if (line.type == QStringLiteral("chat") && line.text.trimmed().isEmpty()) return;
	const auto key = line.accountId + QLatin1Char(':') + line.messageId;
	if (!line.messageId.isEmpty()) {
		if (seenEvents.contains(key)) return;
		seenEvents.insert(key);
		seenOrder.append(key);
		if (seenOrder.size() > 1000) seenEvents.remove(seenOrder.takeFirst());
	}
	line.receivedAt = QDateTime::currentMSecsSinceEpoch();
	emit incoming(line);
	if (auto *overlay = mainWindow->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime"))) {
		if (line.type == QStringLiteral("chat")) overlay->publishChat(line.platform, line.name, line.text);
		else overlay->publishAlert(line.type, line.platform, line.name, line.amount, line.text);
	}
}

void ChatGateway::sendMessage(const QString &message, const QStringList &accountIds)
{
	const QString text = message.trimmed();
	if (text.isEmpty() || text.size() > 200) {
		for (const auto &id : accountIds) emit sendStatus(id, QStringLiteral("Сообщение: 1–200 символов"), false);
		return;
	}
	for (const auto &id : accountIds) {
		const auto account = known.value(id);
		if (account.id.isEmpty() || sending.contains(id)) {
			emit sendStatus(id, QStringLiteral("Аккаунт недоступен или отправка выполняется"), false);
			continue;
		}
		QString url;
		QJsonObject body;
		if (account.platform == QStringLiteral("twitch") && !account.providerId.isEmpty()) {
			url = QStringLiteral("https://api.twitch.tv/helix/chat/messages");
			body = {{QStringLiteral("broadcaster_id"), account.providerId},
				{QStringLiteral("sender_id"), account.providerId},
				{QStringLiteral("message"), text}};
		} else if (account.platform == QStringLiteral("youtube") && !youtube.value(id).chatId.isEmpty()) {
			url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"),
				{{QStringLiteral("part"), QStringLiteral("snippet")}});
			body = {{QStringLiteral("snippet"), QJsonObject{
				{QStringLiteral("liveChatId"), youtube.value(id).chatId},
				{QStringLiteral("type"), QStringLiteral("textMessageEvent")},
				{QStringLiteral("textMessageDetails"), QJsonObject{{QStringLiteral("messageText"), text}}}}}};
		} else if (account.platform == QStringLiteral("kick") && !account.providerId.isEmpty()) {
			bool validId = false;
			const qint64 broadcaster = account.providerId.toLongLong(&validId);
			if (validId && broadcaster > 0) {
				url = QStringLiteral("https://api.kick.com/public/v1/chat");
				body = {{QStringLiteral("type"), QStringLiteral("user")},
					{QStringLiteral("broadcaster_user_id"), broadcaster},
					{QStringLiteral("content"), text}};
			}
		}
		if (url.isEmpty()) {
			emit sendStatus(id, QStringLiteral("Нет данных канала или активного YouTube-чата"), false);
			continue;
		}
		sending.insert(id);
		authorized(id, url, QJsonDocument(body).toJson(QJsonDocument::Compact),
			[this, id](const HttpResult &result) {
				sending.remove(id);
				bool ok = result.ok;
				if (known.value(id).platform == QStringLiteral("twitch") && result.ok) {
					const auto data = result.json.value(QStringLiteral("data")).toArray();
					ok = !data.isEmpty() && data.first().toObject().value(QStringLiteral("is_sent")).toBool();
				} else if (known.value(id).platform == QStringLiteral("kick") && result.ok) {
					ok = result.json.value(QStringLiteral("is_sent")).toBool(false);
				}
				emit sendStatus(id, ok ? QStringLiteral("Сообщение отправлено") :
					result.ok ? QStringLiteral("Платформа не подтвердила отправку") : result.error, ok);
			});
	}
}

} // namespace xinkx
