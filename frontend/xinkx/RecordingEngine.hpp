#pragma once

#include <QString>
#include <QJsonArray>
#include <QVector>
#include <obs.h>

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
	// Per-output montage markers. Never enabled implicitly for old profiles.
	bool recordMarkers = false;
	// Existing jobs retain their saved inherit flag; new independent jobs use CQP.
	bool inherit = false;
	QString encoder = QStringLiteral("obs_nvenc_h264_tex");
	int width = 0;
	int height = 0;
	int fps = 0;
	int quality = 27;
	QString preset = QStringLiteral("p4");
	QString rateControl = QStringLiteral("quality");
	int bitrate = 12000;
	// Bitmask of OBS audio mixes 1-6. Bit 0 = OBS mixer 1; zero = silent video.
	// Legacy recording jobs containing "audioMix" are migrated when loaded.
	int audioTracks = 1;
	QString folder;
	bool sessionFolder = true; // Put independent recordings in a dated session folder.
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
// Snapshot of active jobs; used for diagnostics and the marker workspace.
QJsonArray activeRecordingSessions(OBSBasic *main);
// Appends a Kdenlive sidecar marker to every active opted-in output.
// Returns only jobs whose video PTS was successfully recorded.
QJsonArray captureRecordingMarker(OBSBasic *main, const QString &label, int type, bool alsoCsv = false);
bool hasMarkedRecording(OBSBasic *main);
void nativeRecordingStarted(OBSBasic *main, obs_output_t *output, const QString &filePath);
void nativeRecordingStopped(OBSBasic *main);
void toggleRecordingJob(OBSBasic *main, const QString &id);

} // namespace xinkx
