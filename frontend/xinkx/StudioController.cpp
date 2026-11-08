#include "StudioController.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>
#include <obs.h>

#include <QTimer>

#include <algorithm>
#include <cmath>

namespace xinkx {

namespace {

struct SourceInfo {
	QString name;
	SelectedTransform transform;
	int count = 0;
};

bool collectSelectedSource(obs_scene_t *, obs_sceneitem_t *item, void *userData)
{
	if (!obs_sceneitem_selected(item)) {
		return true;
	}

	auto *info = static_cast<SourceInfo *>(userData);
	++info->count;
	if (info->count > 1) {
		return true;
	}

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source) {
		return true;
	}

	const char *sourceName = obs_source_get_name(source);
	info->name = QString::fromUtf8(sourceName ? sourceName : "");

	vec2 position{};
	vec2 scale{};
	obs_sceneitem_get_pos(item, &position);
	obs_sceneitem_get_scale(item, &scale);

	info->transform.itemId = obs_sceneitem_get_id(item);
	info->transform.locked = obs_sceneitem_locked(item);
	info->transform.x = position.x;
	info->transform.y = position.y;
	info->transform.rotation = obs_sceneitem_get_rot(item);
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_crop(item, &crop);
	info->transform.cropLeft = crop.left;
	info->transform.cropTop = crop.top;
	info->transform.cropRight = crop.right;
	info->transform.cropBottom = crop.bottom;

	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		vec2 bounds{};
		obs_sceneitem_get_bounds(item, &bounds);
		info->transform.width = bounds.x;
		info->transform.height = bounds.y;
		info->transform.resizable = true;
	} else {
		const uint32_t sourceWidth = obs_source_get_width(source);
		const uint32_t sourceHeight = obs_source_get_height(source);
		if (sourceWidth > 0 && sourceHeight > 0) {
			const double contentWidth = std::max(1, static_cast<int>(sourceWidth) - crop.left - crop.right);
			const double contentHeight = std::max(1, static_cast<int>(sourceHeight) - crop.top - crop.bottom);
			info->transform.width = contentWidth * std::abs(scale.x);
			info->transform.height = contentHeight * std::abs(scale.y);
			info->transform.resizable = true;
		}
	}
	return true;
}

struct ItemLookup {
	qint64 expectedId;
	obs_sceneitem_t *item = nullptr;
};

bool findSelectedItem(obs_scene_t *, obs_sceneitem_t *item, void *userData)
{
	auto *lookup = static_cast<ItemLookup *>(userData);
	if (obs_sceneitem_selected(item) && obs_sceneitem_get_id(item) == lookup->expectedId) {
		lookup->item = item;
		return false;
	}
	return true;
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
	if (selected.count > 1) {
		selected.name = QStringLiteral("Multiple sources selected");
		selected.transform = {};
	}
	const bool transformChanged = firstRefresh || !(selected.transform == previousTransform);
	if (!firstRefresh && sceneName == previousScene && selected.name == previousSource &&
	    !transformChanged && streaming == previousStreaming && recording == previousRecording) {
		// Refresh the inspector after the user finishes typing, even if OBS state is unchanged.
		emit selectedTransformChanged(selected.transform);
		return;
	}

	previousScene = sceneName;
	previousSource = selected.name;
	previousTransform = selected.transform;
	previousStreaming = streaming;
	previousRecording = recording;
	firstRefresh = false;
	emit stateChanged(sceneName, selected.name, streaming, recording);
	emit selectedTransformChanged(selected.transform);
}

bool StudioController::applySelectedTransform(qint64 itemId, double x, double y, double width, double height)
{
	if (itemId < 0 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
	    !std::isfinite(height)) {
		return false;
	}

	OBSBasic *mainWindow = OBSBasic::Get();
	if (!mainWindow) {
		return false;
	}
	OBSScene scene = mainWindow->GetCurrentScene();
	if (!scene) {
		return false;
	}

	ItemLookup lookup{itemId};
	obs_scene_enum_items(scene, findSelectedItem, &lookup);
	obs_sceneitem_t *item = lookup.item;
	if (!item || obs_sceneitem_locked(item)) {
		return false;
	}

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source) {
		return false;
	}

	const bool bounded = obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
	vec2 originalPosition{};
	vec2 originalSize{};
	obs_sceneitem_get_pos(item, &originalPosition);
	if (bounded) {
		obs_sceneitem_get_bounds(item, &originalSize);
	} else {
		obs_sceneitem_get_scale(item, &originalSize);
	}

	if (width <= 0 || height <= 0) {
		return false;
	}

	vec2 newSize{};
	if (bounded) {
		newSize = {static_cast<float>(width), static_cast<float>(height)};
	} else {
		obs_sceneitem_crop crop{};
		obs_sceneitem_get_crop(item, &crop);
		const int contentWidth = static_cast<int>(obs_source_get_width(source)) - crop.left - crop.right;
		const int contentHeight = static_cast<int>(obs_source_get_height(source)) - crop.top - crop.bottom;
		if (contentWidth <= 0 || contentHeight <= 0) {
			// Some sources have no intrinsic dimensions; their position is still editable.
			newSize = originalSize;
		} else {
			newSize = {static_cast<float>((originalSize.x < 0 ? -width : width) / contentWidth),
				   static_cast<float>((originalSize.y < 0 ? -height : height) / contentHeight)};
		}
	}

	if (std::abs(originalPosition.x - x) < 0.01 && std::abs(originalPosition.y - y) < 0.01 &&
	    std::abs(originalSize.x - newSize.x) < 0.00001 && std::abs(originalSize.y - newSize.y) < 0.00001) {
		return true;
	}

	OBSData before = OBSBasic::BackupScene(scene);
	const vec2 newPosition{static_cast<float>(x), static_cast<float>(y)};
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_pos(item, &newPosition);
	if (bounded) {
		obs_sceneitem_set_bounds(item, &newSize);
	} else {
		obs_sceneitem_set_scale(item, &newSize);
	}
	obs_sceneitem_defer_update_end(item);
	OBSData after = OBSBasic::BackupScene(scene);
	mainWindow->CreateSceneUndoRedoAction(QStringLiteral("XINKX: Transform %1")
						     .arg(QString::fromUtf8(obs_source_get_name(source))),
						 before, after);
	refresh();
	return true;
}

bool StudioController::applySelectedCropRotation(qint64 itemId, double rotation, int left, int top,
						 int right, int bottom)
{
	if (itemId < 0 || !std::isfinite(rotation) || rotation < -360.0 || rotation > 360.0 ||
	    std::min({left, top, right, bottom}) < 0) {
		return false;
	}
	OBSBasic *mainWindow = OBSBasic::Get();
	if (!mainWindow) {
		return false;
	}
	OBSScene scene = mainWindow->GetCurrentScene();
	if (!scene) {
		return false;
	}
	ItemLookup lookup{itemId};
	obs_scene_enum_items(scene, findSelectedItem, &lookup);
	obs_sceneitem_t *item = lookup.item;
	if (!item || obs_sceneitem_locked(item)) {
		return false;
	}
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source) {
		return false;
	}
	const int sourceWidth = static_cast<int>(obs_source_get_width(source));
	const int sourceHeight = static_cast<int>(obs_source_get_height(source));
	// Cropping a zero-sized/dynamic source is not meaningful. Rotation remains editable.
	if ((left != 0 || right != 0 || top != 0 || bottom != 0) &&
	    (sourceWidth <= left + right || sourceHeight <= top + bottom)) {
		return false;
	}
	obs_sceneitem_crop oldCrop{};
	obs_sceneitem_get_crop(item, &oldCrop);
	if (std::abs(obs_sceneitem_get_rot(item) - rotation) < 0.01 && oldCrop.left == left &&
	    oldCrop.top == top && oldCrop.right == right && oldCrop.bottom == bottom) {
		return true;
	}
	OBSData before = OBSBasic::BackupScene(scene);
	const obs_sceneitem_crop crop{left, top, right, bottom};
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_set_rot(item, static_cast<float>(rotation));
	obs_sceneitem_defer_update_end(item);
	OBSData after = OBSBasic::BackupScene(scene);
	mainWindow->CreateSceneUndoRedoAction(
		QStringLiteral("XINKX: Crop / rotate %1").arg(QString::fromUtf8(obs_source_get_name(source))),
		before, after);
	refresh();
	return true;
}

} // namespace xinkx
