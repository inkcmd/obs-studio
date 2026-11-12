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
#include <cstring>
#include <memory>
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

struct Run {
	RecordingJob job;
	OBSOutputAutoRelease output;
	OBSEncoderAutoRelease videoEncoder;
	OBSEncoderAutoRelease audioEncoder;
	OBSSource source;
	obs_view_t *view = nullptr;
	bool started = false;
	bool stopping = false;
	QElapsedTimer created;

	~Run()
	{
		if (output && obs_output_active(output)) obs_output_force_stop(output);
		output = nullptr;
		videoEncoder = nullptr;
		audioEncoder = nullptr;
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

	bool startJob(const RecordingJob &job, QStringList &errors)
	{
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
			if (!job.inherit && job.fps > 0) {
				videoInfo.fps_num = uint32_t(job.fps);
				videoInfo.fps_den = 1;
			}
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
			else obs_data_set_int(vSettings, x264 ? "crf" : "cqp", job.quality);
			obs_data_set_int(vSettings, "keyint_sec", 2);
		}
		run->videoEncoder = obs_video_encoder_create(encoderId.toUtf8().constData(),
			(QByteArray("xinkx_rec_video_") + id).constData(), vSettings, nullptr);
		if (!run->videoEncoder) {
			errors << QStringLiteral("%1: видеокодировщик %2 недоступен").arg(job.name, encoderId);
			return false;
		}
		obs_encoder_set_video(run->videoEncoder, mix);
		if (job.kind == QStringLiteral("portrait") && !job.inherit && job.width > 0 && job.height > 0)
			obs_encoder_set_scaled_size(run->videoEncoder, uint32_t(job.width), uint32_t(job.height));
		// Each recording uses a different name/encoder; audio comes from the selected OBS mix.
		if (job.audioMix >= 0) {
			OBSDataAutoRelease aSettings = obs_data_create();
			obs_data_set_int(aSettings, "bitrate", 192);
			run->audioEncoder = obs_audio_encoder_create("ffmpeg_aac",
				(QByteArray("xinkx_rec_audio_") + id).constData(), aSettings, size_t(job.audioMix), nullptr);
			if (!run->audioEncoder) {
				errors << QStringLiteral("%1: AAC-кодировщик недоступен").arg(job.name);
				return false;
			}
			obs_encoder_set_audio(run->audioEncoder, obs_get_audio());
		}
		const QString directory = job.folder.isEmpty() ? defaultFolder() : job.folder;
		if (!QDir().mkpath(directory)) {
			errors << QStringLiteral("%1: невозможно создать папку %2").arg(job.name, directory);
			return false;
		}
		const QString ext = job.format == QStringLiteral("mp4") ? QStringLiteral("mp4") : QStringLiteral("mkv");
		const QString filename = safeFileName(job.name) + QLatin1Char('_') +
			QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz")) + QLatin1Char('.') + ext;
		OBSDataAutoRelease outputSettings = obs_data_create();
		obs_data_set_string(outputSettings, "path", QDir(directory).filePath(filename).toUtf8().constData());
		run->output = obs_output_create("ffmpeg_muxer", (QByteArray("xinkx_rec_") + id).constData(), outputSettings, nullptr);
		if (!run->output) {
			errors << QStringLiteral("%1: ffmpeg_muxer недоступен").arg(job.name);
			return false;
		}
		obs_output_set_video_encoder(run->output, run->videoEncoder);
		if (run->audioEncoder) obs_output_set_audio_encoder(run->output, run->audioEncoder, 0);
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
		const auto before = runs.size();
		for (auto it = runs.begin(); it != runs.end();) {
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
		job.inherit = obj.value(QStringLiteral("inherit")).toBool(true);
		job.encoder = obj.value(QStringLiteral("encoder")).toString(QStringLiteral("obs_nvenc_h264_tex"));
		job.width = obj.value(QStringLiteral("width")).toInt();
		job.height = obj.value(QStringLiteral("height")).toInt();
		job.fps = obj.value(QStringLiteral("fps")).toInt();
		job.quality = obj.value(QStringLiteral("quality")).toInt(20);
		job.rateControl = obj.value(QStringLiteral("rateControl")).toString(QStringLiteral("quality"));
		job.bitrate = obj.value(QStringLiteral("bitrate")).toInt(12000);
		job.audioMix = obj.value(QStringLiteral("audioMix")).toInt(0);
		job.folder = obj.value(QStringLiteral("folder")).toString();
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
		obj[QStringLiteral("inherit")] = job.inherit;
		obj[QStringLiteral("encoder")] = job.encoder;
		obj[QStringLiteral("width")] = job.width;
		obj[QStringLiteral("height")] = job.height;
		obj[QStringLiteral("fps")] = job.fps;
		obj[QStringLiteral("quality")] = job.quality;
		obj[QStringLiteral("rateControl")] = job.rateControl;
		obj[QStringLiteral("bitrate")] = job.bitrate;
		obj[QStringLiteral("audioMix")] = job.audioMix;
		obj[QStringLiteral("folder")] = job.folder;
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
