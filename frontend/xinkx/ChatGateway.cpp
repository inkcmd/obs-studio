#include "ChatGateway.hpp"

#include "NativeWebSocket.hpp"
#include "OverlayRuntime.hpp"
#include "PlatformApi.hpp"
#include "PresentationSettings.hpp"
#include "StudioDiagnostics.hpp"
#include "YouTubeGrpcStream.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/RemoteTextThread.hpp>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

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

bool quotaExceeded(const QJsonObject &json)
{
	const QJsonObject error = json.value(QStringLiteral("error")).toObject();
	for (const auto &item : error.value(QStringLiteral("errors")).toArray()) {
		const QString reason = item.toObject().value(QStringLiteral("reason")).toString();
		if (reason == QStringLiteral("quotaExceeded") || reason == QStringLiteral("dailyLimitExceeded")) return true;
	}
	const QString message = error.value(QStringLiteral("message")).toString();
	return message.contains(QStringLiteral("exceeded your"), Qt::CaseInsensitive) &&
		(message.contains(QStringLiteral("quota"), Qt::CaseInsensitive) ||
		 message.contains(QStringLiteral("/youtube/v3/getting-started")));
}

bool tagsMatch(const QJsonArray &remote, const QStringList &expected)
{
	QStringList actual;
	for (const QJsonValue &item : remote) {
		if (!item.isString()) return false;
		actual << item.toString();
	}
	if (actual.size() != expected.size()) return false;
	QStringList sorted = expected;
	actual.sort(Qt::CaseInsensitive);
	sorted.sort(Qt::CaseInsensitive);
	for (int i = 0; i < sorted.size(); ++i)
		if (actual.at(i).compare(sorted.at(i), Qt::CaseInsensitive) != 0) return false;
	return true;
}

qint64 nextYoutubeQuotaReset()
{
	const QTimeZone pacific("America/Los_Angeles");
	const QDateTime now = QDateTime::currentDateTime().toTimeZone(pacific);
	return QDateTime(now.date().addDays(1), QTime(0, 0), pacific).toMSecsSinceEpoch();
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
	for (auto *socket : twitchReconnect) delete socket;
	twitchReconnect.clear();
	for (auto *stream : youtubeStreams) delete stream;
	youtubeStreams.clear();
	for (auto *socket : kickSockets) delete socket;
	kickSockets.clear();
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

QString ChatGateway::canonicalChatId(const QString &accountId) const
{
	const auto account = known.value(accountId);
	if (account.platform != QStringLiteral("youtube")) return accountId;
	QString owner = account.authorizationSourceId.isEmpty() ? accountId : account.authorizationSourceId;
	if (!known.contains(owner)) owner = accountId;
	if (account.providerId.isEmpty()) return owner;
	// A channel can also have independently configured OAuth outputs. Prefer
	// an actual token owner, with a deterministic choice across reloads.
	for (const auto &candidate : known) {
		if (candidate.platform != QStringLiteral("youtube") ||
		    candidate.providerId != account.providerId || !candidate.authorizationSourceId.isEmpty()) continue;
		if (candidate.id < owner) owner = candidate.id;
	}
	return owner;
}

QVector<StreamAccount> ChatGateway::chatAccounts() const
{
	QVector<StreamAccount> result;
	for (const auto &account : accounts()) {
		if (account.platform == QStringLiteral("youtube") && canonicalChatId(account.id) != account.id)
			continue;
		result.push_back(account);
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
	QSet<QString> newlyAuthorized;
	for (const auto &account : updated) {
		if (account.platform == QStringLiteral("twitch") &&
		    (account.oauth.accessToken != known.value(account.id).oauth.accessToken ||
		     account.oauth.clientId != known.value(account.id).oauth.clientId ||
		     account.providerId != known.value(account.id).providerId))
			newlyAuthorized.insert(account.id);
	}
	for (auto it = twitchScopes.begin(); it != twitchScopes.end();) {
		const auto old = known.value(it.key());
		const auto next = updated.value(it.key());
		if (next.id.isEmpty() || next.oauth.accessToken != old.oauth.accessToken ||
		    next.providerId != old.providerId || next.oauth.clientId != old.oauth.clientId) {
			nextTwitchAt[it.key()] = 0; // A successful OAuth login must retry immediately.
			twitchPermissionChecks.remove(it.key());
			for (auto warning = twitchOptionalWarnings.begin(); warning != twitchOptionalWarnings.end();) {
				if (warning->startsWith(it.key())) warning = twitchOptionalWarnings.erase(warning);
				else ++warning;
			}
			it = twitchScopes.erase(it);
		} else ++it;
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
	for (auto it = twitchReconnect.begin(); it != twitchReconnect.end();) {
		const auto account = updated.value(it.key());
		if (account.id.isEmpty() || account.providerId != known.value(it.key()).providerId ||
		    account.oauth.accessToken != known.value(it.key()).oauth.accessToken) {
			delete it.value();
			it = twitchReconnect.erase(it);
		} else ++it;
	}
	for (auto it = youtube.begin(); it != youtube.end();) {
		if (!updated.contains(it.key()) ||
		    updated[it.key()].providerId != known.value(it.key()).providerId) {
			delete youtubeStreams.take(it.key());
			it = youtube.erase(it);
		} else {
			// The broadcast did not change: keep its continuation token and
			// reauthenticate the stream with the newly saved OAuth account.
			if (updated[it.key()].oauth.accessToken != known.value(it.key()).oauth.accessToken)
				delete youtubeStreams.take(it.key());
			++it;
		}
	}
	for (auto it = kickSockets.begin(); it != kickSockets.end();) {
		if (!updated.contains(it.key()) ||
		    updated[it.key()].providerName != known.value(it.key()).providerName) {
			delete it.value();
			it = kickSockets.erase(it);
		} else ++it;
	}
	for (auto it = kick.begin(); it != kick.end();) {
		if (!updated.contains(it.key())) it = kick.erase(it);
		else ++it;
	}
	known = updated;
	for (auto it = viewerSnapshot.begin(); it != viewerSnapshot.end();) {
		if (!known.contains(it.key()) || canonicalChatId(it.key()) != it.key())
			it = viewerSnapshot.erase(it);
		else ++it;
	}
	for (auto it = youtube.begin(); it != youtube.end();) {
		if (canonicalChatId(it.key()) != it.key()) {
			delete youtubeStreams.take(it.key());
			it = youtube.erase(it);
		}
		else ++it;
	}
	for (const auto &account : updated) {
		if (newlyAuthorized.contains(account.id)) nextTwitchAt[account.id] = 0;
		if (newlyAuthorized.contains(account.id)) twitchRetryAttempts.remove(account.id);
		if (account.platform == QStringLiteral("youtube") && canonicalChatId(account.id) == account.id &&
		    !youtube.contains(account.id)) {
			youtube.insert(account.id, {});
		}
		if (account.platform == QStringLiteral("kick") && !kick.contains(account.id)) {
			KickState state;
			state.slug = account.providerName.toLower().trimmed();
			const auto config = studioSettingsPath();
			if (!config.isEmpty()) {
				QSettings settings(config, QSettings::IniFormat);
				state.roomId = settings.value(QStringLiteral("chat/kickRoom-%1").arg(account.id)).toString();
			}
			kick.insert(account.id, state);
		}
	}
	for (auto it = twitchRetryAttempts.begin(); it != twitchRetryAttempts.end();) {
		if (!known.contains(it.key())) it = twitchRetryAttempts.erase(it);
		else ++it;
	}
	nextReloadAt = QDateTime::currentMSecsSinceEpoch() + 30000;
}

void ChatGateway::retryDisconnectedChats()
{
	reloadAccounts();
	for (const auto &account : chatAccounts()) {
		if (account.platform == QStringLiteral("twitch") && !twitch.contains(account.id) &&
		    !twitchPermissionChecks.contains(account.id)) {
			nextTwitchAt[account.id] = 0;
			twitchRetryAttempts.remove(account.id);
		} else if (account.platform == QStringLiteral("youtube") && youtube.contains(account.id) &&
			   !youtube[account.id].pending && !youtubeStreams.contains(account.id)) {
			youtube[account.id].nextAt = 0;
			youtube[account.id].attempts = 0;
		} else if (account.platform == QStringLiteral("kick") && kick.contains(account.id) &&
			   !kick[account.id].pending && !kickSockets.contains(account.id)) {
			kick[account.id].nextAt = 0;
			kick[account.id].attempts = 0;
		}
	}
	// An actual quotaExceeded is not a connectivity failure. Keep the shared
	// YouTube quota pause, even for an explicit user-requested reconnect.
	schedule();
}

void ChatGateway::schedule()
{
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (now >= nextReloadAt) reloadAccounts();
	for (auto it = youtube.begin(); it != youtube.end(); ++it) {
		if (now < youtubeQuotaResumeAt) break;
		if (!it->pending && !youtubeStreams.contains(it.key()) && now >= it->nextAt) pollYouTube(it.key());
	}
	for (const auto &account : known) {
		if (account.platform == QStringLiteral("twitch") && !twitch.contains(account.id) &&
		    now >= nextTwitchAt.value(account.id)) startTwitch(account.id);
		if (account.platform == QStringLiteral("youtube") && canonicalChatId(account.id) != account.id)
			continue;
		if (account.platform == QStringLiteral("youtube") && now < youtubeQuotaResumeAt) continue;
		if (now >= nextViewerPoll.value(account.id)) {
			nextViewerPoll[account.id] = now + (account.platform == QStringLiteral("youtube") ? 180000 : 60000);
			pollViewers(account.id);
		}
	}
	for (auto it = kick.begin(); it != kick.end(); ++it) {
		if (!it->pending && !kickSockets.contains(it.key()) && now >= it->nextAt) startKick(it.key());
		if (kickSockets.contains(it.key()) && it->subscribed && !it->pending &&
		    now >= it->tokenExpiresAt - 90000 && now >= it->nextAt) {
			const auto id = it.key();
			kickGuestToken(id, [this, id](const QString &token) {
				if (!token.isEmpty() && kickSockets.contains(id)) {
					kickSockets[id]->send(QJsonDocument(QJsonObject{
						{QStringLiteral("id"), 3},
						{QStringLiteral("refresh"), QJsonObject{{QStringLiteral("token"), token}}}})
						.toJson(QJsonDocument::Compact));
				}
			});
		}
	}
}

void ChatGateway::pollViewers(const QString &id)
{
	if (canonicalChatId(id) != id) return;
	const auto account = known.value(id);
	if (account.id.isEmpty()) return;
	const auto report = [this, id, platform = account.platform](int count) {
		emit viewersReceived(id, count);
		viewerSnapshot[id] = count;
		int sum = 0;
		bool knownValue = false;
		for (const auto &account : known) {
			if (account.platform != platform || !viewerSnapshot.contains(account.id) ||
			    canonicalChatId(account.id) != account.id) continue;
			const int value = viewerSnapshot.value(account.id);
			if (value >= 0) { sum += value; knownValue = true; }
		}
		if (auto *overlay = mainWindow->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime")))
			overlay->publishViewers(platform, knownValue ? sum : -1);
	};
	if (account.platform == QStringLiteral("twitch")) {
		if (account.providerId.isEmpty()) { report(-1); return; }
		authorized(id, apiUrl(QStringLiteral("https://api.twitch.tv/helix/streams"),
			{{QStringLiteral("user_id"), account.providerId}}), {}, [report](const HttpResult &result) {
			if (!result.ok) { report(-1); return; }
			const auto entries = result.json.value(QStringLiteral("data")).toArray();
			report(entries.isEmpty() ? 0 : entries.first().toObject().value(QStringLiteral("viewer_count")).toInt());
		});
	} else if (account.platform == QStringLiteral("youtube")) {
		const QString url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"),
			{{QStringLiteral("part"), QStringLiteral("id")},
			 {QStringLiteral("broadcastStatus"), QStringLiteral("active")},
			 {QStringLiteral("maxResults"), QStringLiteral("50")}});
		authorized(id, url, {}, [this, id, report](const HttpResult &result) {
			if (!result.ok) { report(-1); return; }
			const auto items = result.json.value(QStringLiteral("items")).toArray();
			if (items.isEmpty()) { report(0); return; }
			QStringList videoIds;
			for (const auto &value : items) {
				const QString videoId = value.toObject().value(QStringLiteral("id")).toString();
				if (!videoId.isEmpty() && !videoIds.contains(videoId)) videoIds.append(videoId);
			}
			if (videoIds.isEmpty()) { report(-1); return; }
			const QString videoUrl = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/videos"),
				{{QStringLiteral("part"), QStringLiteral("liveStreamingDetails")},
				 {QStringLiteral("id"), videoIds.join(QLatin1Char(','))}});
			authorized(id, videoUrl, {}, [report, expected = int(videoIds.size())](const HttpResult &response) {
				if (!response.ok || response.json.value(QStringLiteral("items")).toArray().isEmpty()) {
					report(-1); return;
				}
				int total = 0;
				int countedVideos = 0;
				for (const auto &video : response.json.value(QStringLiteral("items")).toArray()) {
					const auto details = video.toObject().value(QStringLiteral("liveStreamingDetails")).toObject();
					const auto count = details.value(QStringLiteral("concurrentViewers"));
					if (!count.isString() && !count.isDouble()) continue;
					bool valid = false;
					const int viewers = count.toVariant().toString().toInt(&valid);
					if (valid && viewers >= 0) { total += viewers; ++countedVideos; }
				}
				// A partially known sum looks like an exact audience count.
				report(countedVideos == expected ? total : -1);
			});
		});
	} else if (account.platform == QStringLiteral("kick")) {
		const auto slug = account.providerName;
		if (slug.isEmpty()) { report(-1); return; }
		const QString url = apiUrl(QStringLiteral("https://api.kick.com/public/v1/channels"),
			{{QStringLiteral("slug"), slug}});
		authorized(id, url, {}, [report](const HttpResult &result) {
			if (!result.ok || result.json.value(QStringLiteral("data")).toArray().isEmpty()) {
				report(-1); return;
			}
			const auto stream = result.json.value(QStringLiteral("data")).toArray().first().toObject()
				.value(QStringLiteral("stream")).toObject();
			report(stream.isEmpty() ? 0 : stream.value(QStringLiteral("viewer_count")).toInt());
		});
	}
}

void ChatGateway::request(const QString &id, const QString &url, const QByteArray &body,
			  const QByteArray &contentType, const RequestDone &done, bool useToken,
			  const QStringList &extraHeaders, const QByteArray &verb)
{
	if (!known.contains(id)) return;
	const bool youtubeApi = known.value(id).platform == QStringLiteral("youtube") &&
		url.contains(QStringLiteral("googleapis.com/youtube/v3/"));
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (youtubeApi && now < youtubeQuotaResumeAt) {
		done({{}, 403, false, QStringLiteral("YouTube: дневная квота API исчерпана. Повтор после %1")
			.arg(QDateTime::fromMSecsSinceEpoch(youtubeQuotaResumeAt).toString(QStringLiteral("dd.MM HH:mm")))});
		return;
	}
	std::vector<std::string> headers{"Accept: application/json"};
	for (const auto &header : extraHeaders) headers.push_back(header.toStdString());
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
	const auto method = verb.isEmpty() ? (contentType.isEmpty() ? std::string("GET") : std::string("POST"))
					      : verb.toStdString();
	struct Transfer {
		std::string response;
		std::string error;
		long status = 0;
		bool ok = false;
	};
	const auto transfer = std::make_shared<Transfer>();
	auto *worker = QThread::create([transfer, address, payload, mime, method, headers = std::move(headers)] {
		const bool hasBody = method != "GET";
		transfer->ok = GetRemoteFile(address.c_str(), transfer->response, transfer->error, &transfer->status,
					     hasBody ? mime.c_str() : nullptr, method.c_str(),
					     hasBody ? payload.c_str() : nullptr, headers, nullptr, 20, false,
					     static_cast<int>(payload.size()));
	});
	connect(worker, &QThread::finished, this, [this, transfer, id, done, youtubeApi] {
		if (!known.contains(id)) return;
		const int code = static_cast<int>(transfer->status);
		const auto json = QJsonDocument::fromJson(QByteArray::fromStdString(transfer->response)).object();
		const bool success = transfer->ok && code >= 200 && code < 300;
		QString error = success ? QString{} : endpointError(json, code, QString::fromStdString(transfer->error));
		if (youtubeApi && code == 403 && quotaExceeded(json)) {
			youtubeQuotaResumeAt = nextYoutubeQuotaReset();
			error = QStringLiteral("YouTube: дневная квота API исчерпана. Повтор после %1")
				.arg(QDateTime::fromMSecsSinceEpoch(youtubeQuotaResumeAt).toString(QStringLiteral("dd.MM HH:mm")));
			status(id, error);
		}
		done({json, code, success, error, QByteArray::fromStdString(transfer->response)});
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
	const QString credentialOwner = account.authorizationSourceId.isEmpty() ? id : account.authorizationSourceId;
	const auto oldRefresh = account.oauth.refreshToken;
	account.oauth.accessToken = json.value(QStringLiteral("access_token")).toString();
	account.oauth.refreshToken = json.value(QStringLiteral("refresh_token")).toString(oldRefresh);
	const auto expires = json.value(QStringLiteral("expires_in")).toVariant().toLongLong();
	account.oauth.expiresAt = expires > 0 ? QDateTime::currentSecsSinceEpoch() + expires : 0;
	auto stored = loadStreamAccounts();
	for (auto &entry : stored) {
		if (entry.id != credentialOwner) continue;
		// Never overwrite a newer OAuth login or changes to the account editor.
		if (entry.oauth.refreshToken != oldRefresh) return;
		entry.oauth = account.oauth;
		if (saveStreamAccounts(stored)) {
			for (auto it = known.begin(); it != known.end(); ++it)
				if (it.key() == credentialOwner || it.value().authorizationSourceId == credentialOwner)
					it.value().oauth = account.oauth;
		}
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

void ChatGateway::jsonApi(const QString &id, const QString &url, const QByteArray &verb,
			 const QJsonObject &body, const RequestDone &done, bool retry)
{
	tokenFor(id, [this, id, url, verb, body, done, retry](const QString &token) {
		if (token.isEmpty()) { done({{}, 0, false, QStringLiteral("OAuth недоступен")}); return; }
		const QByteArray payload = body.isEmpty() ? QByteArray{} : QJsonDocument(body).toJson(QJsonDocument::Compact);
		request(id, url, payload, QByteArray("application/json"),
			[this, id, url, verb, body, done, retry](const HttpResult &result) {
				if (result.status == 401 && !retry) {
					tokenFor(id, [this, id, url, verb, body, done](const QString &token) {
						if (token.isEmpty()) done({{}, 401, false, QStringLiteral("OAuth отклонён")});
						else jsonApi(id, url, verb, body, done, true);
					}, true);
					return;
				}
				done(result);
			}, true, {}, verb);
	});
}

void ChatGateway::inspectBroadcast(const QString &id, const QString &youtubeBroadcastId)
{
	const auto account = known.value(id);
	if (account.id.isEmpty()) return;
	if (account.platform == QStringLiteral("twitch")) {
		if (account.providerId.isEmpty()) { emit broadcastResult(id, QStringLiteral("Нет Twitch ID"), false); return; }
		const auto url = apiUrl(QStringLiteral("https://api.twitch.tv/helix/channels"),
			{{QStringLiteral("broadcaster_id"), account.providerId}});
		authorized(id, url, {}, [this, id, account](const HttpResult &result) {
			if (!result.ok) { emit broadcastResult(id, result.error, false); return; }
			const auto data = result.json.value(QStringLiteral("data")).toArray();
			if (data.isEmpty()) { emit broadcastResult(id, QStringLiteral("Канал не найден"), false); return; }
			const auto item = data.first().toObject();
			QJsonObject output{{QStringLiteral("platform"), QStringLiteral("twitch")},
				{QStringLiteral("title"), item.value(QStringLiteral("title"))},
				{QStringLiteral("tags"), item.value(QStringLiteral("tags"))},
				{QStringLiteral("categoryId"), item.value(QStringLiteral("game_id"))},
				{QStringLiteral("categoryName"), item.value(QStringLiteral("game_name"))}};
			// Twitch's channel bio is a user property, not part of /helix/channels.
			authorized(id, apiUrl(QStringLiteral("https://api.twitch.tv/helix/users"),
				{{QStringLiteral("id"), account.providerId}}), {},
				[this, id, output](const HttpResult &profile) mutable {
					if (profile.ok && !profile.json.value(QStringLiteral("data")).toArray().isEmpty())
						output.insert(QStringLiteral("description"), profile.json.value(QStringLiteral("data"))
							.toArray().first().toObject().value(QStringLiteral("description")));
					emit broadcastInfo(id, output);
				});
		});
	} else if (account.platform == QStringLiteral("kick")) {
		authorized(id, QStringLiteral("https://api.kick.com/public/v1/channels"), {},
			[this, id](const HttpResult &result) {
				if (!result.ok) { emit broadcastResult(id, result.error, false); return; }
				const auto data = result.json.value(QStringLiteral("data")).toArray();
				if (data.isEmpty()) { emit broadcastResult(id, QStringLiteral("Канал Kick не найден"), false); return; }
				const auto item = data.first().toObject();
				const auto live = item.value(QStringLiteral("stream")).toObject();
				QString title = item.value(QStringLiteral("stream_title")).toString();
				if (title.isEmpty()) title = live.value(QStringLiteral("title")).toString();
				if (title.isEmpty()) title = item.value(QStringLiteral("title")).toString();
				const auto category = item.value(QStringLiteral("category")).toObject().isEmpty()
					? live.value(QStringLiteral("category")).toObject()
					: item.value(QStringLiteral("category")).toObject();
				emit broadcastInfo(id, {{QStringLiteral("platform"), QStringLiteral("kick")},
					{QStringLiteral("title"), title},
					{QStringLiteral("tags"), live.value(QStringLiteral("custom_tags"))},
					{QStringLiteral("description"), item.value(QStringLiteral("channel_description")).toString(
						item.value(QStringLiteral("description")).toString())},
					{QStringLiteral("categoryId"), category.value(QStringLiteral("id"))},
					{QStringLiteral("categoryName"), category.value(QStringLiteral("name"))}});
			});
	} else if (account.platform == QStringLiteral("youtube")) {
		const auto url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"),
			{{QStringLiteral("part"), QStringLiteral("id,snippet,status,contentDetails")},
			 {QStringLiteral("mine"), QStringLiteral("true")},
			 {QStringLiteral("broadcastType"), QStringLiteral("all")},
			 {QStringLiteral("maxResults"), QStringLiteral("50")}});
		authorized(id, url, {}, [this, id, youtubeBroadcastId](const HttpResult &result) {
			if (!result.ok) { emit broadcastResult(id, result.error, false); return; }
			QJsonArray broadcasts;
			QJsonObject selected;
			QJsonArray active;
			QJsonArray boundToConfiguredStream;
			const QString configuredStream = known.value(id).youtubeStreamId;
			for (const auto &entry : result.json.value(QStringLiteral("items")).toArray()) {
				const auto item = entry.toObject();
				const auto status = item.value(QStringLiteral("status")).toObject();
				const QString lifecycle = status.value(QStringLiteral("lifeCycleStatus")).toString();
				if (lifecycle == QStringLiteral("complete") || lifecycle == QStringLiteral("revoked")) continue;
				const auto snippet = item.value(QStringLiteral("snippet")).toObject();
				const QJsonObject value{{QStringLiteral("id"), item.value(QStringLiteral("id"))},
					{QStringLiteral("title"), snippet.value(QStringLiteral("title"))},
					{QStringLiteral("description"), snippet.value(QStringLiteral("description"))},
					{QStringLiteral("categoryId"), snippet.value(QStringLiteral("categoryId"))},
					{QStringLiteral("lifecycle"), lifecycle},
					{QStringLiteral("boundStreamId"), item.value(QStringLiteral("contentDetails")).toObject()
						.value(QStringLiteral("boundStreamId"))}};
				broadcasts.append(value);
				if (!youtubeBroadcastId.isEmpty() && value.value(QStringLiteral("id")) == youtubeBroadcastId)
					selected = value;
				if (lifecycle == QStringLiteral("live") || lifecycle == QStringLiteral("liveStarting"))
					active.append(value);
				if (!configuredStream.isEmpty() &&
				    value.value(QStringLiteral("boundStreamId")).toString() == configuredStream)
					boundToConfiguredStream.append(value);
			}
			if (selected.isEmpty()) {
				if (active.size() == 1) selected = active.first().toObject();
				else if (boundToConfiguredStream.size() == 1) selected = boundToConfiguredStream.first().toObject();
				else if (broadcasts.size() == 1) selected = broadcasts.first().toObject();
			}
			QJsonObject output = selected;
			output.insert(QStringLiteral("platform"), QStringLiteral("youtube"));
			output.insert(QStringLiteral("broadcasts"), broadcasts);
			const QString boundStreamId = selected.value(QStringLiteral("boundStreamId")).toString();
			if (boundStreamId.isEmpty()) {
				emit broadcastInfo(id, output);
				return;
			}
			// A healthy local RTMP output does not imply that this broadcast is
			// bound to the same YouTube ingest stream. Compare IDs and stream keys
			// without displaying or logging the stream key itself.
			const QString streamUrl = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveStreams"),
				{{QStringLiteral("part"), QStringLiteral("id,status,cdn")},
				 {QStringLiteral("id"), boundStreamId}});
			authorized(id, streamUrl, {}, [this, id, output, boundStreamId](const HttpResult &streamResult) mutable {
				if (!streamResult.ok || streamResult.json.value(QStringLiteral("items")).toArray().isEmpty()) {
					output.insert(QStringLiteral("ingestCheckError"), streamResult.error.isEmpty()
						? QStringLiteral("видеопоток не найден") : streamResult.error);
					emit broadcastInfo(id, output);
					return;
				}
				const QJsonObject stream = streamResult.json.value(QStringLiteral("items"))
					.toArray().first().toObject();
				const auto health = stream.value(QStringLiteral("status")).toObject();
				output.insert(QStringLiteral("ingestStatus"), health.value(QStringLiteral("streamStatus")));
				output.insert(QStringLiteral("ingestHealth"), health.value(QStringLiteral("healthStatus"))
					.toObject().value(QStringLiteral("status")));
				const auto configured = known.value(id);
				if (!configured.youtubeStreamId.isEmpty())
					output.insert(QStringLiteral("configuredStreamMatch"),
						configured.youtubeStreamId == boundStreamId);
				const QString remoteKey = stream.value(QStringLiteral("cdn")).toObject()
					.value(QStringLiteral("ingestionInfo")).toObject()
					.value(QStringLiteral("streamName")).toString();
				if (!remoteKey.isEmpty() && !configured.destination.streamKey.isEmpty())
					output.insert(QStringLiteral("ingestKeyMatch"),
						remoteKey == configured.destination.streamKey);
				emit broadcastInfo(id, output);
			});
		});
	}
}

void ChatGateway::searchCategories(const QString &id, const QString &query)
{
	const auto account = known.value(id);
	if (account.id.isEmpty()) return;
	const auto name = query.trimmed().left(90);
	if (name.size() < 2 && account.platform != QStringLiteral("youtube")) return;
	QString url;
	if (account.platform == QStringLiteral("twitch")) url = apiUrl(
		QStringLiteral("https://api.twitch.tv/helix/search/categories"),
		{{QStringLiteral("query"), name}, {QStringLiteral("first"), QStringLiteral("20")}});
	else if (account.platform == QStringLiteral("kick")) url = apiUrl(
		QStringLiteral("https://api.kick.com/public/v2/categories"),
		{{QStringLiteral("name"), name}, {QStringLiteral("limit"), QStringLiteral("20")}});
	else url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/videoCategories"),
		{{QStringLiteral("part"), QStringLiteral("snippet")}, {QStringLiteral("regionCode"), QStringLiteral("US")},
		 {QStringLiteral("hl"), QStringLiteral("ru")}});
	authorized(id, url, {}, [this, id](const HttpResult &result) {
		if (!result.ok) { emit broadcastResult(id, result.error, false); return; }
		QJsonArray categories;
		const auto account = known.value(id);
		const auto items = account.platform == QStringLiteral("youtube") ?
			result.json.value(QStringLiteral("items")).toArray() : result.json.value(QStringLiteral("data")).toArray();
		for (const auto &entry : items) {
			const auto obj = entry.toObject();
			const auto label = account.platform == QStringLiteral("youtube") ?
				obj.value(QStringLiteral("snippet")).toObject().value(QStringLiteral("title")).toString()
				: obj.value(QStringLiteral("name")).toString();
			// Public YouTube API returns video categories, not game titles.
			const auto value = obj.value(QStringLiteral("id")).toVariant().toString();
			if (!value.isEmpty() && !label.isEmpty()) categories.append(QJsonObject{
				{QStringLiteral("id"), value}, {QStringLiteral("name"), label}});
			if (categories.size() >= 50) break;
		}
		emit categoryResults(id, categories);
	});
}

void ChatGateway::updateBroadcast(const QString &id, const QString &broadcastId,
				  const QString &title, const QString &description, const QString &categoryId,
				  const QStringList &tags, bool applyTags)
{
	const auto account = known.value(id);
	if (account.id.isEmpty()) return;
	const QString heading = title.trimmed().left(100);
	const QString category = categoryId.trimmed().left(40);
	QJsonObject body;
	QString url;
	QByteArray method("PATCH");
	if (account.platform == QStringLiteral("twitch")) {
		url = apiUrl(QStringLiteral("https://api.twitch.tv/helix/channels"),
			{{QStringLiteral("broadcaster_id"), account.providerId}});
		if (!heading.isEmpty()) body.insert(QStringLiteral("title"), heading);
		if (!category.isEmpty()) body.insert(QStringLiteral("game_id"), category);
		if (applyTags) body.insert(QStringLiteral("tags"), QJsonArray::fromStringList(tags));
		const auto finish = [this, id, description](bool changed, const QString &error) {
			if (!changed || description.isEmpty()) {
				emit broadcastResult(id, changed ? QStringLiteral("Twitch: данные сохранены") : error, changed);
				return;
			}
			const auto userUrl = apiUrl(QStringLiteral("https://api.twitch.tv/helix/users"),
				{{QStringLiteral("description"), description.left(300)}});
			jsonApi(id, userUrl, QByteArray("PUT"), {}, [this, id](const HttpResult &reply) {
				emit broadcastResult(id, reply.ok ? QStringLiteral("Twitch: эфир и описание канала сохранены")
					: QStringLiteral("Twitch: описание не сохранено (нужно user:edit): ") + reply.error, reply.ok);
			});
		};
		if (body.isEmpty()) {
			finish(!description.isEmpty(), QStringLiteral("Twitch: укажи название, категорию или описание"));
			return;
		}
		jsonApi(id, url, method, body, [finish](const HttpResult &result) { finish(result.ok, result.error); });
		return;
	} else if (account.platform == QStringLiteral("kick")) {
		url = QStringLiteral("https://api.kick.com/public/v1/channels");
		if (!heading.isEmpty()) body.insert(QStringLiteral("stream_title"), heading);
		bool ok = false;
		const int number = category.toInt(&ok);
		if (!category.isEmpty() && (!ok || number <= 0)) {
			emit broadcastResult(id, QStringLiteral("Kick: категория должна быть числовым ID"), false); return;
		}
		if (number > 0) body.insert(QStringLiteral("category_id"), number);
		if (applyTags) body.insert(QStringLiteral("custom_tags"), QJsonArray::fromStringList(tags));
	} else if (account.platform == QStringLiteral("youtube")) {
		if (broadcastId.isEmpty()) {
			emit broadcastResult(id, QStringLiteral("YouTube: выбери эфир"), false); return;
		}
		const auto readUrl = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"),
			{{QStringLiteral("part"), QStringLiteral("snippet,contentDetails")}, {QStringLiteral("id"), broadcastId}});
		authorized(id, readUrl, {}, [this, id, broadcastId, heading, description, category, tags, applyTags](const HttpResult &result) {
			if (!result.ok || result.json.value(QStringLiteral("items")).toArray().isEmpty()) {
				emit broadcastResult(id, QStringLiteral("YouTube: не удалось прочитать эфир: ") + result.error, false);
				return;
			}
			const QJsonObject existing = result.json.value(QStringLiteral("items")).toArray().first().toObject();
			const auto originalSnippet = existing.value(QStringLiteral("snippet")).toObject();
			const auto originalDetails = existing.value(QStringLiteral("contentDetails")).toObject();
			const bool metadataUnchanged =
				(heading.isEmpty() || originalSnippet.value(QStringLiteral("title")).toString() == heading) &&
				originalSnippet.value(QStringLiteral("description")).toString() == description.left(5000) &&
				(category.isEmpty() || originalSnippet.value(QStringLiteral("categoryId")).toString() == category);
			if (metadataUnchanged) {
				if (applyTags) updateYouTubeVideoTags(id, broadcastId, heading, description, category, tags);
				else emit broadcastResult(id, QStringLiteral("YouTube: данные эфира уже актуальны"), true);
				return;
			}
			QJsonObject monitor = originalDetails.value(QStringLiteral("monitorStream")).toObject();
			// liveBroadcasts.update also requires scheduledStartTime and monitorStream
			// settings even when only the snippet part is being changed.
			if (originalSnippet.value(QStringLiteral("scheduledStartTime")).toString().isEmpty()) {
				emit broadcastResult(id, QStringLiteral("YouTube: API не вернул обязательные настройки эфира"), false);
				return;
			}
			// JSON API may omit default false/zero values from the read response.
			if (!monitor.contains(QStringLiteral("enableMonitorStream")))
				monitor.insert(QStringLiteral("enableMonitorStream"), false);
			if (!monitor.contains(QStringLiteral("broadcastStreamDelayMs")))
				monitor.insert(QStringLiteral("broadcastStreamDelayMs"), 0);
			// Only send mutable snippet properties; Google also returns read-only
			// fields such as liveChatId, publishedAt and thumbnails.
			QJsonObject snippet{{QStringLiteral("title"), originalSnippet.value(QStringLiteral("title"))},
				{QStringLiteral("scheduledStartTime"), originalSnippet.value(QStringLiteral("scheduledStartTime"))}};
			for (const QString &field : {QStringLiteral("scheduledEndTime"), QStringLiteral("categoryId")})
				if (originalSnippet.contains(field)) snippet.insert(field, originalSnippet.value(field));
			if (!heading.isEmpty()) snippet.insert(QStringLiteral("title"), heading);
			// Empty text intentionally clears the existing video description.
			snippet.insert(QStringLiteral("description"), description.left(5000));
			if (!category.isEmpty()) snippet.insert(QStringLiteral("categoryId"), category);
			const auto endpoint = QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts?part=snippet");
			const auto body = QJsonObject{{QStringLiteral("id"), broadcastId},
				{QStringLiteral("snippet"), snippet},
				{QStringLiteral("contentDetails"), QJsonObject{{QStringLiteral("monitorStream"), monitor}}}};
			jsonApi(id, endpoint, QByteArray("PUT"), body, [this, id, broadcastId, heading, description, category, tags, applyTags](const HttpResult &reply) {
				if (!reply.ok) {
					emit broadcastResult(id, QStringLiteral("YouTube: ") + reply.error, false);
					return;
				}
				const auto saved = reply.json.value(QStringLiteral("snippet")).toObject();
				const bool verified = (heading.isEmpty() || saved.value(QStringLiteral("title")).toString() == heading) &&
					saved.value(QStringLiteral("description")).toString() == description.left(5000) &&
					(category.isEmpty() || saved.value(QStringLiteral("categoryId")).toString() == category);
				if (!verified || !applyTags) {
					emit broadcastResult(id, verified ? QStringLiteral("YouTube: название и описание эфира обновлены")
						: QStringLiteral("YouTube: ответ API не подтвердил обновление названия, описания или категории"), verified);
					return;
				}
				updateYouTubeVideoTags(id, broadcastId, heading, description, category, tags);
			});
		});
		return;
	} else return;
	if (body.isEmpty()) { emit broadcastResult(id, QStringLiteral("Для этой площадки укажи название или категорию"), false); return; }
	jsonApi(id, url, method, body, [this, id, account, tags, applyTags](const HttpResult &result) {
		const auto label = platformTitle(account.platform);
		const QString reason = account.platform == QStringLiteral("kick") && result.status == 401
			? QStringLiteral("Kick: HTTP 401 после обновления токена. Переподключи OAuth в «Настройки → Трансляция» "
				"с разрешением channel:write и проверь, что это аккаунт владельца канала.")
			: account.platform == QStringLiteral("kick") && result.status == 403
			? QStringLiteral("Kick: HTTP 403 — проверь разрешение channel:write и права на канал")
			: label + QStringLiteral(": ") + result.error;
		if (!result.ok || !applyTags) {
			emit broadcastResult(id, result.ok ? label + QStringLiteral(": данные обновлены") : reason, result.ok);
			return;
		}
		// Kick has historically returned HTTP 204 while ignoring some metadata.
		// Check the server's actual tag list before claiming a successful save.
		const auto inspectUrl = QStringLiteral("https://api.kick.com/public/v1/channels");
		authorized(id, inspectUrl, {}, [this, id, account, label, tags](const HttpResult &verified) {
			if (!verified.ok || verified.json.value(QStringLiteral("data")).toArray().isEmpty()) {
				emit broadcastResult(id, label + QStringLiteral(": эфир обновлён, но теги не удалось проверить"), false);
				return;
			}
			const QJsonObject channel = verified.json.value(QStringLiteral("data")).toArray().first().toObject();
			const QJsonValue value = channel.value(QStringLiteral("stream")).toObject()
				.value(QStringLiteral("custom_tags"));
			if (!value.isArray()) {
				emit broadcastResult(id, label + QStringLiteral(": Kick не вернул список тегов для проверки"), false);
				return;
			}
			const QJsonArray actual = value.toArray();
			const bool match = tagsMatch(actual, tags);
			emit broadcastResult(id, match ? label + QStringLiteral(": эфир и теги обновлены")
				: label + QStringLiteral(": эфир обновлён, но теги не совпадают с сохранёнными"), match);
		});
	});
}

void ChatGateway::updateYouTubeVideoTags(const QString &id, const QString &broadcastId,
					 const QString &heading, const QString &description,
					 const QString &category, const QStringList &tags)
{
	// The broadcast endpoint cannot change video tags. Fetch current metadata
	// first: no videos.update request or its quota cost if tags already match.
	const auto videoUrl = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/videos"),
		{{QStringLiteral("part"), QStringLiteral("snippet")}, {QStringLiteral("id"), broadcastId}});
	authorized(id, videoUrl, {}, [this, id, broadcastId, tags, heading, description, category](const HttpResult &result) {
		const QJsonArray items = result.json.value(QStringLiteral("items")).toArray();
		if (!result.ok || items.isEmpty()) {
			emit broadcastResult(id, QStringLiteral("YouTube: теги не прочитаны: ") + result.error, false);
			return;
		}
		const QJsonObject existing = items.first().toObject().value(QStringLiteral("snippet")).toObject();
		QStringList current;
		for (const QJsonValue &tag : existing.value(QStringLiteral("tags")).toArray())
			if (tag.isString()) current << tag.toString();
		if (current == tags) {
			emit broadcastResult(id, QStringLiteral("YouTube: эфир и теги актуальны"), true);
			return;
		}
		QJsonObject snippet{
			{QStringLiteral("title"), heading.isEmpty() ? existing.value(QStringLiteral("title")) : QJsonValue(heading)},
			{QStringLiteral("description"), description.left(5000)},
			{QStringLiteral("categoryId"), category.isEmpty() ? existing.value(QStringLiteral("categoryId")) : QJsonValue(category)},
			{QStringLiteral("tags"), QJsonArray::fromStringList(tags)}};
		for (const QString &field : {QStringLiteral("defaultLanguage"), QStringLiteral("defaultAudioLanguage")})
			if (existing.contains(field)) snippet.insert(field, existing.value(field));
		if (snippet.value(QStringLiteral("title")).toString().isEmpty() ||
		    snippet.value(QStringLiteral("categoryId")).toString().isEmpty()) {
			emit broadcastResult(id, QStringLiteral("YouTube: нельзя обновить теги без названия и категории видео"), false);
			return;
		}
		const QJsonObject request{{QStringLiteral("id"), broadcastId}, {QStringLiteral("snippet"), snippet}};
		const auto updateUrl = QStringLiteral("https://www.googleapis.com/youtube/v3/videos?part=snippet");
		jsonApi(id, updateUrl, QByteArray("PUT"), request, [this, id, tags](const HttpResult &updated) {
			if (!updated.ok) {
				emit broadcastResult(id, QStringLiteral("YouTube: теги не сохранены: ") + updated.error, false);
				return;
			}
			QStringList confirmed;
			for (const QJsonValue &tag : updated.json.value(QStringLiteral("snippet")).toObject()
					.value(QStringLiteral("tags")).toArray())
				if (tag.isString()) confirmed << tag.toString();
			const bool matches = confirmed == tags;
			emit broadcastResult(id, matches ? QStringLiteral("YouTube: эфир и теги обновлены")
				: QStringLiteral("YouTube: API не подтвердил новые теги"), matches);
		});
	});
}

void ChatGateway::saveYouTubeStream(const QString &id, const QString &streamId, const QString &broadcastId)
{
	const auto url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveStreams"),
		{{QStringLiteral("part"), QStringLiteral("id,cdn")}, {QStringLiteral("id"), streamId}});
	authorized(id, url, {}, [this, id, streamId, broadcastId](const HttpResult &result) {
		if (!result.ok || result.json.value(QStringLiteral("items")).toArray().isEmpty()) {
			emit broadcastResult(id, QStringLiteral("YouTube: не удалось получить параметры видеопотока: ") + result.error, false);
			return;
		}
		const auto ingest = result.json.value(QStringLiteral("items")).toArray().first().toObject()
			.value(QStringLiteral("cdn")).toObject().value(QStringLiteral("ingestionInfo")).toObject();
		const QString address = ingest.value(QStringLiteral("rtmpsIngestionAddress")).toString(
			ingest.value(QStringLiteral("ingestionAddress")).toString());
		const QString key = ingest.value(QStringLiteral("streamName")).toString();
		if (key.isEmpty() || !validIngestUrl(address)) {
			emit broadcastResult(id, QStringLiteral("YouTube: API не вернул RTMP(S)-адрес и ключ"), false); return;
		}
		auto stored = loadStreamAccounts();
		bool saved = false;
		for (auto &account : stored) {
			if (account.id != id) continue;
			account.youtubeStreamId = streamId;
			account.destination.ingestUrl = address;
			account.destination.streamKey = key;
			account.destination.fromOAuth = true;
			account.destination.serviceName.clear();
			account.destination.serviceServer.clear();
			saved = saveStreamAccounts(stored);
			break;
		}
		if (saved) reloadAccounts();
		emit broadcastResult(id, saved
			? QStringLiteral("YouTube: поток %1 для эфира %2 сохранён в защищённых настройках").arg(streamId, broadcastId)
			: QStringLiteral("YouTube: не удалось сохранить RTMP-конфигурацию"), saved);
	});
}

void ChatGateway::bindYouTubeStream(const QString &id, const QString &broadcastId, const QString &streamId)
{
	if (known.value(id).platform != QStringLiteral("youtube") || broadcastId.isEmpty() || streamId.isEmpty()) return;
	const auto url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts/bind"),
		{{QStringLiteral("id"), broadcastId}, {QStringLiteral("streamId"), streamId},
		 {QStringLiteral("part"), QStringLiteral("id,contentDetails")}});
	jsonApi(id, url, QByteArray("POST"), {}, [this, id, streamId, broadcastId](const HttpResult &result) {
		if (!result.ok) { emit broadcastResult(id, QStringLiteral("YouTube bind: ") + result.error, false); return; }
		saveYouTubeStream(id, streamId, broadcastId);
	});
}

void ChatGateway::createYouTubeBroadcast(const QString &id, const QString &title,
					const QString &description, const QString &categoryId)
{
	if (known.value(id).platform != QStringLiteral("youtube") || title.trimmed().isEmpty()) return;
	const QJsonObject snippet{{QStringLiteral("title"), title.trimmed().left(100)},
		{QStringLiteral("description"), description.left(5000)},
		{QStringLiteral("scheduledStartTime"), QDateTime::currentDateTimeUtc().addSecs(600).toString(Qt::ISODate)}};
	auto create = snippet;
	if (!categoryId.trimmed().isEmpty()) create.insert(QStringLiteral("categoryId"), categoryId.trimmed());
	jsonApi(id, QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts?part=id,snippet,status,contentDetails"),
		QByteArray("POST"), {{QStringLiteral("snippet"), create},
			{QStringLiteral("status"), QJsonObject{{QStringLiteral("privacyStatus"), QStringLiteral("unlisted")},
				{QStringLiteral("selfDeclaredMadeForKids"), false}}},
			{QStringLiteral("contentDetails"), QJsonObject{{QStringLiteral("enableAutoStart"), true},
				{QStringLiteral("enableAutoStop"), true}}}},
		[this, id, title](const HttpResult &result) {
			const QString broadcastId = result.json.value(QStringLiteral("id")).toString();
			if (!result.ok || broadcastId.isEmpty()) {
				emit broadcastResult(id, QStringLiteral("Создание YouTube эфира: ") + result.error, false); return;
			}
			jsonApi(id, QStringLiteral("https://www.googleapis.com/youtube/v3/liveStreams?part=id,snippet,cdn,contentDetails"),
				QByteArray("POST"), {{QStringLiteral("snippet"), QJsonObject{
					{QStringLiteral("title"), title.left(100) + QStringLiteral(" · XINKX")}}},
					{QStringLiteral("cdn"), QJsonObject{{QStringLiteral("ingestionType"), QStringLiteral("rtmp")},
						{QStringLiteral("resolution"), QStringLiteral("variable")},
						{QStringLiteral("frameRate"), QStringLiteral("variable")}}},
					{QStringLiteral("contentDetails"), QJsonObject{{QStringLiteral("isReusable"), true}}}},
				[this, id, broadcastId](const HttpResult &reply) {
					const QString streamId = reply.json.value(QStringLiteral("id")).toString();
					if (!reply.ok || streamId.isEmpty()) {
						emit broadcastResult(id, QStringLiteral("Эфир %1 создан, но поток создать не удалось: %2")
							.arg(broadcastId, reply.error), false); return;
					}
					bindYouTubeStream(id, broadcastId, streamId);
					inspectBroadcast(id);
				});
		});
}

void ChatGateway::pollYouTube(const QString &id)
{
	if (!known.contains(id) || youtubeStreams.contains(id)) return;
	auto &state = youtube[id];
	if (state.pending) return;
	if (!state.chatId.isEmpty()) {
		startYouTubeStream(id);
		return;
	}
	state.pending = true;
	state.nextAt = QDateTime::currentMSecsSinceEpoch() + 60000;
	status(id, QStringLiteral("YouTube: ищем активную трансляцию"));
	const QString url = apiUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"),
		{{QStringLiteral("part"), QStringLiteral("snippet,status")},
		 {QStringLiteral("broadcastStatus"), QStringLiteral("active")},
		 {QStringLiteral("maxResults"), QStringLiteral("50")}});
	authorized(id, url, {}, [this, id](const HttpResult &result) {
		if (!youtube.contains(id)) return;
		if (!result.ok) {
			youtube[id].pending = false;
			if (result.status == 0) {
				auto &state = youtube[id];
				state.attempts = std::min(state.attempts + 1, 6);
				state.nextAt = QDateTime::currentMSecsSinceEpoch() +
					std::min(120000, 2000 * (1 << state.attempts));
			}
			youtube[id].nextAt = std::max(youtube[id].nextAt, youtubeQuotaResumeAt);
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
		youtube[id].pending = false;
		youtube[id].attempts = 0;
		startYouTubeStream(id);
	});
}

void ChatGateway::startYouTubeStream(const QString &id)
{
	if (!youtube.contains(id) || youtube[id].pending || youtubeStreams.contains(id)) return;
	auto &state = youtube[id];
	if (state.chatId.isEmpty()) return;
	state.pending = true;
	const QString chatId = state.chatId;
	const QString pageToken = state.nextPage;
	const bool refreshToken = state.forceTokenRefresh;
	state.forceTokenRefresh = false;
	tokenFor(id, [this, id, chatId, pageToken](const QString &token) {
		if (!youtube.contains(id)) return;
		youtube[id].pending = false;
		if (token.isEmpty()) {
			youtube[id].nextAt = QDateTime::currentMSecsSinceEpoch() + 60000;
			status(id, QStringLiteral("YouTube: OAuth недоступен для потокового чата"));
			return;
		}
		if (youtubeStreams.contains(id) || youtube[id].chatId != chatId) return;
		status(id, QStringLiteral("YouTube: подключаем потоковый чат"));
		const quint64 generation = ++nextSocketGeneration;
		youtubeGeneration[id] = generation;
		auto identity = std::make_shared<YouTubeGrpcStream *>(nullptr);
		*identity = new YouTubeGrpcStream(this, token, chatId, pageToken,
			[this, id, chatId, identity, generation](const QJsonObject &page) {
				if (youtubeGeneration.value(id) != generation || youtubeStreams.value(id) != *identity ||
				    !youtube.contains(id) ||
				    youtube[id].chatId != chatId) return;
				auto &state = youtube[id];
				// Discard only the initial history on a genuinely new chat. After a
				// reconnect, the saved page token resumes without message loss.
				if (state.baseline) handleYouTubePage(id, page);
				else {
					state.baseline = true;
					status(id, QStringLiteral("YouTube: потоковый чат подключён"));
				}
				state.nextPage = page.value(QStringLiteral("nextPageToken")).toString(state.nextPage);
				state.attempts = 0;
				if (page.contains(QStringLiteral("offlineAt"))) {
					delete youtubeStreams.take(id);
					state.chatId.clear(); state.nextPage.clear(); state.baseline = false;
					state.nextAt = QDateTime::currentMSecsSinceEpoch() + 60000;
					status(id, QStringLiteral("YouTube: эфир завершён"));
				}
			},
			[this, id, chatId, identity, generation](int grpcStatus, const QString &reason) {
				if (youtubeGeneration.value(id) != generation || youtubeStreams.value(id) != *identity) return;
				delete youtubeStreams.take(id);
				if (!youtube.contains(id) || youtube[id].chatId != chatId) return;
				auto &state = youtube[id];
				state.attempts = std::min(state.attempts + 1, 6);
				const int delay = std::min(120000, 2000 * (1 << state.attempts));
				state.nextAt = QDateTime::currentMSecsSinceEpoch() + delay;
				if (grpcStatus == 5 || grpcStatus == 9) {
					state.chatId.clear(); state.nextPage.clear(); state.baseline = false;
					state.nextAt = QDateTime::currentMSecsSinceEpoch() + 60000;
				} else if (grpcStatus == 16) {
					// Refresh revoked OAuth before the next gRPC connection.
					state.forceTokenRefresh = true;
				} else if (grpcStatus == 8 &&
					(reason.contains(QStringLiteral("quota"), Qt::CaseInsensitive) ||
					 reason.contains(QStringLiteral("exceeded"), Qt::CaseInsensitive))) {
					youtubeQuotaResumeAt = nextYoutubeQuotaReset();
					state.nextAt = youtubeQuotaResumeAt;
				}
				status(id, QStringLiteral("YouTube streamList: %1 · переподключение").arg(reason));
			});
		youtubeStreams[id] = *identity;
	}, refreshToken);
}

void ChatGateway::handleYouTubePage(const QString &id, const QJsonObject &page)
{
	for (const auto &value : page.value(QStringLiteral("items")).toArray()) {
		const auto item = value.toObject();
		const auto snippet = item.value(QStringLiteral("snippet")).toObject();
		const auto type = snippet.value(QStringLiteral("type")).toString();
		if (type != QStringLiteral("textMessageEvent") && type != QStringLiteral("superChatEvent") &&
		    type != QStringLiteral("superStickerEvent") && type != QStringLiteral("newSponsorEvent") &&
		    type != QStringLiteral("memberMilestoneChatEvent") &&
		    type != QStringLiteral("membershipGiftingEvent") &&
		    type != QStringLiteral("giftMembershipReceivedEvent")) continue;
		ChatLine line;
		line.platform = QStringLiteral("youtube");
		line.accountId = id;
		line.messageId = item.value(QStringLiteral("id")).toString();
		line.name = item.value(QStringLiteral("authorDetails")).toObject()
				.value(QStringLiteral("displayName")).toString(QStringLiteral("Зритель"));
		line.text = snippet.value(QStringLiteral("displayMessage")).toString(
				snippet.value(QStringLiteral("textMessageDetails")).toObject()
				.value(QStringLiteral("messageText")).toString());
		if (type == QStringLiteral("superChatEvent") || type == QStringLiteral("superStickerEvent")) {
			line.type = QStringLiteral("donation");
			const auto details = snippet.value(type == QStringLiteral("superChatEvent")
					? QStringLiteral("superChatDetails") : QStringLiteral("superStickerDetails")).toObject();
			line.amount = details.value(QStringLiteral("amountDisplayString")).toString();
			line.text = details.value(QStringLiteral("userComment")).toString(line.text);
		} else if (type != QStringLiteral("textMessageEvent")) line.type = QStringLiteral("sub");
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
	if (twitchScopes.contains(id)) {
		if (twitchScopes.value(id).contains(QStringLiteral("user:read:chat"))) openTwitchSocket(id);
		else nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 300000;
		return;
	}
	if (twitchPermissionChecks.contains(id)) return;
	twitchPermissionChecks.insert(id);
	tokenFor(id, [this, id](const QString &token) {
		if (token.isEmpty()) {
			twitchPermissionChecks.remove(id);
			const int failures = std::min(++twitchRetryAttempts[id], 6);
			nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() +
				std::min(120000, 2000 * (1 << failures));
			status(id, QStringLiteral("Twitch: OAuth временно недоступен · повторное подключение"));
			return;
		}
		request(id, QStringLiteral("https://id.twitch.tv/oauth2/validate"), {}, {},
			[this, id, token](const HttpResult &result) {
				twitchPermissionChecks.remove(id);
				// A new login can complete while the previous token is being validated.
				if (known.value(id).oauth.accessToken != token) {
					nextTwitchAt[id] = 0;
					return;
				}
				if (!result.ok) {
					if (result.status == 0 || result.status >= 500 || result.status == 429) {
						const int failures = std::min(++twitchRetryAttempts[id], 6);
						nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() +
							std::min(120000, 2000 * (1 << failures));
						status(id, QStringLiteral("Twitch: сеть/сервер недоступны · повторное подключение (%1)").arg(result.error));
					} else {
						status(id, QStringLiteral("Twitch: OAuth отклонён (%1). Переподключи аккаунт").arg(result.error));
						nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 300000;
					}
					return;
				}
				twitchRetryAttempts.remove(id);
				const QString actualUserId = result.json.value(QStringLiteral("user_id")).toString();
				const QString actualClientId = result.json.value(QStringLiteral("client_id")).toString();
				QSet<QString> scopes;
				for (const auto &scope : result.json.value(QStringLiteral("scopes")).toArray()) scopes.insert(scope.toString());
				twitchScopes[id] = scopes;
				if (actualUserId != known.value(id).providerId ||
				    actualClientId != known.value(id).oauth.clientId) {
					status(id, QStringLiteral("Twitch: OAuth не соответствует пользователю или Client ID. Переподключи аккаунт"));
					twitchScopes[id].remove(QStringLiteral("user:read:chat"));
					nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 300000;
					return;
				}
				if (!scopes.contains(QStringLiteral("user:read:chat"))) {
					status(id, QStringLiteral("Twitch: токен не содержит user:read:chat (в нём %1 прав). Проверь итог OAuth в настройках трансляции")
						.arg(scopes.size()));
					nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 300000;
					return;
				}
				if (!scopes.contains(QStringLiteral("user:write:chat")))
					emit providerWarning(id, QStringLiteral("Twitch: для отправки сообщений потребуется повторный OAuth с user:write:chat"));
				if (!scopes.contains(QStringLiteral("channel:read:subscriptions")))
					emit providerWarning(id, QStringLiteral("Twitch: уведомления о подписках отключены: в OAuth нет channel:read:subscriptions"));
				if (!scopes.contains(QStringLiteral("moderator:read:followers")))
					emit providerWarning(id, QStringLiteral("Twitch: уведомления о фолловерах отключены: в OAuth нет moderator:read:followers"));
				openTwitchSocket(id);
			}, false, {QStringLiteral("Authorization: OAuth %1").arg(token)});
	});
}

void ChatGateway::openTwitchSocket(const QString &id)
{
	if (twitch.contains(id) || twitchReconnect.contains(id)) return;
	status(id, QStringLiteral("Twitch: подключаем EventSub"));
	const quint64 generation = ++nextSocketGeneration;
	twitchGeneration[id] = generation;
	auto identity = std::make_shared<NativeWebSocket *>(nullptr);
	*identity = new NativeWebSocket(this, QUrl(QStringLiteral("wss://eventsub.wss.twitch.tv/ws?keepalive_timeout_seconds=30")),
		[this, id, identity, generation](const QByteArray &frame) {
			if (twitchGeneration.value(id) == generation && twitch.value(id) == *identity)
				handleTwitchFrame(id, frame);
		},
		[this, id, identity, generation](const QString &reason) {
			if (twitchGeneration.value(id) != generation || twitch.value(id) != *identity) return;
			status(id, QStringLiteral("Twitch: ") + reason);
			nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 15000;
			delete twitch.take(id);
		});
	twitch[id] = *identity;
}

void ChatGateway::reconnectTwitchSocket(const QString &id, const QUrl &url)
{
	if (!twitch.contains(id) || twitchReconnect.contains(id)) return;
	// Twitch's reconnect URL transfers existing EventSub subscriptions. The
	// original socket must stay alive until the replacement says welcome.
	if (url.scheme() != QStringLiteral("wss") || url.host() != QStringLiteral("eventsub.wss.twitch.tv") ||
	    url.path() != QStringLiteral("/ws") || !url.userInfo().isEmpty() ||
	    (url.port(-1) != -1 && url.port(-1) != 443)) {
		status(id, QStringLiteral("Twitch: недопустимый адрес переподключения EventSub"));
		return;
	}
	status(id, QStringLiteral("Twitch: переносим EventSub на новое соединение"));
	const quint64 generation = ++nextSocketGeneration;
	twitchReconnectGeneration[id] = generation;
	auto identity = std::make_shared<NativeWebSocket *>(nullptr);
	*identity = new NativeWebSocket(this, url,
		[this, id, identity, generation](const QByteArray &frame) {
			const auto message = QJsonDocument::fromJson(frame).object();
			const QString kind = message.value(QStringLiteral("metadata")).toObject()
				.value(QStringLiteral("message_type")).toString();
			if (kind == QStringLiteral("session_welcome") &&
			    twitchReconnectGeneration.value(id) == generation && twitchReconnect.value(id) == *identity) {
				auto *replacement = twitchReconnect.take(id);
				delete twitch.take(id);
				twitch[id] = replacement;
				twitchGeneration[id] = generation;
				status(id, QStringLiteral("Twitch: EventSub перенесён без повторной подписки"));
				return;
			}
			if (twitchGeneration.value(id) == generation && twitch.value(id) == *identity)
				handleTwitchFrame(id, frame);
		},
		[this, id, identity, generation](const QString &reason) {
			if (twitchReconnectGeneration.value(id) == generation && twitchReconnect.value(id) == *identity) {
				delete twitchReconnect.take(id);
				status(id, QStringLiteral("Twitch: перенос прервался: %1").arg(reason));
				return; // The old socket still has the existing subscriptions.
			}
			if (twitchGeneration.value(id) != generation || twitch.value(id) != *identity) return;
			delete twitch.take(id);
			nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 15000;
			status(id, QStringLiteral("Twitch: %1 · новое подключение").arg(reason));
		});
	twitchReconnect[id] = *identity;
}

QString ChatGateway::kickRoomId(const QString &accountId) const
{
	return kick.value(accountId).roomId;
}

void ChatGateway::setKickRoomId(const QString &accountId, const QString &roomId)
{
	if (!known.contains(accountId) || known.value(accountId).platform != QStringLiteral("kick")) return;
	const QString value = roomId.trimmed();
	if (!value.isEmpty() && !QRegularExpression(QStringLiteral("^[0-9]{1,15}$")).match(value).hasMatch()) {
		status(accountId, QStringLiteral("Kick: ID комнаты должен содержать 1–15 цифр"));
		return;
	}
	const auto path = studioSettingsPath();
	if (!path.isEmpty()) {
		QSettings settings(path, QSettings::IniFormat);
		settings.setValue(QStringLiteral("chat/kickRoom-%1").arg(accountId), value);
	}
	delete kickSockets.take(accountId);
	auto &state = kick[accountId];
	state.roomId = value;
	state.pending = false;
	state.subscribed = false;
	state.nextAt = 0;
	state.attempts = 0;
	status(accountId, value.isEmpty() ? QStringLiteral("Kick: автоопределение комнаты") :
		QStringLiteral("Kick: комната %1 сохранена").arg(value));
}

void ChatGateway::startKick(const QString &id)
{
	if (!kick.contains(id)) return;
	auto &state = kick[id];
	state.nextAt = QDateTime::currentMSecsSinceEpoch() + 30000;
	if (!state.roomId.isEmpty()) kickDiscover(id);
	else kickResolveRoom(id);
}

void ChatGateway::kickResolveRoom(const QString &id)
{
	if (!kick.contains(id)) return;
	auto &state = kick[id];
	const auto slug = state.slug;
	if (!QRegularExpression(QStringLiteral("^[a-z0-9_]{1,40}$")).match(slug).hasMatch()) {
		status(id, QStringLiteral("Kick: укажи chatroom ID или обнови имя канала OAuth"));
		state.nextAt = QDateTime::currentMSecsSinceEpoch() + 60000;
		return;
	}
	state.pending = true;
	const auto url = apiUrl(QStringLiteral("https://kick.com/api/v2/channels/%1").arg(slug), {});
	request(id, url, {}, {}, [this, id](const HttpResult &result) {
		if (!kick.contains(id)) return;
		kick[id].pending = false;
		const QString room = result.json.value(QStringLiteral("chatroom")).toObject()
					.value(QStringLiteral("id")).toVariant().toString();
		if (!result.ok || !QRegularExpression(QStringLiteral("^[0-9]{1,15}$")).match(room).hasMatch()) {
			status(id, QStringLiteral("Kick: не удалось определить комнату, введи её ID вручную"));
			auto &state = kick[id];
			state.attempts = std::min(6, state.attempts + 1);
			state.nextAt = QDateTime::currentMSecsSinceEpoch() +
				std::min(120000, 2000 * (1 << state.attempts));
			return;
		}
		kick[id].roomId = room;
		status(id, QStringLiteral("Kick: найдена комната %1").arg(room));
		kickDiscover(id);
	}, false, {QStringLiteral("User-Agent: Mozilla/5.0")});
}

void ChatGateway::kickDiscover(const QString &id)
{
	if (!kick.contains(id) || kick[id].pending) return;
	auto &state = kick[id];
	state.pending = true;
	if (state.guestId.isEmpty()) state.guestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const QJsonObject body{
		{QStringLiteral("client"), QJsonObject{{QStringLiteral("id"), state.guestId},
						{QStringLiteral("type"), QStringLiteral("web")}}},
		{QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("accepted_providers"), QJsonArray{
			QJsonObject{{QStringLiteral("provider"), QStringLiteral("centrifugo")}}}}}},
	};
	request(id, QStringLiteral("https://web.kick.com/api/v1/realtime/connection"),
		QJsonDocument(body).toJson(QJsonDocument::Compact), QByteArray("application/json"),
		[this, id](const HttpResult &result) {
			if (!kick.contains(id)) return;
			if (!result.ok) {
				kick[id].pending = false;
				kick[id].attempts = std::min(6, kick[id].attempts + 1);
				kick[id].nextAt = QDateTime::currentMSecsSinceEpoch() +
					std::min(120000, 2000 * (1 << kick[id].attempts));
				status(id, QStringLiteral("Kick discovery: ") + result.error);
				return;
			}
			QString address;
			const auto connections = result.json.value(QStringLiteral("data")).toObject()
						 .value(QStringLiteral("connections")).toArray();
			for (const auto &candidate : connections) {
				const auto item = candidate.toObject();
				if (item.value(QStringLiteral("provider")) == QStringLiteral("centrifugo")) {
					address = item.value(QStringLiteral("credentials")).toObject()
							.value(QStringLiteral("url")).toString();
					break;
				}
			}
			const QUrl socketUrl(address, QUrl::StrictMode);
			const bool valid = socketUrl.isValid() && socketUrl.scheme() == QStringLiteral("wss") &&
				QRegularExpression(QStringLiteral("^realtime\\.[a-z0-9-]+\\.platform\\.kick\\.com$"))
					.match(socketUrl.host()).hasMatch() &&
				socketUrl.path() == QStringLiteral("/connection/websocket");
			if (!valid) {
				kick[id].pending = false;
				kick[id].attempts = std::min(6, kick[id].attempts + 1);
				kick[id].nextAt = QDateTime::currentMSecsSinceEpoch() +
					std::min(120000, 2000 * (1 << kick[id].attempts));
				status(id, QStringLiteral("Kick: неожиданный адрес WSS"));
				return;
			}
			kick[id].socketUrl = socketUrl;
			kick[id].pending = false;
			kickGuestToken(id, [this, id](const QString &token) {
				if (token.isEmpty() || !kick.contains(id) || kickSockets.contains(id)) return;
				const auto url = kick[id].socketUrl;
				status(id, QStringLiteral("Kick: соединяемся с комнатой %1").arg(kick[id].roomId));
				const quint64 generation = ++nextSocketGeneration;
				kickGeneration[id] = generation;
				auto identity = std::make_shared<NativeWebSocket *>(nullptr);
				*identity = new NativeWebSocket(this, url,
					[this, id, identity, generation](const QByteArray &frame) {
						if (kickGeneration.value(id) == generation && kickSockets.value(id) == *identity)
							handleKickFrame(id, frame);
					},
					[this, id, identity, generation](const QString &error) {
						if (kickGeneration.value(id) != generation || kickSockets.value(id) != *identity) return;
						delete kickSockets.take(id);
						if (!kick.contains(id)) return;
						auto &state = kick[id];
						state.subscribed = false;
						state.attempts = std::min(6, state.attempts + 1);
						state.nextAt = QDateTime::currentMSecsSinceEpoch() +
							std::min(60000, 2000 * (1 << state.attempts));
						status(id, QStringLiteral("Kick: %1 · повторное соединение").arg(error));
					},
					[this, id, token, identity, generation] {
						if (kickGeneration.value(id) == generation && kickSockets.value(id) == *identity)
							kickSockets[id]->send(QJsonDocument(QJsonObject{
							{QStringLiteral("id"), 1},
							{QStringLiteral("connect"), QJsonObject{
								{QStringLiteral("token"), token},
								{QStringLiteral("name"), QStringLiteral("js")}}}})
							.toJson(QJsonDocument::Compact));
					}, QByteArray("https://kick.com"));
				kickSockets[id] = *identity;
			});
		}, false, {QStringLiteral("Origin: https://kick.com")});
}

void ChatGateway::kickGuestToken(const QString &id, const std::function<void(const QString &)> &done)
{
	if (!kick.contains(id) || kick[id].pending) return;
	auto &state = kick[id];
	state.pending = true;
	const QJsonObject body{{QStringLiteral("client_id"), state.guestId}};
	request(id, QStringLiteral("https://web.kick.com/api/v1/realtime/auth/connection"),
		QJsonDocument(body).toJson(QJsonDocument::Compact), QByteArray("application/json"),
		[this, id, done](const HttpResult &result) {
			if (!kick.contains(id)) return;
			auto &state = kick[id];
			state.pending = false;
			const QString token = result.json.value(QStringLiteral("data")).toObject()
						.value(QStringLiteral("token")).toString();
			if (!result.ok || token.isEmpty()) {
				status(id, QStringLiteral("Kick guest token: ") + result.error);
				state.attempts = std::min(6, state.attempts + 1);
				state.nextAt = QDateTime::currentMSecsSinceEpoch() +
					std::min(120000, 2000 * (1 << state.attempts));
				done({});
				return;
			}
			state.guestToken = token;
			state.tokenExpiresAt = QDateTime::currentMSecsSinceEpoch() + 35 * 60 * 1000LL;
			done(token);
		}, false, {QStringLiteral("Origin: https://kick.com")});
}

void ChatGateway::handleKickFrame(const QString &id, const QByteArray &frame)
{
	if (!kick.contains(id) || !kickSockets.contains(id)) return;
	for (const auto &lineBytes : frame.split('\n')) {
		if (lineBytes.isEmpty()) continue;
		const auto value = QJsonDocument::fromJson(lineBytes).object();
		if (value.isEmpty()) {
			kickSockets[id]->send(QByteArray("{}"));
			continue;
		}
		if (value.value(QStringLiteral("error")).isObject()) {
			const auto error = value.value(QStringLiteral("error")).toObject();
			status(id, QStringLiteral("Kick: код %1 · %2")
				.arg(error.value(QStringLiteral("code")).toVariant().toString(),
				     error.value(QStringLiteral("message")).toString()));
			delete kickSockets.take(id);
			auto &state = kick[id];
			state.subscribed = false;
			state.attempts = std::min(6, state.attempts + 1);
			state.nextAt = QDateTime::currentMSecsSinceEpoch() +
				std::min(120000, 2000 * (1 << state.attempts));
			return;
		}
		const int messageId = value.value(QStringLiteral("id")).toInt();
		if (messageId == 1 && value.value(QStringLiteral("connect")).isObject()) {
			kickSockets[id]->send(QJsonDocument(QJsonObject{
				{QStringLiteral("id"), 2},
				{QStringLiteral("subscribe"), QJsonObject{{QStringLiteral("channel"),
					QStringLiteral("chatrooms.%1.v2").arg(kick[id].roomId)}}}})
				.toJson(QJsonDocument::Compact));
		}
		if (messageId == 2 && value.value(QStringLiteral("subscribe")).isObject()) {
			kick[id].subscribed = true;
			kick[id].attempts = 0;
			status(id, QStringLiteral("Kick: чат подключён · комната %1 (Centrifugo)").arg(kick[id].roomId));
		}
		const auto publication = value.value(QStringLiteral("push")).toObject()
						.value(QStringLiteral("pub")).toObject();
		if (publication.isEmpty()) continue;
		const auto envelope = publication.value(QStringLiteral("data")).toObject();
		const QString event = envelope.value(QStringLiteral("event")).toString(
			publication.value(QStringLiteral("tags")).toObject().value(QStringLiteral("event")).toString());
		if (event != QStringLiteral("App\\Events\\ChatMessageEvent") && event != QStringLiteral("ChatMessageEvent")) continue;
		const auto messageValue = envelope.value(QStringLiteral("data"));
		const auto message = messageValue.isString()
					? QJsonDocument::fromJson(messageValue.toString().toUtf8()).object()
					: messageValue.toObject();
		const QString content = message.value(QStringLiteral("content")).toString();
		if (content.trimmed().isEmpty()) continue;
		ChatLine chat;
		chat.platform = QStringLiteral("kick");
		chat.accountId = id;
		chat.messageId = message.value(QStringLiteral("id")).toVariant().toString();
		chat.name = message.value(QStringLiteral("sender")).toObject()
				.value(QStringLiteral("username")).toString(QStringLiteral("Зритель"));
		chat.text = content;
		dispatch(std::move(chat));
	}
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
					const QString message = QStringLiteral("Twitch %1: %2").arg(type, result.error);
					if (type == QStringLiteral("channel.chat.message")) {
						status(id, message);
						nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 60000;
						delete twitch.take(id);
					} else {
						const QString key = id + (result.status == 403 ? QStringLiteral(":403") : type);
						if (twitchOptionalWarnings.contains(key)) return;
						twitchOptionalWarnings.insert(key);
						emit providerWarning(id, result.status == 403
							? QStringLiteral("Twitch: часть уведомлений недоступна (403). Проверь разрешения OAuth для подписок, фолловеров и Bits")
							: message);
					}
				}
				else if (type == QStringLiteral("channel.chat.message")) {
					status(id, QStringLiteral("Twitch: чат подключён · EventSub"));
				}
			});
	};
	subscribe(QStringLiteral("channel.chat.message"), QStringLiteral("1"),
		{{QStringLiteral("broadcaster_user_id"), userId}, {QStringLiteral("user_id"), userId}});
	const auto scopes = twitchScopes.value(id);
	if (scopes.contains(QStringLiteral("moderator:read:followers"))) subscribe(QStringLiteral("channel.follow"), QStringLiteral("2"),
		{{QStringLiteral("broadcaster_user_id"), userId}, {QStringLiteral("moderator_user_id"), userId}});
	if (scopes.contains(QStringLiteral("channel:read:subscriptions"))) {
		subscribe(QStringLiteral("channel.subscribe"), QStringLiteral("1"), {{QStringLiteral("broadcaster_user_id"), userId}});
		subscribe(QStringLiteral("channel.subscription.message"), QStringLiteral("1"),
		{{QStringLiteral("broadcaster_user_id"), userId}});
		subscribe(QStringLiteral("channel.subscription.gift"), QStringLiteral("1"),
		{{QStringLiteral("broadcaster_user_id"), userId}});
	}
	subscribe(QStringLiteral("channel.raid"), QStringLiteral("1"), {{QStringLiteral("to_broadcaster_user_id"), userId}});
	if (scopes.contains(QStringLiteral("bits:read")))
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
	if (messageType == QStringLiteral("session_reconnect")) {
		const QUrl target(payload.value(QStringLiteral("session")).toObject()
			.value(QStringLiteral("reconnect_url")).toString());
		reconnectTwitchSocket(id, target);
		return;
	}
	if (messageType == QStringLiteral("revocation")) {
		const QString type = payload.value(QStringLiteral("subscription")).toObject()
			.value(QStringLiteral("type")).toString();
		if (type != QStringLiteral("channel.chat.message")) {
			emit providerWarning(id, QStringLiteral("Twitch: подписка %1 отозвана").arg(type));
			return;
		}
		status(id, QStringLiteral("Twitch: подписка чата отозвана · переподключение"));
		delete twitch.take(id);
		nextTwitchAt[id] = QDateTime::currentMSecsSinceEpoch() + 60000;
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
	// liveChat/messages returns the same YouTube message for each linked RTMP
	// output. Its message ID belongs to the chat, not to the output account.
	if (line.platform == QStringLiteral("youtube")) line.accountId = canonicalChatId(line.accountId);
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
	if (line.type != QStringLiteral("chat")) return;
	const auto account = known.value(line.accountId);
	if (line.name.compare(account.providerName, Qt::CaseInsensitive) == 0) return;
	const auto settings = mainWindow->findChild<PresentationSettings *>(QStringLiteral("xinkxPresentationSettings"));
	if (!settings) return;
	const QString normalized = line.text.trimmed().toCaseFolded();
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (const auto &value : settings->snapshot().value(QStringLiteral("botCommands")).toArray()) {
		const auto command = value.toObject();
		if (!command.value(QStringLiteral("enabled")).toBool()) continue;
		const auto trigger = command.value(QStringLiteral("trigger")).toString().toCaseFolded();
		if (normalized != trigger && !normalized.startsWith(trigger + QLatin1Char(' '))) continue;
		const QString key = line.accountId + trigger +
			(command.value(QStringLiteral("perViewer")).toBool() ? line.name.toCaseFolded() : QString{});
		const int wait = command.value(QStringLiteral("cooldownMs")).toInt();
		if (wait > 0 && botCooldowns.contains(key) && now - botCooldowns.value(key) < wait) break;
		botCooldowns[key] = now;
		sendMessage(command.value(QStringLiteral("reply")).toString(), {line.accountId});
		break;
	}
	if (botCooldowns.size() > 2048) botCooldowns.clear();
}

void ChatGateway::sendMessage(const QString &message, const QStringList &accountIds)
{
	const QString text = message.trimmed();
	if (text.isEmpty() || text.size() > 200) {
		for (const auto &id : accountIds) emit sendStatus(id, QStringLiteral("Сообщение: 1–200 символов"), false);
		return;
	}
	QSet<QString> sentTo;
	for (const auto &requestedId : accountIds) {
		const QString id = canonicalChatId(requestedId);
		if (sentTo.contains(id)) continue;
		sentTo.insert(id);
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
