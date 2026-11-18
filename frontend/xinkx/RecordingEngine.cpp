#include "RecordingEngine.hpp"

#include "PortraitCanvas.hpp"
#include "StreamAccounts.hpp"
#include "StudioDiagnostics.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSBasicControls.hpp>

#include <obs-frontend-api.h>
#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>
#include <limits>
#include <vector>

namespace xinkx {
namespace {

QString configFile()
{
	char *profile = obs_frontend_get_current_profile_path();
	QString directory;
	if (profile) {
		directory = QString::fromUtf8(profile);
		bfree(profile);
	}
	if (directory.isEmpty()) directory = QFileInfo(studioSettingsPath()).absolutePath();
	return QDir(directory).filePath(QStringLiteral("xinkx-recording.json"));
}

QString defaultFolder()
{
	auto *config = obs_frontend_get_profile_config();
	QString path;
	if (config) {
		const char *mode = config_get_string(config, "Output", "Mode");
		if (mode && !strcmp(mode, "Advanced"))
			path = QString::fromUtf8(config_get_string(config, "AdvOut", "RecFilePath"));
		else
			path = QString::fromUtf8(config_get_string(config, "SimpleOutput", "FilePath"));
	}
	if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (path.isEmpty()) path = QDir::homePath();
	return path;
}

RecordingJob fallbackJob()
{
	RecordingJob job;
	job.id = QStringLiteral("obs-primary");
	job.kind = QStringLiteral("obs");
	job.name = QStringLiteral("Основная запись OBS");
	job.folder.clear();
	return job;
}

QString safeFileName(QString text)
{
	for (QChar &ch : text) {
		if (QStringLiteral("\\/:*?\"<>|").contains(ch) || ch.unicode() < 32) ch = QLatin1Char('_');
	}
	text = text.trimmed();
	if (text.isEmpty()) text = QStringLiteral("recording");
	return text.left(100);
}

// Each output has a different clock, even when several outputs encode the same
// OBS video mix. PTS is taken from packets headed to that output's muxer, not
// from QElapsedTimer or the GUI's start/stop timestamps.
struct MarkerOutput {
	obs_output_t *output = nullptr; // Borrowed; the owning Run/native output outlives this object.
	QString filePath;
	QString jobId;
	QString name;
	QJsonArray markers;
	std::mutex packetMutex;
	int64_t firstPts = std::numeric_limits<int64_t>::max();
	int64_t lastPts = std::numeric_limits<int64_t>::min();
	uint64_t lastCaptureNs = 0;
	int32_t timebaseNum = 1;
	int32_t timebaseDen = 1;
	uint32_t fpsNum = 60;
	uint32_t fpsDen = 1;
	bool wantCsv = false;
	bool subscribed = false;
	struct Pending {
		qsizetype index;
		uint64_t requestedNs;
		qint64 actualFrame = 0;
		bool resolved = false;
	};
	std::vector<Pending> pending;

	static void packet(obs_output_t *, encoder_packet *pkt, encoder_packet_time *time, void *param)
	{
		if (!pkt || pkt->type != OBS_ENCODER_VIDEO || pkt->timebase_den <= 0 || pkt->timebase_num <= 0)
			return;
		auto *self = static_cast<MarkerOutput *>(param);
		std::lock_guard<std::mutex> lock(self->packetMutex);
		self->firstPts = std::min(self->firstPts, pkt->pts);
		if (pkt->pts >= self->lastPts) {
			self->lastPts = pkt->pts;
			self->lastCaptureNs = time ? time->cts : 0;
			self->timebaseNum = pkt->timebase_num;
			self->timebaseDen = pkt->timebase_den;
		}
		if (time && time->cts) {
			const double frame = double(pkt->pts - self->firstPts) * pkt->timebase_num /
				pkt->timebase_den * self->fpsNum / self->fpsDen;
			for (auto &request : self->pending) {
				if (!request.resolved && time->cts >= request.requestedNs) {
					request.actualFrame = std::max<qint64>(0, static_cast<qint64>(std::llround(frame)));
					request.resolved = true;
				}
			}
		}
	}

	MarkerOutput(obs_output_t *out, const RecordingJob &job, const QString &path)
		: output(out), filePath(path), jobId(job.id), name(job.name)
	{
		if (auto *encoder = obs_output_get_video_encoder(output)) {
			if (auto *video = obs_encoder_video(encoder)) {
				if (const auto *info = video_output_get_info(video)) {
					fpsNum = info->fps_num ? info->fps_num : 60;
					fpsDen = (info->fps_den ? info->fps_den : 1) *
						std::max(1u, obs_encoder_get_frame_rate_divisor(encoder));
				}
			}
		}
		obs_output_add_packet_callback(output, packet, this);
		subscribed = true;
	}

	~MarkerOutput()
	{
		// remove waits for an in-flight packet callback; the clock then cannot
		// be accessed from the encoding thread while its storage is destroyed.
		if (subscribed) obs_output_remove_packet_callback(output, packet, this);
		reconcile();
		finalize();
	}

	bool running() const { return output && obs_output_active(output) && !obs_output_paused(output); }

	bool writeJson() const
	{
		if (markers.isEmpty()) return true; // Never create empty sidecars.
		const QByteArray bytes = QJsonDocument(markers).toJson(QJsonDocument::Indented);
		QSaveFile file(filePath + QStringLiteral(".markers.json"));
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
	}

	bool writeCsv() const
	{
		if (markers.isEmpty() || !wantCsv) return true;
		QByteArray bytes("\xef\xbb\xbfpos,comment,type\n");
		for (const auto &value : markers) {
			const auto obj = value.toObject();
			QString comment = obj.value(QStringLiteral("comment")).toString();
			comment.replace(QLatin1Char('"'), QStringLiteral("\"\""));
			if (!comment.isEmpty() && QStringLiteral("=+-@").contains(comment.at(0)))
				comment.prepend(QLatin1Char('\''));
			bytes += QStringLiteral("%1,\"%2\",%3\n")
				.arg(obj.value(QStringLiteral("pos")).toVariant().toLongLong())
				.arg(comment).arg(obj.value(QStringLiteral("type")).toInt()).toUtf8();
		}
		QSaveFile file(filePath + QStringLiteral(".markers.csv"));
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
	}

	void finalize() const
	{
		if (!writeJson() || !writeCsv())
			writeDiagnostic(QStringLiteral("markers"), QStringLiteral("Cannot finalize marker sidecar for %1").arg(filePath));
	}

	void reconcile()
	{
		std::vector<std::pair<qsizetype, qint64>> completed;
		{
			std::lock_guard<std::mutex> lock(packetMutex);
			for (const auto &request : pending)
				if (request.resolved) completed.emplace_back(request.index, request.actualFrame);
			pending.erase(std::remove_if(pending.begin(), pending.end(),
				[](const Pending &request) { return request.resolved; }), pending.end());
		}
		if (completed.empty()) return;
		bool changed = false;
		for (const auto &[index, frame] : completed) {
			if (index < 0 || index >= markers.size()) continue;
			auto marker = markers.at(index).toObject();
			if (marker.value(QStringLiteral("pos")).toDouble() == frame) continue;
			marker[QStringLiteral("pos")] = static_cast<double>(frame);
			markers.replace(index, marker);
			changed = true;
		}
		if (changed && (!writeJson() || !writeCsv()))
			writeDiagnostic(QStringLiteral("markers"), QStringLiteral("Cannot reconcile marker PTS for %1").arg(filePath));
	}

	bool append(const QString &label, int type, bool csv, QJsonObject &session)
	{
		if (!running()) return false;
		int64_t first, last;
		uint64_t cts;
		int32_t num, den;
		{
			std::lock_guard<std::mutex> lock(packetMutex);
			first = firstPts;
			last = lastPts;
			cts = lastCaptureNs;
			num = timebaseNum;
			den = timebaseDen;
		}
		if (!fpsNum || !fpsDen) return false;
		// B-frames can arrive in DTS order; PTS is the presentation position.
		const bool hasVideo = first != std::numeric_limits<int64_t>::max() && last >= first;
		const double ptsSeconds = hasVideo ? double(last - first) * num / den : 0.0;
		const double frames = ptsSeconds * fpsNum / fpsDen;
		const uint64_t now = os_gettime_ns();
		// Use the last encoded PTS as a crash-safe provisional position.
		// When the first frame captured after the hotkey arrives, its actual
		// PTS replaces this value. Never extrapolate from wall-clock time.
		const qint64 pos = std::max<qint64>(0, static_cast<qint64>(std::llround(frames)));
		const QJsonObject marker{{QStringLiteral("pos"), static_cast<double>(pos)},
			{QStringLiteral("comment"), label}, {QStringLiteral("type"), type}};
		markers.append(marker);
		wantCsv = wantCsv || csv;
		// Atomic write after EVERY marker: a crash leaves the last committed
		// importable Kdenlive JSON intact. No separate journal recovery needed.
		if (!writeJson()) {
			markers.removeLast();
			writeDiagnostic(QStringLiteral("markers"), QStringLiteral("Cannot save marker sidecar for %1").arg(filePath));
			return false;
		}
		if (!writeCsv())
			writeDiagnostic(QStringLiteral("markers"), QStringLiteral("Cannot save optional marker CSV for %1").arg(filePath));
		if (cts || !hasVideo) {
			std::lock_guard<std::mutex> lock(packetMutex);
			pending.push_back({markers.size() - 1, now});
		}
		session = QJsonObject{{QStringLiteral("jobId"), jobId}, {QStringLiteral("name"), name},
			{QStringLiteral("file"), filePath.left(1024)}, {QStringLiteral("frame"), static_cast<double>(pos)},
			{QStringLiteral("elapsedMs"), double(pos) * 1000 * fpsDen / fpsNum}};
		return true;
	}
};

struct Run {
	RecordingJob job;
	OBSOutputAutoRelease output;
	OBSEncoderAutoRelease videoEncoder;
	std::array<OBSEncoderAutoRelease, 6> audioEncoders;
	OBSSource source;
	obs_view_t *view = nullptr;
	QString outputPath;
	bool started = false;
	bool stopping = false;
	QElapsedTimer created;
	std::unique_ptr<MarkerOutput> marker;

	~Run()
	{
		marker.reset();
		if (output && obs_output_active(output)) obs_output_force_stop(output);
		output = nullptr;
		videoEncoder = nullptr;
		for (auto &encoder : audioEncoders) encoder = nullptr;
		if (view) {
			obs_view_set_source(view, 0, nullptr);
			obs_view_remove(view);
			obs_view_destroy(view);
		}
	}
};

class RecordingEngine final : public QObject {
public:
	explicit RecordingEngine(OBSBasic *owner) : QObject(owner), main(owner)
	{
		setObjectName(QStringLiteral("xinkxRecordingEngine"));
		auto *timer = new QTimer(this);
		timer->setInterval(300);
		connect(timer, &QTimer::timeout, this, [this] { reap(); });
		timer->start();
	}

	bool active() const
	{
		if (main && main->RecordingActive()) return true;
		return std::any_of(runs.begin(), runs.end(), [](const auto &run) {
			return run->output && obs_output_active(run->output);
		});
	}

	void toggle()
	{
		if (active()) {
			stop();
			return;
		}
		const auto jobs = loadRecordingJobs();
		if (std::none_of(jobs.cbegin(), jobs.cend(), [](const auto &job) { return job.enabled; })) {
			QMessageBox::information(main, QStringLiteral("Запись XINKX"),
				QStringLiteral("Нет включённых заданий. Настройте записи через стрелку рядом с кнопкой."));
			return;
		}
		QStringList errors;
		bool nativeRequested = false;
		for (const auto &job : jobs) {
			if (!job.enabled) continue;
			if (job.kind == QStringLiteral("obs")) {
				nativeRequested = true;
				continue;
			}
			if (!startJob(job, errors)) continue;
		}
		if (nativeRequested && main && !main->RecordingActive()) main->StartRecording();
		updateButton();
		if (!errors.isEmpty())
			QMessageBox::warning(main, QStringLiteral("Не удалось запустить часть записей"), errors.join(QLatin1Char('\n')));
	}

	void stop()
	{
		for (auto &run : runs) {
			if (run->output && obs_output_active(run->output) && !run->stopping) {
				run->stopping = true;
				obs_output_stop(run->output);
			}
		}
		if (main && main->RecordingActive()) main->StopRecording();
		updateButton();
	}

	bool jobActive(const QString &id) const
	{
		for (const auto &job : loadRecordingJobs())
			if (job.id == id && job.kind == QStringLiteral("obs")) return main && main->RecordingActive();
		return std::any_of(runs.begin(), runs.end(), [&id](const auto &run) {
			return run->job.id == id && run->output && obs_output_active(run->output);
		});
	}

	bool markedRecordingActive() const
	{
		if (nativeMarker && nativeMarker->running()) return true;
		return std::any_of(runs.begin(), runs.end(), [](const auto &run) {
			return run->marker && run->marker->running() && !run->stopping;
		});
	}

	QJsonArray captureMarker(const QString &label, int type, bool alsoCsv)
	{
		QJsonArray sessions;
		for (const auto &run : runs) {
			if (!run->marker || run->stopping) continue;
			QJsonObject item;
			if (run->marker->append(label, type, alsoCsv, item)) sessions.append(item);
		}
		if (nativeMarker) {
			QJsonObject item;
			if (nativeMarker->append(label, type, alsoCsv, item)) sessions.append(item);
		}
		return sessions;
	}

	void nativeStarted(obs_output_t *output, const QString &path)
	{
		nativeStopped();
		if (!output || path.isEmpty()) return;
		for (const auto &job : loadRecordingJobs()) {
			if (job.kind != QStringLiteral("obs") || !job.recordMarkers) continue;
			nativeOutput = obs_output_get_ref(output);
			nativeMarker = std::make_unique<MarkerOutput>(nativeOutput, job, path);
			break;
		}
	}

	void nativeStopped()
	{
		nativeMarker.reset();
		nativeOutput = nullptr;
	}

	QJsonArray sessions() const
	{
		QJsonArray result;
		for (const auto &run : runs) {
			if (!run->started || !run->output || !obs_output_active(run->output) ||
			    !run->created.isValid()) continue;
			result.append(QJsonObject{
				{QStringLiteral("jobId"), run->job.id},
				{QStringLiteral("name"), run->job.name},
				{QStringLiteral("file"), run->outputPath.left(1024)},
				{QStringLiteral("elapsedMs"), static_cast<double>(run->created.elapsed())}});
		}
		return result;
	}

	void toggleJob(const RecordingJob &job)
	{
		if (job.kind == QStringLiteral("obs")) {
			if (main->RecordingActive()) main->StopRecording();
			else main->StartRecording();
		} else {
			auto it = std::find_if(runs.begin(), runs.end(), [&job](const auto &run) {
				return run->job.id == job.id;
			});
			if (it != runs.end()) {
				if ((*it)->output && obs_output_active((*it)->output) && !(*it)->stopping) {
					(*it)->stopping = true;
					obs_output_stop((*it)->output);
				}
			} else {
				QStringList errors;
				startJob(job, errors);
				if (!errors.isEmpty()) QMessageBox::warning(main, QStringLiteral("Запись XINKX"), errors.join(QLatin1Char('\n')));
			}
		}
		updateButton();
	}

	void shutdown()
	{
		nativeStopped();
		for (auto &run : runs) {
			if (!run->output || !obs_output_active(run->output)) continue;
			obs_output_force_stop(run->output);
			QElapsedTimer clock;
			clock.start();
			while (obs_output_active(run->output) && clock.elapsed() < 2000)
				QThread::msleep(10);
		}
		runs.clear();
		updateButton();
	}

	void updateButton()
	{
		if (!main) return;
		if (auto *controls = main->findChild<OBSBasicControls *>())
			controls->SetXinkxRecording(active());
	}

private:
	QPointer<OBSBasic> main;
	std::vector<std::unique_ptr<Run>> runs;
	OBSOutputAutoRelease nativeOutput;
	std::unique_ptr<MarkerOutput> nativeMarker;
	QString currentSession;

	void beginSessionIfNeeded()
	{
		// A new batch after all previous outputs stopped must not reuse its folder,
		// even if the 300 ms reaper has not deleted the old Run objects yet.
		const bool anyRunning = std::any_of(runs.begin(), runs.end(), [](const auto &run) {
			return run->output && !run->stopping &&
				(obs_output_active(run->output) ||
				 (run->created.isValid() && run->created.elapsed() < 1000));
		});
		if (!anyRunning) currentSession.clear();
		if (currentSession.isEmpty()) {
			// One date directory per day, then one time directory per recording batch.
			// Concurrent jobs share the same path even if their encoders start later.
			currentSession = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd/HH-mm-ss-zzz"));
		}
	}

	bool startJob(const RecordingJob &job, QStringList &errors)
	{
		beginSessionIfNeeded();
		auto run = std::make_unique<Run>();
		run->job = job;
		obs_video_info videoInfo{};
		if (!obs_get_video_info(&videoInfo)) {
			errors << QStringLiteral("%1: видеоподсистема OBS недоступна").arg(job.name);
			return false;
		}
		video_t *mix = nullptr;
		if (job.kind == QStringLiteral("portrait")) {
			mix = PortraitCanvas::instance().video();
		} else {
			run->source = obs_get_source_by_uuid(job.sourceUuid.toUtf8().constData());
			if (!run->source || !(obs_source_get_output_flags(run->source) & OBS_SOURCE_VIDEO)) {
				errors << QStringLiteral("%1: выбранный источник отсутствует или не содержит видео").arg(job.name);
				return false;
			}
			const uint32_t nativeWidth = obs_source_get_width(run->source);
			const uint32_t nativeHeight = obs_source_get_height(run->source);
			// Async sources may report 0 until their first frame. Preserve a
			// usable mix and allow OBS to render their frames once available.
			videoInfo.base_width = nativeWidth ? nativeWidth : videoInfo.base_width;
			videoInfo.base_height = nativeHeight ? nativeHeight : videoInfo.base_height;
			videoInfo.output_width = videoInfo.base_width;
			videoInfo.output_height = videoInfo.base_height;
			if (!job.inherit && job.width > 0 && job.height > 0) {
				videoInfo.output_width = uint32_t(job.width);
				videoInfo.output_height = uint32_t(job.height);
			}
			// All OBS auxiliary views are driven by the main video thread. Do
			// not change the view mix FPS: an independent 30 FPS video_t next
			// to the 60 FPS render clock produces 30 encoded frames each second
			// with 60 FPS timestamps (half-length video, full-length audio).
			// The requested FPS is applied as an encoder frame-rate divisor below.
			run->view = obs_view_create();
			if (!run->view) {
				errors << QStringLiteral("%1: не удалось создать OBS view").arg(job.name);
				return false;
			}
			obs_view_set_source(run->view, 0, run->source);
			mix = obs_view_add2(run->view, &videoInfo);
		}
		if (!mix) {
			errors << QStringLiteral("%1: отдельный видеомикшер недоступен").arg(job.name);
			return false;
		}
		const auto id = job.id.toUtf8();
		OBSDataAutoRelease vSettings;
		QString encoderId = job.encoder;
		if (job.inherit) {
			OBSOutputAutoRelease stock = obs_frontend_get_recording_output();
			obs_encoder_t *stockEncoder = stock ? obs_output_get_video_encoder(stock) : nullptr;
			if (stockEncoder) {
				encoderId = QString::fromUtf8(obs_encoder_get_id(stockEncoder));
				vSettings = obs_encoder_get_settings(stockEncoder);
			}
		}
		if (encoderId.isEmpty() || !obs_get_encoder_codec(encoderId.toUtf8().constData())) {
			encoderId = obs_get_encoder_codec("obs_nvenc_h264_tex") ? QStringLiteral("obs_nvenc_h264_tex") :
				QStringLiteral("obs_x264");
		}
		if (!vSettings) {
			vSettings = obs_data_create();
			const bool x264 = encoderId == QStringLiteral("obs_x264");
			const bool cbr = job.rateControl == QStringLiteral("cbr");
			obs_data_set_string(vSettings, "rate_control", cbr ? "CBR" : (x264 ? "CRF" : "CQP"));
			if (cbr) obs_data_set_int(vSettings, "bitrate", job.bitrate);
			else obs_data_set_int(vSettings, x264 ? "crf" : "cqp", std::clamp(job.quality, 12, 35));
			if (encoderId.startsWith(QStringLiteral("obs_nvenc_")))
				obs_data_set_string(vSettings, "preset", job.preset.toUtf8().constData());
			obs_data_set_int(vSettings, "keyint_sec", 2);
		}
		run->videoEncoder = obs_video_encoder_create(encoderId.toUtf8().constData(),
			(QByteArray("xinkx_rec_video_") + id).constData(), vSettings, nullptr);
		if (!run->videoEncoder) {
			errors << QStringLiteral("%1: видеокодировщик %2 недоступен").arg(job.name, encoderId);
			return false;
		}
		obs_encoder_set_video(run->videoEncoder, mix);
		if (!job.inherit && job.fps > 0) {
			const auto *mixInfo = video_output_get_info(mix);
			if (!mixInfo || !mixInfo->fps_num || !mixInfo->fps_den) {
				errors << QStringLiteral("%1: нельзя определить FPS видеомикшера").arg(job.name);
				return false;
			}
			const double sourceFps = double(mixInfo->fps_num) / double(mixInfo->fps_den);
			const int divisor = std::max(1, int(std::lround(sourceFps / job.fps)));
			const double actualFps = sourceFps / divisor;
			if (std::abs(actualFps - job.fps) > 0.1) {
				errors << QStringLiteral("%1: %2 FPS недоступно при частоте OBS %3 FPS; "
					"выберите делитель основной частоты или «Как в OBS»")
					.arg(job.name).arg(job.fps).arg(sourceFps, 0, 'f', 2);
				return false;
			}
			if (divisor > 1 && !obs_encoder_set_frame_rate_divisor(run->videoEncoder, uint32_t(divisor))) {
				errors << QStringLiteral("%1: не удалось установить FPS кодировщика").arg(job.name);
				return false;
			}
		}
		if (job.kind == QStringLiteral("portrait") && !job.inherit && job.width > 0 && job.height > 0)
			obs_encoder_set_scaled_size(run->videoEncoder, uint32_t(job.width), uint32_t(job.height));
		// Mixes are configured in OBS Advanced Audio Properties. A clean video
		// source does not implicitly isolate its game/Discord/microphone audio.
		// ffmpeg_muxer requires contiguous file track indices (0..N-1), even
		// when the selected OBS mixer indices are non-contiguous (e.g. 2,3,4).
		int fileTrack = 0;
		for (int mixIndex = 0; mixIndex < 6; ++mixIndex) {
			if (!(job.audioTracks & (1 << mixIndex))) continue;
			if (!obs_get_audio()) {
				errors << QStringLiteral("%1: аудиоподсистема OBS недоступна").arg(job.name);
				return false;
			}
			OBSDataAutoRelease aSettings = obs_data_create();
			obs_data_set_int(aSettings, "bitrate", 192);
			const QByteArray encoderName = QByteArray("xinkx_rec_audio_") + id + '_' +
				QByteArray::number(mixIndex + 1);
			auto &audioEncoder = run->audioEncoders[size_t(fileTrack)];
			audioEncoder = obs_audio_encoder_create("ffmpeg_aac", encoderName.constData(),
				aSettings, size_t(mixIndex), nullptr);
			if (!audioEncoder) {
				errors << QStringLiteral("%1: AAC-кодировщик дорожки OBS %2 недоступен")
					.arg(job.name).arg(mixIndex + 1);
				return false;
			}
			obs_encoder_set_audio(audioEncoder, obs_get_audio());
			++fileTrack;
		}
		const QString root = job.folder.isEmpty() ? defaultFolder() : job.folder;
		const QString directory = job.sessionFolder ? QDir(root).filePath(currentSession) : root;
		if (!QDir().mkpath(directory)) {
			errors << QStringLiteral("%1: невозможно создать папку %2").arg(job.name, directory);
			return false;
		}
		const QString ext = job.format == QStringLiteral("mp4") ? QStringLiteral("mp4") : QStringLiteral("mkv");
		const QString basename = safeFileName(job.name) + QLatin1Char('_') +
			QDateTime::currentDateTime().toString(QStringLiteral("HH-mm-ss-zzz"));
		QString filename = basename + QLatin1Char('.') + ext;
		// Never overwrite an earlier clip when a job is restarted in the same session.
		for (int suffix = 2; QFileInfo::exists(QDir(directory).filePath(filename)) ||
			QFileInfo::exists(QDir(directory).filePath(filename + QStringLiteral(".markers.json"))); ++suffix)
			filename = basename + QLatin1Char('_') + QString::number(suffix) + QLatin1Char('.') + ext;
		OBSDataAutoRelease outputSettings = obs_data_create();
		run->outputPath = QDir(directory).filePath(filename);
		obs_data_set_string(outputSettings, "path", run->outputPath.toUtf8().constData());
		// The stock ffmpeg_muxer has OBS_OUTPUT_AUDIO set and cannot start
		// with zero audio encoders. Use the video-only variant for silent jobs.
		const char *outputId = fileTrack ? "ffmpeg_muxer" : "xinkx_ffmpeg_video_only_muxer";
		run->output = obs_output_create(outputId, (QByteArray("xinkx_rec_") + id).constData(), outputSettings, nullptr);
		if (!run->output) {
			errors << QStringLiteral("%1: файловый выход %2 недоступен").arg(job.name, QString::fromUtf8(outputId));
			return false;
		}
		obs_output_set_video_encoder(run->output, run->videoEncoder);
		for (int fileIndex = 0; fileIndex < fileTrack; ++fileIndex)
			obs_output_set_audio_encoder(run->output, run->audioEncoders[size_t(fileIndex)],
				size_t(fileIndex));
		if (job.recordMarkers)
			run->marker = std::make_unique<MarkerOutput>(run->output, job, run->outputPath);
		if (!obs_output_start(run->output)) {
			errors << QStringLiteral("%1: %2").arg(job.name,
				QString::fromUtf8(obs_output_get_last_error(run->output) ? obs_output_get_last_error(run->output) : "Ошибка запуска"));
			return false;
		}
		run->started = true;
		run->created.start();
		writeDiagnostic(QStringLiteral("recording"), QStringLiteral("Started %1 (%2)").arg(job.name, job.kind));
		runs.emplace_back(std::move(run));
		return true;
	}

	void reap()
	{
		if (nativeMarker) nativeMarker->reconcile();
		const auto before = runs.size();
		for (auto it = runs.begin(); it != runs.end();) {
			if ((*it)->marker) (*it)->marker->reconcile();
			if (!(*it)->output || (!obs_output_active((*it)->output) && (*it)->created.isValid() &&
			    (*it)->created.elapsed() > 2000)) {
				if ((*it)->started) writeDiagnostic(QStringLiteral("recording"),
					QStringLiteral("Stopped %1").arg((*it)->job.name));
				it = runs.erase(it);
			} else ++it;
		}
		if (before != runs.size()) updateButton();
	}
};

RecordingEngine *engine(OBSBasic *main)
{
	if (!main) return nullptr;
	auto *existing = static_cast<RecordingEngine *>(main->findChild<QObject *>(
		QStringLiteral("xinkxRecordingEngine"), Qt::FindDirectChildrenOnly));
	return existing ? existing : new RecordingEngine(main);
}

} // namespace

QVector<RecordingJob> loadRecordingJobs()
{
	QFile file(configFile());
	if (!file.exists()) return {fallbackJob()};
	if (!file.open(QIODevice::ReadOnly)) return {};
	QJsonParseError error{};
	const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) return {};
	QVector<RecordingJob> jobs;
	for (const auto &value : doc.object().value(QStringLiteral("jobs")).toArray()) {
		if (!value.isObject()) continue;
		const auto obj = value.toObject();
		RecordingJob job;
		job.id = obj.value(QStringLiteral("id")).toString();
		job.name = obj.value(QStringLiteral("name")).toString();
		job.kind = obj.value(QStringLiteral("kind")).toString();
		job.sourceUuid = obj.value(QStringLiteral("source")).toString();
		job.enabled = obj.value(QStringLiteral("enabled")).toBool(true);
		job.recordMarkers = obj.value(QStringLiteral("recordMarkers")).toBool(false);
		job.inherit = obj.value(QStringLiteral("inherit")).toBool(true); // Preserve old saved jobs.
		job.encoder = obj.value(QStringLiteral("encoder")).toString(QStringLiteral("obs_nvenc_h264_tex"));
		job.width = obj.value(QStringLiteral("width")).toInt();
		job.height = obj.value(QStringLiteral("height")).toInt();
		job.fps = obj.value(QStringLiteral("fps")).toInt();
		job.quality = obj.value(QStringLiteral("quality")).toInt(20);
		job.preset = obj.value(QStringLiteral("preset")).toString(QStringLiteral("p4"));
		job.rateControl = obj.value(QStringLiteral("rateControl")).toString(QStringLiteral("quality"));
		job.bitrate = obj.value(QStringLiteral("bitrate")).toInt(12000);
		// Existing single-track profiles keep their original audio (including -1 = mute).
		// New profiles store a 6-bit mask instead of a single mixer number.
		if (obj.contains(QStringLiteral("audioTracks"))) {
			job.audioTracks = obj.value(QStringLiteral("audioTracks")).toInt(1) & 0x3f;
		} else {
			const int oldMix = obj.value(QStringLiteral("audioMix")).toInt(0);
			job.audioTracks = oldMix >= 0 && oldMix < 6 ? (1 << oldMix) : 0;
		}
		job.folder = obj.value(QStringLiteral("folder")).toString();
		job.sessionFolder = obj.value(QStringLiteral("sessionFolder")).toBool(true);
		job.format = obj.value(QStringLiteral("format")).toString(QStringLiteral("mkv"));
		if (job.id.isEmpty() || job.name.trimmed().isEmpty()) continue;
		if (job.kind != QStringLiteral("obs") && job.kind != QStringLiteral("portrait") &&
		    job.kind != QStringLiteral("scene") && job.kind != QStringLiteral("source")) continue;
		jobs << job;
	}
	return jobs;
}

bool saveRecordingJobs(const QVector<RecordingJob> &jobs, QString *error)
{
	QJsonArray array;
	for (const auto &job : jobs) {
		QJsonObject obj;
		obj[QStringLiteral("id")] = job.id;
		obj[QStringLiteral("name")] = job.name;
		obj[QStringLiteral("kind")] = job.kind;
		obj[QStringLiteral("source")] = job.sourceUuid;
		obj[QStringLiteral("enabled")] = job.enabled;
		obj[QStringLiteral("recordMarkers")] = job.recordMarkers;
		obj[QStringLiteral("inherit")] = job.inherit;
		obj[QStringLiteral("encoder")] = job.encoder;
		obj[QStringLiteral("width")] = job.width;
		obj[QStringLiteral("height")] = job.height;
		obj[QStringLiteral("fps")] = job.fps;
		obj[QStringLiteral("quality")] = job.quality;
		obj[QStringLiteral("preset")] = job.preset;
		obj[QStringLiteral("rateControl")] = job.rateControl;
		obj[QStringLiteral("bitrate")] = job.bitrate;
		obj[QStringLiteral("audioTracks")] = job.audioTracks & 0x3f;
		obj[QStringLiteral("folder")] = job.folder;
		obj[QStringLiteral("sessionFolder")] = job.sessionFolder;
		obj[QStringLiteral("format")] = job.format;
		array.append(obj);
	}
	const QString path = configFile();
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		if (error) *error = QStringLiteral("Не удалось создать папку конфигурации");
		return false;
	}
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		if (error) *error = file.errorString();
		return false;
	}
	QJsonObject document;
	document[QStringLiteral("version")] = 1;
	document[QStringLiteral("jobs")] = array;
	if (file.write(QJsonDocument(document).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
		if (error) *error = file.errorString();
		return false;
	}
	return true;
}

bool recordingJobsNativeOnly()
{
	const auto jobs = loadRecordingJobs();
	int enabled = 0;
	bool native = false;
	for (const auto &job : jobs) {
		if (!job.enabled) continue;
		++enabled;
		native = job.kind == QStringLiteral("obs");
	}
	return enabled == 1 && native;
}

bool recordingJobActive(OBSBasic *main, const QString &id)
{
	if (!main) return false;
	if (auto *instance = static_cast<RecordingEngine *>(main->findChild<QObject *>(
		QStringLiteral("xinkxRecordingEngine"), Qt::FindDirectChildrenOnly))) return instance->jobActive(id);
	for (const auto &job : loadRecordingJobs())
		if (job.id == id && job.kind == QStringLiteral("obs")) return main->RecordingActive();
	return false;
}

QJsonArray activeRecordingSessions(OBSBasic *main)
{
	QJsonArray sessions;
	if (!main) return sessions;
	if (auto *instance = static_cast<RecordingEngine *>(main->findChild<QObject *>(
		QStringLiteral("xinkxRecordingEngine"), Qt::FindDirectChildrenOnly)))
		sessions = instance->sessions();
	if (main->RecordingActive()) {
		sessions.append(QJsonObject{
			{QStringLiteral("jobId"), QStringLiteral("obs-primary")},
			{QStringLiteral("name"), QStringLiteral("Основная запись OBS")},
			{QStringLiteral("file"), QString()},
			{QStringLiteral("elapsedMs"), -1}});
	}
	return sessions;
}

QJsonArray captureRecordingMarker(OBSBasic *main, const QString &label, int type, bool alsoCsv)
{
	if (!main) return {};
	return engine(main)->captureMarker(label, type, alsoCsv);
}

bool hasMarkedRecording(OBSBasic *main)
{
	if (!main) return false;
	auto *instance = static_cast<RecordingEngine *>(main->findChild<QObject *>(
		QStringLiteral("xinkxRecordingEngine"), Qt::FindDirectChildrenOnly));
	return instance && instance->markedRecordingActive();
}

void nativeRecordingStarted(OBSBasic *main, obs_output_t *output, const QString &filePath)
{
	if (auto *instance = engine(main)) instance->nativeStarted(output, filePath);
}

void nativeRecordingStopped(OBSBasic *main)
{
	if (!main) return;
	if (auto *instance = static_cast<RecordingEngine *>(main->findChild<QObject *>(
		QStringLiteral("xinkxRecordingEngine"), Qt::FindDirectChildrenOnly))) instance->nativeStopped();
}

void toggleRecordingJob(OBSBasic *main, const QString &id)
{
	if (!main) return;
	const auto jobs = loadRecordingJobs();
	for (const auto &job : jobs) {
		if (job.id == id) {
			engine(main)->toggleJob(job);
			return;
		}
	}
}

void toggleRecordingJobs(OBSBasic *main)
{
	if (auto *instance = engine(main)) instance->toggle();
}

void stopRecordingJobs(OBSBasic *main)
{
	if (!main) return;
	if (auto *instance = static_cast<RecordingEngine *>(main->findChild<QObject *>(
		QStringLiteral("xinkxRecordingEngine"), Qt::FindDirectChildrenOnly))) instance->shutdown();
}

void refreshRecordingButton(OBSBasic *main)
{
	if (auto *instance = engine(main)) instance->updateButton();
}

} // namespace xinkx
