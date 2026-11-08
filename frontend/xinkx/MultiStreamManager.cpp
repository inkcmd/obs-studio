#include "MultiStreamManager.hpp"
#include "StreamAccounts.hpp"

#include <QByteArray>
#include <QTimer>

#include <algorithm>
#include <cstring>

namespace xinkx {

namespace {

bool hasEncoder(const char *name)
{
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		if (id && std::strcmp(id, name) == 0) {
			return true;
		}
	}
	return false;
}

QString startFailure(obs_output_t *output)
{
	const char *detail = output ? obs_output_get_last_error(output) : nullptr;
	return detail && *detail ? QString::fromUtf8(detail) : QStringLiteral("OBS не запустил RTMP-выход");
}

} // namespace

MultiStreamManager::MultiStreamManager(QObject *parent) : QObject(parent)
{
	auto *timer = new QTimer(this);
	timer->setInterval(500);
	QObject::connect(timer, &QTimer::timeout, this, [this] { updateStatus(); });
	timer->start();
}

MultiStreamManager::~MultiStreamManager()
{
	shutdown();
}

void MultiStreamManager::startConfigured()
{
	// The OBS primary output has already started. These are additional encoded
	// outputs of the same OBS program, not an independent streaming subsystem.
	if (anythingActive()) {
		return;
	}
	for (auto &session : sessions) {
		release(session);
	}
	const QVector<StreamAccount> accounts = loadStreamAccounts();
	sessions.clear();
	sessions.resize(static_cast<size_t>(accounts.size()));
	for (int i = 0; i < accounts.size(); ++i) {
		const StreamAccount &account = accounts[i];
		if (!account.enabled) {
			continue;
		}
		StreamDestination dest;
		dest.url = account.ingestUrl;
		dest.key = account.streamKey;
		dest.encoder = account.encoder;
		dest.bitrateKbps = account.bitrate;
		dest.width = account.width;
		dest.height = account.height;
		QString error;
		if (!start(i, dest, &error)) {
			blog(LOG_WARNING, "XINKX extra stream #%d failed: %s", i + 1, error.toUtf8().constData());
			emit streamError(QStringLiteral("%1: %2").arg(account.label, error));
		}
	}
}

void MultiStreamManager::stopAll()
{
	for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
		stop(i);
	}
}

int MultiStreamManager::activeCount() const
{
	int result = 0;
	for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
		result += active(i) ? 1 : 0;
	}
	return result;
}

int MultiStreamManager::configuredCount() const
{
	int result = 0;
	for (const auto &account : loadStreamAccounts()) {
		result += account.enabled ? 1 : 0;
	}
	return result;
}

uint64_t MultiStreamManager::totalBytes() const
{
	uint64_t total = 0;
	for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
		if (active(i)) {
			total += obs_output_get_total_bytes(sessions[i].output);
		}
	}
	return total;
}

int MultiStreamManager::totalDroppedFrames() const
{
	int total = 0;
	for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
		if (active(i)) {
			total += obs_output_get_frames_dropped(sessions[i].output);
		}
	}
	return total;
}

int MultiStreamManager::totalVideoFrames() const
{
	int total = 0;
	for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
		if (active(i)) {
			total += obs_output_get_total_frames(sessions[i].output);
		}
	}
	return total;
}

void MultiStreamManager::shutdown()
{
	for (auto &session : sessions) {
		if (session.output && obs_output_active(session.output)) {
			// A normal stop is asynchronous; force a synchronous shutdown on exit.
			obs_output_force_stop(session.output);
		}
		release(session);
	}
}

void MultiStreamManager::release(Session &session)
{
	if (session.output) {
		obs_output_release(session.output);
		session.output = nullptr;
	}
	if (session.service) {
		obs_service_release(session.service);
		session.service = nullptr;
	}
	if (session.video) {
		obs_encoder_release(session.video);
		session.video = nullptr;
	}
	if (session.audio) {
		obs_encoder_release(session.audio);
		session.audio = nullptr;
	}
	session.connecting = false;
	session.stopRequested = false;
	session.started = false;
}

bool MultiStreamManager::active(int index) const
{
	return index >= 0 && index < static_cast<int>(sessions.size()) && sessions[index].output &&
	       obs_output_active(sessions[index].output);
}

bool MultiStreamManager::anythingActive() const
{
	for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
		if (active(i)) {
			return true;
		}
	}
	return false;
}

bool MultiStreamManager::start(int index, const StreamDestination &destination, QString *error)
{
	if (index < 0 || index >= static_cast<int>(sessions.size())) {
		return false;
	}
	Session &session = sessions[index];
	const auto reject = [this, &session, error, index](const QString &reason) {
		session.message = reason;
		if (error) {
			*error = reason;
		}
		emit destinationChanged(index);
		return false;
	};
	if (active(index) || session.connecting) {
		if (error) {
			*error = QStringLiteral("Выход уже запущен");
		}
		return false;
	}
	const QString url = destination.url.trimmed();
	if (!(url.startsWith(QStringLiteral("rtmp://"), Qt::CaseInsensitive) ||
	      url.startsWith(QStringLiteral("rtmps://"), Qt::CaseInsensitive)) ||
	    destination.key.trimmed().isEmpty()) {
		return reject(QStringLiteral("Укажи RTMP(S)-адрес и ключ трансляции"));
	}
	obs_video_info videoInfo{};
	if (!obs_get_video_info(&videoInfo) || !obs_get_video() || !obs_get_audio()) {
		return reject(QStringLiteral("Видео и звук OBS ещё не инициализированы"));
	}
	if (destination.width < 320 || destination.height < 240 || destination.width % 2 != 0 ||
	    destination.height % 2 != 0 || destination.bitrateKbps < 500 || destination.bitrateKbps > 51000) {
		return reject(QStringLiteral("Некорректные размеры кадра или битрейт"));
	}
	// The previous inactive output can be discarded; no local RTMP relay is used.
	release(session);
	const QByteArray name = QByteArray("xinkx_extra_") + QByteArray::number(index);
	obs_data_t *serviceSettings = obs_data_create();
	obs_data_set_string(serviceSettings, "server", url.toUtf8().constData());
	obs_data_set_string(serviceSettings, "key", destination.key.toUtf8().constData());
	session.service = obs_service_create("rtmp_custom", (name + "_service").constData(), serviceSettings, nullptr);
	obs_data_release(serviceSettings);

	const QString encoderChoice = destination.encoder;
	const char *encoderId = nullptr;
	if (encoderChoice == QStringLiteral("x264")) {
		encoderId = "obs_x264";
	} else if (encoderChoice == QStringLiteral("nvenc")) {
		encoderId = hasEncoder("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "ffmpeg_nvenc";
	} else {
		encoderId = hasEncoder("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" :
			    hasEncoder("ffmpeg_nvenc") ? "ffmpeg_nvenc" : "obs_x264";
	}

	obs_data_t *videoSettings = obs_data_create();
	obs_data_set_string(videoSettings, "rate_control", "CBR");
	obs_data_set_int(videoSettings, "bitrate", destination.bitrateKbps);
	obs_data_set_int(videoSettings, "keyint_sec", 2);
	session.video = obs_video_encoder_create(encoderId, (name + "_video").constData(), videoSettings, nullptr);
	obs_data_release(videoSettings);

	obs_data_t *audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", 160);
	session.audio = obs_audio_encoder_create("ffmpeg_aac", (name + "_audio").constData(), audioSettings, 0,
					  nullptr);
	obs_data_release(audioSettings);
	session.output = obs_output_create("rtmp_output", (name + "_output").constData(), nullptr, nullptr);
	if (!session.video || !session.audio || !session.service || !session.output) {
		session.message = QStringLiteral("Не удалось создать RTMP-выход или H.264/AAC энкодер");
		if (error) {
			*error = session.message;
		}
		release(session);
		emit destinationChanged(index);
		return false;
	}
	obs_encoder_set_video(session.video, obs_get_video());
	obs_encoder_set_audio(session.audio, obs_get_audio());
	obs_encoder_set_scaled_size(session.video, static_cast<uint32_t>(destination.width),
				    static_cast<uint32_t>(destination.height));
	obs_output_set_video_encoder(session.output, session.video);
	obs_output_set_audio_encoder(session.output, session.audio, 0);
	obs_output_set_service(session.output, session.service);
	obs_output_set_reconnect_settings(session.output, 5, 5);
	session.stopRequested = false;
	session.started = false;
	session.connecting = true;
	session.message.clear();
	if (!obs_output_start(session.output)) {
		session.message = startFailure(session.output);
		if (error) {
			*error = session.message;
		}
		release(session);
		emit destinationChanged(index);
		return false;
	}
	emit destinationChanged(index);
	return true;
}

void MultiStreamManager::stop(int index)
{
	if (index < 0 || index >= static_cast<int>(sessions.size())) {
		return;
	}
	Session &session = sessions[index];
	if (session.output && obs_output_active(session.output)) {
		session.stopRequested = true;
		session.connecting = false;
		obs_output_stop(session.output);
		emit destinationChanged(index);
	}
}

QString MultiStreamManager::description(int index) const
{
	if (index < 0 || index >= static_cast<int>(sessions.size())) {
		return QString();
	}
	const Session &session = sessions[index];
	if (session.stopRequested && active(index)) {
		return QStringLiteral("Остановка...");
	}
	if (active(index)) {
		if (session.connecting && !session.started) {
			return QStringLiteral("Подключение...");
		}
		const uint64_t bytes = obs_output_get_total_bytes(session.output);
		const int dropped = obs_output_get_frames_dropped(session.output);
		return QStringLiteral("LIVE · %1 МиБ · пропущено кадров: %2")
			.arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1))
			.arg(dropped);
	}
	return session.message.isEmpty() ? QStringLiteral("Остановлен") : session.message;
}

void MultiStreamManager::updateStatus()
{
	for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
		Session &session = sessions[i];
		if (session.output && !obs_output_active(session.output) &&
		    (session.connecting || session.started || session.stopRequested)) {
			if (!session.stopRequested) {
				session.message = startFailure(session.output);
			} else {
				session.message.clear();
			}
			session.connecting = false;
			session.stopRequested = false;
			session.started = false;
		}
		if (session.output && obs_output_active(session.output) && session.connecting) {
			// OBS reports active during connection. Actual network progress appears in stats.
			if (obs_output_get_total_bytes(session.output) > 0) {
				session.started = true;
				session.connecting = false;
			}
		}
		const QString display = description(i);
		if (display != session.lastDisplay) {
			session.lastDisplay = display;
			emit destinationChanged(i);
		}
	}
}

} // namespace xinkx
