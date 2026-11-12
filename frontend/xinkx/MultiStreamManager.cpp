#include "MultiStreamManager.hpp"
#include "PortraitCanvas.hpp"
#include "StudioDiagnostics.hpp"

#include <QMenu>
#include <QIcon>
#include <QTimer>

#include <algorithm>

namespace xinkx {

using namespace Qt::StringLiterals;

static QIcon platformIcon(const QString &platform)
{
	if (platform == u"twitch"_s || platform == u"youtube"_s || platform == u"kick"_s)
		return QIcon(u":/xinkx/%1.svg"_s.arg(platform));
	return QIcon();
}

OBSServiceAutoRelease createStreamService(const StreamDestination &destination, const char *name)
{
	if (!destination.ready()) {
		return {};
	}
	OBSDataAutoRelease settings = obs_data_create();
	if (!destination.serviceName.isEmpty()) {
		obs_data_set_string(settings, "service", destination.serviceName.toUtf8().constData());
		obs_data_set_string(settings, "server", destination.serviceServer.toUtf8().constData());
		obs_data_set_string(settings, "key", destination.streamKey.toUtf8().constData());
		return obs_service_create("rtmp_common", name, settings, nullptr);
	}
	obs_data_set_string(settings, "server", destination.ingestUrl.toUtf8().constData());
	obs_data_set_string(settings, "key", destination.streamKey.toUtf8().constData());
	return obs_service_create("rtmp_custom", name, settings, nullptr);
}

OBSEncoderAutoRelease createStreamEncoder(const StreamDestination &destination, const char *name, video_t *video)
{
	const auto nvenc = [] {
		if (obs_get_encoder_codec("obs_nvenc_h264_tex")) {
			return "obs_nvenc_h264_tex";
		}
		return obs_get_encoder_codec("ffmpeg_nvenc") ? "ffmpeg_nvenc" : nullptr;
	};
	const char *encoderId = destination.encoder == u"x264"_s ? "obs_x264" : nvenc();
	if (!encoderId && destination.encoder == u"auto"_s) {
		encoderId = "obs_x264";
	}
	if (!encoderId) {
		return {};
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "rate_control", "CBR");
	obs_data_set_int(settings, "bitrate", destination.bitrate);
	obs_data_set_int(settings, "keyint_sec", 2);
	obs_data_set_int(settings, "bf", 0);
	OBSEncoderAutoRelease encoder = obs_video_encoder_create(encoderId, name, settings, nullptr);
	if (encoder) {
		obs_encoder_set_video(encoder, video ? video : obs_get_video());
		obs_encoder_set_scaled_size(encoder, destination.width, destination.height);
		if (obs_encoder_get_caps(encoder) & OBS_ENCODER_CAP_PASS_TEXTURE) {
			// NVENC's CPU fallback can encode at the global resolution instead.
			obs_encoder_set_gpu_scale_type(encoder, OBS_SCALE_BICUBIC);
		}
	}
	return encoder;
}

MultiStreamManager::MultiStreamManager(QObject *parent) : QObject(parent)
{
	clock.start();
	auto *timer = new QTimer(this);
	timer->setInterval(500);
	connect(timer, &QTimer::timeout, this, &MultiStreamManager::updateStatus);
	timer->start();
}

MultiStreamManager::~MultiStreamManager()
{
	shutdown();
}

bool MultiStreamManager::hasConfiguredAccounts() const
{
	return !loadStreamAccounts(false).isEmpty();
}

bool MultiStreamManager::enabled(const StreamAccount &account) const
{
	return selection.value(account.id, account.enabled);
}

int MultiStreamManager::startConfigured()
{
	if (anythingActive()) {
		return activeCount();
	}
	configured = loadStreamAccounts();
	for (auto &entry : sessions) entry->stopped.Disconnect();
	sessions.clear();
	liveMode = false;
	for (const auto &account : configured) {
		if (enabled(account)) {
			start(account);
		}
	}
	const int started = static_cast<int>(std::count_if(sessions.cbegin(), sessions.cend(),
						   [](const auto &entry) { return entry->running; }));
	liveMode = started > 0;
	updateStatus();
	return started;
}

void MultiStreamManager::start(const StreamAccount &account)
{
	auto &session = *sessions.emplace_back(std::make_unique<Session>());
	session.manager = this;
	session.serial = ++nextSerial;
	session.account = account;
	// Two RTMP connections with the same ingest key race for the same YouTube
	// livestream. A healthy bitrate on the client does not prove YouTube has
	// decoded the intended stream. Require an independent key for each output.
	const auto &dst = account.destination;
	for (const auto &existing : sessions) {
		if (existing.get() == &session || !existing->running) continue;
		const auto &other = existing->account.destination;
		const QString endpoint = dst.serviceName.isEmpty() ? dst.ingestUrl : dst.serviceServer;
		const QString otherEndpoint = other.serviceName.isEmpty() ? other.ingestUrl : other.serviceServer;
		if (endpoint == otherEndpoint && !dst.streamKey.isEmpty() && dst.streamKey == other.streamKey) {
			session.failed = true;
			session.errorDescription = u"Одинаковый RTMP-ключ у двух выходов"_s;
			writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Duplicate ingest destination blocked: %1")
				.arg(account.platform));
			return;
		}
	}
	const auto name = QByteArray("xinkx_extra_") + QByteArray::number(sessions.size() - 1);
	session.service = createStreamService(account.destination, (name + "_service").constData());
	video_t *videoMix = account.destination.portrait ? PortraitCanvas::instance().video() : obs_get_video();
	const auto video = videoMix ? createStreamEncoder(account.destination, (name + "_video").constData(), videoMix)
		: OBSEncoderAutoRelease{};
	OBSDataAutoRelease audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", 160);
	OBSEncoderAutoRelease audio =
		obs_audio_encoder_create("ffmpeg_aac", (name + "_audio").constData(), audioSettings, 0, nullptr);
	OBSDataAutoRelease outputSettings = obs_data_create();
	obs_data_set_bool(outputSettings, "hide_server_url", true);
	session.output = obs_output_create("rtmp_output", (name + "_output").constData(), outputSettings, nullptr);
	if (!session.service || !video || !audio || !session.output) {
		session.failed = true;
		writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Output setup failed: %1, portrait=%2")
			.arg(account.platform).arg(int(account.destination.portrait)));
		return;
	}
	obs_encoder_set_audio(audio, obs_get_audio());
	obs_output_set_video_encoder(session.output, video);
	obs_output_set_audio_encoder(session.output, audio, 0);
	obs_output_set_service(session.output, session.service);
	obs_output_set_reconnect_settings(session.output, 5, 5);
	session.stopped.Connect(obs_output_get_signal_handler(session.output), "stop", outputStopped, &session);
	session.stats.sampleTime = clock.elapsed();
	writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Starting output %1, serial=%2")
		.arg(account.platform).arg(session.serial));
	session.running = obs_output_start(session.output);
	session.failed = !session.running;
	writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Start %1: accepted=%2, portrait=%3, %4x%5")
		.arg(account.platform).arg(int(session.running)).arg(int(account.destination.portrait))
		.arg(account.destination.width).arg(account.destination.height));
}

void MultiStreamManager::restart(Session &session)
{
	if (session.stopping || (session.output && obs_output_active(session.output))) {
		requestRestart(session);
		return;
	}
	if (session.running) return;
	session.restartRequested = false;
	if (!session.output || !session.service) {
		session.failed = true;
		return;
	}
	// Reuse this account's encoders/output, rather than accumulating a new
	// output every time a checkbox is toggled. Counters restart from zero.
	session.stats = OutputStats{};
	session.stats.sampleTime = clock.elapsed();
	session.failed = false;
	writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Restarting output %1, serial=%2")
		.arg(session.account.platform).arg(session.serial));
	session.running = obs_output_start(session.output);
	session.failed = !session.running;
	if (session.running) session.stats.bytes = obs_output_get_total_bytes(session.output);
	writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Restart %1: accepted=%2")
		.arg(session.account.platform).arg(int(session.running)));
}

void MultiStreamManager::requestRestart(Session &session)
{
	session.restartRequested = true;
	if (session.restartPending) return;
	session.restartPending = true;
	const quint64 serial = session.serial;
	// RTMP output stop is asynchronous. Never call obs_output_start from a stop
	// callback or while libobs still reports this output as active.
	QTimer::singleShot(500, this, [this, serial] {
		for (auto &entry : sessions) {
			if (entry->serial != serial) continue;
			entry->restartPending = false;
			if (!entry->restartRequested || !liveMode || !enabled(entry->account)) return;
			if (entry->stopping || (entry->output && obs_output_active(entry->output))) {
				if (++entry->restartAttempts <= 20) requestRestart(*entry);
				else {
					entry->restartRequested = false;
					entry->failed = true;
					writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Restart timeout: %1")
						.arg(entry->account.platform));
					updateStatus();
				}
				return;
			}
			entry->restartAttempts = 0;
			entry->running = false;
			restart(*entry);
			updateStatus();
			return;
		}
	});
}

void MultiStreamManager::outputStopped(void *data, calldata_t *params)
{
	auto *session = static_cast<Session *>(data);
	auto *manager = session->manager;
	const auto code = calldata_int(params, "code");
	const auto serial = session->serial;
	QMetaObject::invokeMethod(manager, [manager, serial, code] {
		for (auto &entry : manager->sessions) {
			if (entry->serial == serial) {
				const int frames = entry->output ? obs_output_get_total_frames(entry->output) : 0;
				writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Stopped %1: code=%2, frames=%3, restarting=%4")
					.arg(entry->account.platform).arg(code).arg(frames).arg(int(entry->restartRequested)));
				entry->lastStopCode = code;
				entry->running = false;
				entry->failed = !entry->stopping && code != OBS_OUTPUT_SUCCESS;
				entry->stopping = false;
				if (entry->restartRequested && manager->liveMode && manager->enabled(entry->account))
					manager->requestRestart(*entry);
				else entry->restartRequested = false;
				manager->updateStatus();
				break;
			}
		}
	}, Qt::QueuedConnection);
}

void MultiStreamManager::stop(Session &session, bool force)
{
	if (!session.running || (session.stopping && !force)) {
		return;
	}
	session.stopping = true;
	writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Stopping output %1, serial=%2, force=%3")
		.arg(session.account.platform).arg(session.serial).arg(int(force)));
	if (force || !obs_output_active(session.output)) {
		obs_output_force_stop(session.output);
	} else {
		obs_output_stop(session.output);
	}
	writeDiagnostic(QStringLiteral("rtmp"), QStringLiteral("Stop requested %1, serial=%2")
		.arg(session.account.platform).arg(session.serial));
}

void MultiStreamManager::stopAll(bool force)
{
	liveMode = false;
	for (auto &session : sessions) {
		session->restartRequested = false;
		session->restartAttempts = 0;
		stop(*session, force);
	}
	updateStatus();
}

void MultiStreamManager::shutdown()
{
	// OBS calls shutdown before clearing scenes. QObject also destroys this
	// child later, so the destructor must not repeat the teardown.
	if (shuttingDown) return;
	shuttingDown = true;
	stopAll(true);
	for (auto &entry : sessions) entry->stopped.Disconnect();
	sessions.clear();
	PortraitCanvas::instance().shutdown();
}

bool MultiStreamManager::anythingActive() const
{
	return std::any_of(sessions.cbegin(), sessions.cend(), [](const auto &session) { return session->running; });
}

int MultiStreamManager::activeCount() const
{
	return static_cast<int>(std::count_if(sessions.cbegin(), sessions.cend(), [](const auto &session) {
		return session->running && session->stats.timer.isValid() && !obs_output_reconnecting(session->output);
	}));
}

int MultiStreamManager::configuredCount() const
{
	const auto accounts = loadStreamAccounts(false);
	return static_cast<int>(std::count_if(accounts.cbegin(), accounts.cend(), [this](const auto &account) {
		return enabled(account);
	}));
}

uint64_t MultiStreamManager::totalBytes() const
{
	uint64_t total = 0;
	for (const auto &session : sessions) {
		if (session->running && session->output) {
			total += obs_output_get_total_bytes(session->output);
		}
	}
	return total;
}

uint64_t MultiStreamManager::extraBitrate() const
{
	uint64_t total = 0;
	for (const auto &session : sessions) {
		if (session->running) {
			total += session->stats.kbps;
		}
	}
	return total;
}

float MultiStreamManager::worstCongestion() const
{
	float worst = 0.0f;
	for (const auto &session : sessions) {
		if (session->running && session->output) {
			worst = std::max(worst, obs_output_get_congestion(session->output));
		}
	}
	return worst;
}

int MultiStreamManager::totalDroppedFrames() const
{
	int total = 0;
	for (const auto &session : sessions) {
		if (session->running && session->output) {
			total += obs_output_get_frames_dropped(session->output);
		}
	}
	return total;
}

int MultiStreamManager::totalVideoFrames() const
{
	int total = 0;
	for (const auto &session : sessions) {
		if (session->running && session->output) {
			total += obs_output_get_total_frames(session->output);
		}
	}
	return total;
}

QString MultiStreamManager::Session::description() const
{
	if (failed) {
		if (!errorDescription.isEmpty()) return errorDescription;
		return account.destination.ready() ? u"Ошибка выхода (OBS code %1)"_s.arg(lastStopCode)
			: u"Нужны параметры RTMP(S)"_s;
	}
	if (!running) {
		return u"Остановлен"_s;
	}
	if (stopping) {
		return u"Остановка"_s;
	}
	if (obs_output_reconnecting(output)) {
		return u"Переподключение"_s;
	}
	return stats.liveDescription(output);
}

QString MultiStreamManager::OutputStats::liveDescription(obs_output_t *output) const
{
	if (!timer.isValid()) {
		return u"Подключение"_s;
	}
	const auto seconds = timer.elapsed() / 1000;
	if (seconds >= 8 && kbps > 500 && videoFrames == 0)
		return u"Поток передаёт данные, но видеокадры не подтверждены"_s;
	return u"LIVE · %1 кбит/с · %2:%3:%4 · кадров: %5 · пропущено: %6"_s
		.arg(kbps).arg(seconds / 3600, 2, 10, QLatin1Char('0'))
		.arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'))
		.arg(videoFrames)
		.arg(obs_output_get_frames_dropped(output));
}

QString MultiStreamManager::sessionDetails() const
{
	QStringList details;
	for (const auto &session : sessions) {
		details.push_back(platformTitle(session->account.platform) + u" · "_s +
				  session->account.displayName() + u": "_s + session->description());
	}
	return details.join(QLatin1Char('\n'));
}

QVector<MultiStreamManager::OutputSnapshot> MultiStreamManager::outputSnapshots() const
{
	QVector<OutputSnapshot> rows;
	for (const auto &account : loadStreamAccounts(false)) {
		if (!enabled(account)) continue;
		OutputSnapshot state;
		state.id = account.id;
		state.platform = account.platform;
		state.name = account.displayName();
		for (const auto &session : sessions) {
			if (session->account.id != account.id) continue;
			state.live = session->running && session->output && obs_output_active(session->output);
			state.reconnecting = session->running && session->output && obs_output_reconnecting(session->output);
			state.kbps = state.live ? session->stats.kbps : 0;
			state.dropped = session->output ? obs_output_get_frames_dropped(session->output) : 0;
			state.videoFrames = session->stats.videoFrames;
			state.state = session->description();
			break;
		}
		rows.push_back(state);
	}
	return rows;
}

void MultiStreamManager::populateMenu(QMenu *menu)
{
	menu->clear();
	const auto accounts = liveMode ? configured : loadStreamAccounts();
	for (const auto &account : accounts) {
		Session *session = nullptr;
		for (const auto &entry : sessions)
			if (entry->account.id == account.id) { session = entry.get(); break; }
		const QString description = liveMode
			? u": %1"_s.arg(session ? session->description() : u"Остановлен"_s) : QString{};
		auto *action = menu->addAction(u"%1 · %2%3"_s.arg(
			platformTitle(account.platform), account.displayName(), description));
		action->setIcon(platformIcon(account.platform));
		action->setCheckable(true);
		action->setChecked(liveMode
			? session && ((session->running && !session->stopping) || session->restartRequested)
			: enabled(account));
		action->setToolTip(liveMode ? u"Включить или отключить только этот RTMP-выход"_s
			: u"Выбрать площадку для следующего запуска"_s);
		connect(action, &QAction::toggled, this, [this, account](bool checked) {
			selection[account.id] = checked;
			if (liveMode) {
				Session *current = nullptr;
				for (const auto &entry : sessions)
					if (entry->account.id == account.id) { current = entry.get(); break; }
				if (checked) {
					if (current && (!current->output || !current->service)) {
						// Retry an account whose previous setup was refused or failed.
						for (auto it = sessions.begin(); it != sessions.end(); ++it) {
							if (it->get() != current) continue;
							current->stopped.Disconnect();
							sessions.erase(it);
							break;
						}
						start(account);
					} else if (current) requestRestart(*current);
					else start(account);
				} else if (current) {
					current->restartRequested = false;
					stop(*current, false);
				}
			}
			updateStatus();
		});
	}
	if (menu->isEmpty()) {
		menu->addAction(u"Добавь аккаунты в настройках трансляции"_s)->setEnabled(false);
	}
}

void MultiStreamManager::OutputStats::sample(obs_output_t *output, qint64 now)
{
	const auto totalBytes = obs_output_get_total_bytes(output);
	if (totalBytes > bytes && !timer.isValid()) {
		timer.start();
	}
	const auto elapsed = now - sampleTime;
	if (elapsed >= 250) {
		kbps = totalBytes >= bytes ? 8 * (totalBytes - bytes) / elapsed : 0;
		videoFrames = obs_output_get_total_frames(output);
		bytes = totalBytes;
		sampleTime = now;
	}
}

void MultiStreamManager::updateStatus()
{
	// Do not notify UI observers while OBS/QWidget ownership is being torn down.
	if (shuttingDown) return;
	const auto now = clock.elapsed();
	if (std::any_of(sessions.cbegin(), sessions.cend(), [](const auto &item) {
		return item->account.destination.portrait && item->running;
	})) PortraitCanvas::instance().sync();
	for (auto &session : sessions) {
		if (session->running) {
			session->stats.sample(session->output, now);
		}
	}
	const bool active = anythingActive();
	if (lastActive != active) {
		lastActive = active;
		emit activityChanged(active);
	}
	emit statusChanged();
}

} // namespace xinkx
