#include "YouTubeStudioBridge.hpp"

#include "NativeWebSocket.hpp"
#include "StreamAccounts.hpp"
#include "StudioDiagnostics.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <utility>

namespace xinkx {
namespace {

QString locateBrowser()
{
#ifdef _WIN32
	const auto environment = QProcessEnvironment::systemEnvironment();
	const QStringList roots{environment.value(QStringLiteral("PROGRAMFILES")),
		 environment.value(QStringLiteral("PROGRAMFILES(X86)")),
		 environment.value(QStringLiteral("LOCALAPPDATA"))};
	for (const auto &root : roots) {
		if (root.isEmpty()) continue;
		for (const auto &relative : {QStringLiteral("Google/Chrome/Application/chrome.exe"),
				QStringLiteral("Microsoft/Edge/Application/msedge.exe")}) {
			const QString path = QDir(root).filePath(relative);
			if (QFileInfo::exists(path)) return path;
		}
	}
	for (const auto &name : {QStringLiteral("chrome.exe"), QStringLiteral("msedge.exe")}) {
		const auto path = QStandardPaths::findExecutable(name);
		if (!path.isEmpty()) return path;
	}
#endif
	return {};
}

bool validVideo(const QString &videoId)
{
	static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
	return id.match(videoId).hasMatch();
}

QString studioProfile()
{
	// A separate Chromium profile, never the user's normal browser data.
	const QString settings = studioSettingsPath();
	if (settings.isEmpty()) return {};
	return QDir(QFileInfo(settings).absolutePath()).filePath(QStringLiteral("youtube-studio-browser"));
}

QJsonObject output(const QJsonObject &message)
{
	const QJsonObject result = message.value(QStringLiteral("result")).toObject();
	const QJsonObject exception = result.value(QStringLiteral("exceptionDetails")).toObject();
	if (!exception.isEmpty()) return {{QStringLiteral("error"), QStringLiteral("Ошибка JavaScript в YouTube Studio")}};
	const QJsonObject remote = result.value(QStringLiteral("result")).toObject();
	if (remote.value(QStringLiteral("value")).isObject()) return remote.value(QStringLiteral("value")).toObject();
	if (remote.value(QStringLiteral("value")).isString())
		return {{QStringLiteral("value"), remote.value(QStringLiteral("value"))}};
	return {{QStringLiteral("error"), QStringLiteral("Пустой/изменённый ответ YouTube Studio")}};
}

} // namespace

YouTubeStudioBridge::YouTubeStudioBridge(QObject *parent) : QObject(parent), profile(studioProfile())
{
	executable = locateBrowser();
	discovery.setInterval(500);
	connect(&discovery, &QTimer::timeout, this, &YouTubeStudioBridge::discover);
}

YouTubeStudioBridge::~YouTubeStudioBridge()
{
	discovery.stop();
	socket.reset();
}

void YouTubeStudioBridge::notify(const QString &message, bool ready)
{
	writeDiagnostic(QStringLiteral("youtube-studio"), message.left(220));
	emit status(message, ready);
}

bool YouTubeStudioBridge::isReadyFor(const QString &channelId) const
{
	return socketReady && !channelId.isEmpty() && verifiedChannel == channelId;
}

void YouTubeStudioBridge::openLogin()
{
	if (executable.isEmpty() || profile.isEmpty()) {
		notify(QStringLiteral("Не найден Chrome или Edge"));
		return;
	}
	if (socketReady) {
		notify(QStringLiteral("YouTube Studio уже подключена"), true);
		return;
	}
	QDir().mkpath(profile);
	const QStringList args{QStringLiteral("--user-data-dir=") + QDir::toNativeSeparators(profile),
		QStringLiteral("--no-first-run"), QStringLiteral("--new-window"),
		QStringLiteral("https://studio.youtube.com/")};
	if (!QProcess::startDetached(executable, args)) {
		notify(QStringLiteral("Не удалось открыть браузер YouTube Studio"));
		return;
	}
	notify(QStringLiteral("Войдите в YouTube Studio. Закройте ВСЕ окна этого профиля и нажмите «Проверить сессию»."));
}

void YouTubeStudioBridge::connectChannel(const QString &channelId)
{
	if (channelId.isEmpty()) {
		notify(QStringLiteral("YouTube OAuth не вернул ID канала. Сначала подключите аккаунт в XINKX."));
		return;
	}
	if (isReadyFor(channelId)) {
		notify(QStringLiteral("YouTube Studio: проверен канал %1").arg(channelId), true);
		return;
	}
	if (executable.isEmpty() || profile.isEmpty()) { notify(QStringLiteral("Chrome/Edge или конфигурация XINKX недоступны")); return; }
	if (!QFileInfo::exists(QDir(profile).filePath(QStringLiteral("Local State")))) {
		notify(QStringLiteral("Сначала выполните вход через «Подключить YouTube Studio»"));
		return;
	}
	if (socket) {
		socket.reset();
		pending.clear();
	}
	socketReady = false;
	verifiedChannel.clear();
	expectedChannel = channelId;
	QDir().mkpath(profile);
	// Remote debugging is never enabled for the user's default Chrome profile.
	// Port 0 lets Chrome choose a free loopback port; never expose this externally.
	const QStringList args{QStringLiteral("--user-data-dir=") + QDir::toNativeSeparators(profile),
		QStringLiteral("--remote-debugging-address=127.0.0.1"),
		QStringLiteral("--remote-debugging-port=0"), QStringLiteral("--no-first-run"),
		QStringLiteral("--new-window"),
		QStringLiteral("https://studio.youtube.com/channel/%1/dashboard").arg(channelId)};
	if (!QProcess::startDetached(executable, args)) {
		notify(QStringLiteral("Chrome/Edge не запущен"));
		return;
	}
	discoveryTries = 0;
	discovery.start();
	notify(QStringLiteral("Проверяем сессию YouTube Studio…"));
}

void YouTubeStudioBridge::discover()
{
	if (++discoveryTries > 40) {
		discovery.stop();
		notify(QStringLiteral("Нет доступа к CDP. Закройте окна профиля YouTube Studio и повторите проверку."));
		return;
	}
	QFile file(QDir(profile).filePath(QStringLiteral("DevToolsActivePort")));
	if (!file.open(QIODevice::ReadOnly)) return;
	bool ok = false;
	const int port = QString::fromUtf8(file.readLine()).trimmed().toInt(&ok);
	if (!ok || port < 1024 || port > 65535) return;
	discovery.stop();
	QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/json/list").arg(port)));
	auto *reply = network.get(request);
	QTimer::singleShot(5000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		const QByteArray body = reply->readAll();
		const bool failed = reply->error() != QNetworkReply::NoError;
		reply->deleteLater();
		if (failed) {
			discovery.start();
			return;
		}
		const auto pages = QJsonDocument::fromJson(body).array();
		QUrl socketUrl;
		for (const auto &value : pages) {
			const QJsonObject page = value.toObject();
			if (page.value(QStringLiteral("type")).toString() != QStringLiteral("page") ||
			    !page.value(QStringLiteral("url")).toString().startsWith(QStringLiteral("https://studio.youtube.com/")))
				continue;
			socketUrl = QUrl(page.value(QStringLiteral("webSocketDebuggerUrl")).toString());
			break;
		}
		if (!socketUrl.isValid() ||
		    (socketUrl.host() != QStringLiteral("127.0.0.1") && socketUrl.host() != QStringLiteral("localhost"))) {
			discovery.start();
			return;
		}
		socket = std::make_unique<NativeWebSocket>(this, socketUrl,
			[this](const QByteArray &data) {
				const QJsonObject message = QJsonDocument::fromJson(data).object();
				const int id = message.value(QStringLiteral("id")).toInt();
				if (id && pending.contains(id)) {
					const auto done = pending.take(id);
					done(message.value(QStringLiteral("error")).isObject()
						? QJsonObject{{QStringLiteral("error"), QStringLiteral("CDP отклонил команду")}}
						: message);
				}
			},
			[this](const QString &) {
				socketReady = false;
				verifiedChannel.clear();
				pending.clear();
				notify(QStringLiteral("Сессия браузера закрыта. Требуется проверка подключения."));
			},
			[this] {
				socketReady = true;
				verifyChannel(12);
			});
	});
}

void YouTubeStudioBridge::verifyChannel(int retries)
{
	// Studio must identify the channel itself, not merely have an arbitrary
	// user-supplied /channel/<id> URL. Wait for app initialization before testing.
	evaluate(QStringLiteral(R"JS((() => {
		const config = window.ytcfg;
		const id = config && typeof config.get === 'function' ?
			(config.get('CHANNEL_ID') || config.get('CHANNEL_ID_FOR_UPLOAD') || '') : '';
		return {channelId: id, studio: location.origin === 'https://studio.youtube.com' &&
			!!document.querySelector('ytcp-app')};
	})())JS"), [this, retries](const QJsonObject &value) {
		const QJsonObject result = output(value);
		const QString actual = result.value(QStringLiteral("channelId")).toString();
		if (result.value(QStringLiteral("studio")).toBool() && actual == expectedChannel) {
			verifiedChannel = expectedChannel;
			notify(QStringLiteral("YouTube Studio подключена: канал %1 проверен").arg(expectedChannel), true);
		} else if (retries > 0 && actual.isEmpty() && !result.contains(QStringLiteral("error"))) {
			QTimer::singleShot(750, this, [this, retries] { verifyChannel(retries - 1); });
		} else {
			verifiedChannel.clear();
			notify(QStringLiteral("Канал YouTube Studio не совпадает с OAuth или не удалось проверить вход. Изменения запрещены."));
		}
	});
}

void YouTubeStudioBridge::command(const QString &method, const QJsonObject &params, JsonDone done)
{
	if (!socketReady || !socket) {
		if (done) done({{QStringLiteral("error"), QStringLiteral("Нет подключения CDP")}});
		return;
	}
	const int id = ++commandId;
	const QJsonObject packet{{QStringLiteral("id"), id}, {QStringLiteral("method"), method},
		{QStringLiteral("params"), params}};
	if (done) pending.insert(id, std::move(done));
	if (!socket->send(QJsonDocument(packet).toJson(QJsonDocument::Compact))) {
		const auto callback = pending.take(id);
		if (callback) callback({{QStringLiteral("error"), QStringLiteral("Ошибка отправки CDP")}});
		return;
	}
	QTimer::singleShot(12000, this, [this, id] {
		if (!pending.contains(id)) return;
		const auto callback = pending.take(id);
		callback({{QStringLiteral("error"), QStringLiteral("Таймаут CDP 12 секунд")}});
	});
}

void YouTubeStudioBridge::evaluate(const QString &expression, JsonDone done)
{
	command(QStringLiteral("Runtime.evaluate"), {{QStringLiteral("expression"), expression},
		{QStringLiteral("returnByValue"), true}, {QStringLiteral("awaitPromise"), true}}, std::move(done));
}

QString YouTubeStudioBridge::quoted(const QString &value)
{
	const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
	return QString::fromUtf8(array.mid(1, array.size() - 2));
}

void YouTubeStudioBridge::navigate(const QString &videoId, std::function<void(bool)> done)
{
	if (!validVideo(videoId) || !isReadyFor(expectedChannel)) { done(false); return; }
	command(QStringLiteral("Page.navigate"),
		{{QStringLiteral("url"), QStringLiteral("https://studio.youtube.com/video/%1/edit").arg(videoId)}},
		[this, done = std::move(done)](const QJsonObject &response) {
			if (response.contains(QStringLiteral("error"))) { done(false); return; }
			QTimer::singleShot(900, this, [this, done] { waitForGameField(12, done); });
		});
}

void YouTubeStudioBridge::waitForGameField(int retries, std::function<void(bool)> done)
{
	evaluate(QStringLiteral(R"JS((() => ({
		found: !!document.querySelector('ytcp-form-gaming input[type="text"]'),
		origin: location.origin,
		channel: window.ytcfg?.get?.('CHANNEL_ID') || ''
	}))())JS"), [this, retries, done](const QJsonObject &raw) {
		const auto result = output(raw);
		if (result.value(QStringLiteral("found")).toBool() &&
		    result.value(QStringLiteral("origin")).toString() == QStringLiteral("https://studio.youtube.com") &&
		    result.value(QStringLiteral("channel")).toString() == expectedChannel) {
			done(true);
		} else if (retries > 0) {
			QTimer::singleShot(650, this, [this, retries, done] { waitForGameField(retries - 1, done); });
		} else {
			evaluate(QStringLiteral(R"JS((() => ({
				login: location.hostname !== 'studio.youtube.com' ||
					location.pathname.includes('signin') || !!document.querySelector('a[href*="accounts.google.com/ServiceLogin"]')
			}))())JS"), [this, done](const QJsonObject &raw) {
				if (output(raw).value(QStringLiteral("login")).toBool()) {
					verifiedChannel.clear();
					notify(QStringLiteral("YouTube Studio: требуется повторный вход"));
				}
				done(false);
			});
		}
	});
}

void YouTubeStudioBridge::readGame(const QString &videoId, std::function<void(const QString &)> done)
{
	navigate(videoId, [this, done](bool ok) {
		if (!ok) { done({}); return; }
		evaluate(QStringLiteral(R"JS((() => ({
		name: document.querySelector('ytcp-form-gaming input[type="text"]')?.value || ''
	}))())JS"), [done](const QJsonObject &raw) {
			done(output(raw).value(QStringLiteral("name")).toString());
		});
	});
}

void YouTubeStudioBridge::getSelectedGame(const QString &videoId)
{
	if (operationBusy || !isReadyFor(expectedChannel)) {
		notify(QStringLiteral("YouTube Studio недоступна или занята")); return;
	}
	operationBusy = true;
	readGame(videoId, [this, videoId](const QString &name) {
		operationBusy = false;
		emit gameRead(videoId, name);
	});
}

void YouTubeStudioBridge::searchGames(const QString &videoId, const QString &query)
{
	const QString value = query.trimmed().left(100);
	if (value.size() < 2 || operationBusy || !isReadyFor(expectedChannel)) {
		notify(QStringLiteral("Сначала проверьте подключение YouTube Studio и выберите эфир")); return;
	}
	operationBusy = true;
	// Run entirely inside the already authenticated Studio renderer. Session
	// cookies, authorization hashes and changing ytcfg values NEVER leave it.
	// No fixed WEB_CREATOR version, cookies, SAPISIDHASH or request context.
	const QString script = QStringLiteral(R"JS((async () => {
		const q = %1;
		const cfg = window.ytcfg;
		const get = key => cfg && typeof cfg.get === 'function' ? cfg.get(key) : undefined;
		const context = get('INNERTUBE_CONTEXT');
		const key = get('INNERTUBE_API_KEY');
		if (!context || !key) return {error: 'YouTube Studio не предоставила контекст API'};
		const sid = document.cookie.split('; ').map(c => [c.slice(0, c.indexOf('=')), c.slice(c.indexOf('=')+1)])
			.find(([n]) => n === 'SAPISID' || n === '__Secure-3PAPISID');
		if (!sid || !sid[1]) return {error: 'Сессия Google недоступна; требуется повторный вход'};
		const timestamp = Math.floor(Date.now()/1000);
		const data = new TextEncoder().encode(`${timestamp} ${decodeURIComponent(sid[1])} ${location.origin}`);
		const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-1', data))]
			.map(b => b.toString(16).padStart(2, '0')).join('');
		const headers = {
			'Content-Type': 'application/json',
			'Authorization': `SAPISIDHASH ${timestamp}_${hash}`,
			'X-Goog-AuthUser': String(get('SESSION_INDEX') ?? 0)
		};
		const clientName = get('INNERTUBE_CONTEXT_CLIENT_NAME');
		const clientVersion = get('INNERTUBE_CONTEXT_CLIENT_VERSION');
		if (clientName) headers['X-Youtube-Client-Name'] = String(clientName);
		if (clientVersion) headers['X-Youtube-Client-Version'] = String(clientVersion);
		const delegated = get('DELEGATED_SESSION_ID');
		if (delegated) headers['X-Goog-PageId'] = delegated;
		try {
			const response = await fetch(`/youtubei/v1/gaming/game_title?alt=json&key=${encodeURIComponent(key)}`,
				{method:'POST', credentials:'include', headers, body:JSON.stringify({context, userInput:q})});
			const result = await response.json().catch(() => ({}));
			if (!response.ok) return {error:`game_title HTTP ${response.status}`, expired: response.status===401 || response.status===403};
			if (!Array.isArray(result.gameTitles)) return {error:'game_title: формат результата изменён'};
			return {games: result.gameTitles.filter(g => typeof g.title === 'string' &&
				typeof g.mid === 'string').slice(0, 30).map(g => ({title:g.title, mid:g.mid}))};
		} catch(e) {return {error: 'Ошибка обращения к YouTube Studio game_title'};}
	})())JS").arg(quoted(value));
	Q_UNUSED(videoId);
	evaluate(script, [this, value](const QJsonObject &raw) {
		operationBusy = false;
		const auto result = output(raw);
		if (result.contains(QStringLiteral("error"))) {
			if (result.value(QStringLiteral("expired")).toBool()) {
				verifiedChannel.clear();
				notify(QStringLiteral("YouTube Studio: HTTP 401/403. Требуется повторный вход"));
			}
			notify(QStringLiteral("Поиск YouTube Studio: ") + result.value(QStringLiteral("error")).toString());
			return;
		}
		emit gamesFound(value, result.value(QStringLiteral("games")).toArray());
	});
}

void YouTubeStudioBridge::failOperation(const QString &videoId, const QString &reason)
{
	operationBusy = false;
	emit gameSaved(videoId, reason, false);
	notify(reason);
}

void YouTubeStudioBridge::setGame(const QString &videoId, const QString &mid, const QString &title)
{
	if (!isReadyFor(expectedChannel) || operationBusy || !validVideo(videoId) ||
	    !mid.startsWith(QStringLiteral("/g/")) || title.trimmed().isEmpty()) {
		failOperation(videoId, QStringLiteral("YouTube Studio: сессия/эфир/игра не проверены")); return;
	}
	operationBusy = true;
	// Let YouTube Studio itself build metadata_update, including any changing
	// attestationResponseData, videoReadMask and authorization headers.
	navigate(videoId, [this, videoId, title](bool ok) {
		if (!ok) {
			failOperation(videoId, QStringLiteral("YouTube Studio: не открывается поле игры (категория «Игры»?)"));
			return;
		}
		const QString script = QStringLiteral(R"JS((async () => {
			const root = document.querySelector('ytcp-form-gaming');
			const input = root?.querySelector('input[type="text"]');
			if (!input) return {error:'Поле игры отсутствует'};
			const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set;
			setter.call(input, %1);
			input.dispatchEvent(new InputEvent('input', {bubbles:true, data:%1, inputType:'insertText'}));
			input.focus();
			await new Promise(resolve => setTimeout(resolve, 1050));
			const options = [...document.querySelectorAll(
				'ytcp-form-autocomplete-option, ytcp-form-autocomplete tp-yt-paper-item, ytcp-form-autocomplete [role="option"]')];
			const exact = options.filter(option => option.textContent.trim() === %1);
			if (exact.length !== 1) return {error: 'Игра не найдена однозначно в подсказках YouTube Studio'};
			exact[0].click();
			await new Promise(resolve => setTimeout(resolve, 400));
			const save = [...document.querySelectorAll('ytcp-button, button')].find(b =>
				b.id === 'save-button' || (/^(Save|Сохранить)$/.test(b.textContent.trim()) &&
				!b.hasAttribute('disabled')));
			if (!save) return {error:'Кнопка сохранения YouTube Studio не найдена'};
			save.click();
			return {clicked:true};
		})())JS").arg(quoted(title.trimmed()));
		evaluate(script, [this, videoId, title](const QJsonObject &raw) {
			const auto result = output(raw);
			if (!result.value(QStringLiteral("clicked")).toBool()) {
				failOperation(videoId, QStringLiteral("YouTube Studio: ") +
					result.value(QStringLiteral("error")).toString(QStringLiteral("Сохранение не выполнено")));
				return;
			}
			QTimer::singleShot(2000, this, [this, videoId, title] {
				readGame(videoId, [this, videoId, title](const QString &actual) {
					operationBusy = false;
					const bool verified = actual.compare(title.trimmed(), Qt::CaseInsensitive) == 0;
					const QString msg = verified
						? QStringLiteral("YouTube: игра «%1» подтверждена после повторного открытия Studio").arg(actual)
						: QStringLiteral("YouTube: сохранение игры не подтверждено. В Studio: «%1»").arg(actual);
					emit gameSaved(videoId, msg, verified);
					notify(msg, isReadyFor(expectedChannel));
				});
			});
		});
	});
}

} // namespace xinkx
