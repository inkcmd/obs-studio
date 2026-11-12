#pragma once

#include <QString>
#include <QVector>

class OBSBasic;

namespace xinkx {

struct RecordingJob {
	QString id;
	QString name;
	// obs = native OBS recording; portrait = current portrait mix;
	// scene/source = source UUID recorded through an independent OBS view.
	QString kind = QStringLiteral("source");
	QString sourceUuid;
	bool enabled = true;
	bool inherit = true;
	QString encoder = QStringLiteral("obs_nvenc_h264_tex");
	int width = 0;
	int height = 0;
	int fps = 0;
	int quality = 20;
	QString rateControl = QStringLiteral("quality");
	int bitrate = 12000;
	int audioMix = 0; // -1 means no audio; otherwise OBS mixer 1-6
	QString folder;
	QString format = QStringLiteral("mkv");
};

QVector<RecordingJob> loadRecordingJobs();
bool saveRecordingJobs(const QVector<RecordingJob> &jobs, QString *error = nullptr);
void showRecordingSettings(OBSBasic *main);
void toggleRecordingJobs(OBSBasic *main);
void stopRecordingJobs(OBSBasic *main);
void refreshRecordingButton(OBSBasic *main);
bool recordingJobsNativeOnly();
bool recordingJobActive(OBSBasic *main, const QString &id);
void toggleRecordingJob(OBSBasic *main, const QString &id);

} // namespace xinkx
