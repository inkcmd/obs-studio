#include "MultiStreamManager.hpp"

#include <QMenu>
#include <QTimer>

#include <algorithm>

namespace xinkx {

using namespace Qt::StringLiterals;

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

OBSEncoderAutoRelease createStreamEncoder(const StreamDestination &destination, const char *name)
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
	OBSEncoderAutoRelease encoder = obs_video_encoder_create(encoderId, name, settings, nullptr);
	if (encoder) {
		obs_encoder_set_video(encoder, obs_get_video());
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
	sessions.clear();
	for (const auto &account : configured) {
		if (enabled(account)) {
			start(account);
		}
	}
	updateStatus();
	return static_cast<int>(std::count_if(sessions.cbegin(), sessions.cend(),
						   [](const auto &entry) { return entry->running; }));
}

void MultiStreamManager::start(const StreamAccount &account)
{
	auto &session = *sessions.emplace_back(std::make_unique<Session>());
	session.manager = this;
	session.serial = ++nextSerial;
	session.account = account;
	const auto name = QByteArray("xinkx_extra_") + QByteArray::number(sessions.size() - 1);
	session.service = createStreamService(account.destination, (name + "_service").constData());
	const auto video = createStreamEncoder(account.destination, (name + "_video").constData());
	OBSDataAutoRelease audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", 160);
	OBSEncoderAutoRelease audio =
		obs_audio_encoder_create("ffmpeg_aac", (name + "_audio").constData(), audioSettings, 0, nullptr);
	OBSDataAutoRelease outputSettings = obs_data_create();
	obs_data_set_bool(outputSettings, "hide_server_url", true);
	session.output = obs_output_create("rtmp_output", (name + "_output").constData(), outputSettings, nullptr);
	if (!session.service || !video || !audio || !session.output) {
		session.failed = true;
		return;
	}
	obs_encoder_set_audio(audio, obs_get_audio());
	obs_output_set_video_encoder(session.output, video);
	obs_output_set_audio_encoder(session.output, audio, 0);
	obs_output_set_service(session.output, session.service);
	obs_output_set_reconnect_settings(session.output, 5, 5);
	session.stopped.Connect(obs_output_get_signal_handler(session.output), "stop", outputStopped, &session);
	session.stats.sampleTime = clock.elapsed();
	session.running = obs_output_start(session.output);
	session.failed = !session.running;
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
				entry->running = false;
				entry->failed = !entry->stopping && code != OBS_OUTPUT_SUCCESS;
				entry->stopping = false;
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
	if (force || !obs_output_active(session.output)) {
		obs_output_force_stop(session.output);
	} else {
		obs_output_stop(session.output);
	}
}

void MultiStreamManager::stopAll(bool force)
{
	for (auto &session : sessions) {
		stop(*session, force);
	}
	updateStatus();
}

void MultiStreamManager::shutdown()
{
	stopAll(true);
	sessions.clear();
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
		return account.destination.ready() ? u"Ошибка выхода"_s : u"Нужны параметры RTMP(S)"_s;
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
	return u"LIVE · %1 кбит/с · %2:%3:%4 · пропущено: %5"_s
		.arg(kbps).arg(seconds / 3600, 2, 10, QLatin1Char('0'))
		.arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'))
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

void MultiStreamManager::populateMenu(QMenu *menu)
{
	menu->clear();
	if (anythingActive()) {
		for (const auto &session : sessions) {
			const auto id = session->account.id;
			auto *action = menu->addAction(u"%1 · %2: %3"_s.arg(
				platformTitle(session->account.platform), session->account.displayName(), session->description()));
			action->setEnabled(session->running && !session->stopping);
			action->setToolTip(u"Остановить только этот выход"_s);
			connect(action, &QAction::triggered, this, [this, id] {
				for (auto &entry : sessions) {
					if (entry->account.id == id) {
						stop(*entry, false);
						updateStatus();
						break;
					}
				}
			});
		}
		return;
	}

	for (const auto &account : loadStreamAccounts()) {
		auto *action = menu->addAction(u"%1 · %2"_s.arg(platformTitle(account.platform), account.displayName()));
		action->setCheckable(true);
		action->setChecked(enabled(account));
		connect(action, &QAction::toggled, this, [this, id = account.id](bool checked) {
			selection[id] = checked;
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
	if (totalBytes && !timer.isValid()) {
		timer.start();
	}
	const auto elapsed = now - sampleTime;
	if (elapsed >= 250) {
		kbps = totalBytes >= bytes ? 8 * (totalBytes - bytes) / elapsed : 0;
		bytes = totalBytes;
		sampleTime = now;
	}
}

void MultiStreamManager::updateStatus()
{
	const auto now = clock.elapsed();
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
