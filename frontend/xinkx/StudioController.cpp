#include "StudioController.hpp"

#include <obs-frontend-api.h>
#include <obs.h>

#include <QTimer>

namespace xinkx {

namespace {

struct SourceInfo {
	QString name;
	QString details;
};

bool collectSelectedSource(obs_scene_t *, obs_sceneitem_t *item, void *userData)
{
	if (!obs_sceneitem_selected(item)) {
		return true;
	}

	auto *info = static_cast<SourceInfo *>(userData);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source) {
		return false;
	}

	const char *sourceName = obs_source_get_name(source);
	info->name = QString::fromUtf8(sourceName ? sourceName : "");

	vec2 position{};
	vec2 scale{};
	obs_sceneitem_get_pos(item, &position);
	obs_sceneitem_get_scale(item, &scale);

	info->details = QStringLiteral("%1 x %2 px\nPosition: %3, %4\nScale: %5 x %6")
				.arg(QString::number(obs_source_get_width(source)))
				.arg(QString::number(obs_source_get_height(source)))
				.arg(position.x, 0, 'f', 0)
				.arg(position.y, 0, 'f', 0)
				.arg(scale.x, 0, 'f', 2)
				.arg(scale.y, 0, 'f', 2);
	return false;
}

} // namespace

StudioController::StudioController(QObject *parent) : QObject(parent)
{
	auto *timer = new QTimer(this);
	timer->setInterval(500);
	connect(timer, &QTimer::timeout, this, [this] { refresh(); });
	timer->start();
	// OBSBasic is created before libobs initialization. Read on the first event-loop tick.
	QTimer::singleShot(0, this, [this] { refresh(); });
}

void StudioController::refresh()
{
	QString sceneName;
	SourceInfo selected;
	// Selection belongs to Preview in Studio Mode; Program can be a different scene.
	obs_source_t *sceneSource = obs_frontend_preview_program_mode_active()
					? obs_frontend_get_current_preview_scene()
					: nullptr;
	if (!sceneSource) {
		sceneSource = obs_frontend_get_current_scene();
	}
	if (sceneSource) {
		const char *name = obs_source_get_name(sceneSource);
		sceneName = QString::fromUtf8(name ? name : "");
		if (obs_scene_t *scene = obs_scene_from_source(sceneSource)) {
			obs_scene_enum_items(scene, collectSelectedSource, &selected);
		}
		obs_source_release(sceneSource);
	}

	const bool streaming = obs_frontend_streaming_active();
	const bool recording = obs_frontend_recording_active();
	if (!firstRefresh && sceneName == previousScene && selected.name == previousSource &&
	    selected.details == previousDetails && streaming == previousStreaming && recording == previousRecording) {
		return;
	}

	previousScene = sceneName;
	previousSource = selected.name;
	previousDetails = selected.details;
	previousStreaming = streaming;
	previousRecording = recording;
	firstRefresh = false;
	emit stateChanged(sceneName, selected.name, selected.details, streaming, recording);
}

} // namespace xinkx
