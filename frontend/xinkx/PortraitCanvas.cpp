#include "PortraitCanvas.hpp"

#include "StreamAccounts.hpp"
#include "StudioDiagnostics.hpp"

#include <components/SourceTree.hpp>
#include <dialogs/OBSBasicSourceSelect.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSBasicPreview.hpp>

#include <obs-frontend-api.h>
#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>

#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QEvent>
#include <QCryptographicHash>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QItemSelectionModel>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QStyle>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace xinkx {
namespace {

constexpr uint32_t portraitWidth = 1080;
constexpr uint32_t portraitHeight = 1920;

QString storeKey(const QString &name)
{
	return QStringLiteral("portrait/layout_%1").arg(QString::fromLatin1(
		QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex()));
}

QString extraKey(const QString &name)
{
	return storeKey(name) + QStringLiteral("_sources");
}

struct Captured {
	OBSSource source;
	QString key;
	QString name;
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	bool visible = true;
};

bool captureItem(obs_scene_t *, obs_sceneitem_t *item, void *context)
{
	auto *found = static_cast<std::vector<Captured> *>(context);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source) return true;
	Captured next;
	next.source = source; // OBSSource adds the strong reference itself.
	next.name = QString::fromUtf8(obs_source_get_name(source));
	next.key = QStringLiteral("%1:%2").arg(obs_sceneitem_get_id(item)).arg(
		QString::fromUtf8(obs_source_get_uuid(source)));
	obs_sceneitem_get_info2(item, &next.info);
	obs_sceneitem_get_crop(item, &next.crop);
	next.visible = obs_sceneitem_visible(item);
	found->push_back(std::move(next));
	return true;
}

bool collectItemRefs(obs_scene_t *, obs_sceneitem_t *item, void *context)
{
	obs_sceneitem_addref(item);
	static_cast<std::vector<obs_sceneitem_t *> *>(context)->push_back(item);
	return true;
}

// Only the display visibility is shared with the OBS render thread.
// The native OBSBasicPreview owns the selection and transforms for BOTH canvases.
struct PortraitPreviewState {
	std::atomic<float> pixelRatio{1.f};
	std::atomic<bool> visible{false};
};
PortraitPreviewState previewState;
std::mutex previewSourceMutex;

// Input bridge for portrait-specific context actions only. No drag/resize
// implementation, no selection geometry, and no separate GPU window.
class PortraitDisplay : public QObject {
public:
	explicit PortraitDisplay(OBSBasicPreview *target, QObject *parent) : QObject(parent), nativePreview(target)
	{
		previewState.pixelRatio.store(float(target->devicePixelRatioF()));
		target->installEventFilter(this);
	}
	~PortraitDisplay() override
	{
		if (nativePreview) nativePreview->removeEventFilter(this);
	}
	std::function<void(float, float, const QPoint &)> contextRequested;
	std::function<void(float, float)> doubleClicked;
	std::function<void()> deleteRequested;
	void resetZoom()
	{
		nativePreview->SetFixedScaling(false);
		nativePreview->ResetScrollingOffset();
		emit nativePreview->DisplayResized();
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (!nativePreview || watched != nativePreview.data() || !previewState.visible.load()) return false;
		previewState.pixelRatio.store(float(nativePreview->devicePixelRatioF()));
		if (event->type() == QEvent::KeyPress && nativePreview->IsPortraitEditing()) {
			auto *key = static_cast<QKeyEvent *>(event);
			if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && deleteRequested) {
				deleteRequested();
				key->accept();
				return true;
			}
		}
		if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick)
			return false;
		auto *mouse = static_cast<QMouseEvent *>(event);
		const auto region = PortraitCanvas::portraitPreviewRegion(nativePreview->width(), nativePreview->height());
		const float ratio = previewState.pixelRatio.load();
		if (!region.contains(float(mouse->position().x()) * ratio,
		                     float(mouse->position().y()) * ratio)) return false;
		if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::RightButton) {
			nativePreview->SetPortraitEditing(true);
			const QPointF point = scenePoint(mouse->position());
			if (contextRequested)
				contextRequested(float(point.x()), float(point.y()), mouse->globalPosition().toPoint());
			mouse->accept();
			return true;
		}
		if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton) {
			nativePreview->SetPortraitEditing(true);
			const QPointF point = scenePoint(mouse->position());
			if (doubleClicked) doubleClicked(float(point.x()), float(point.y()));
			mouse->accept();
			return true;
		}
		return false;
	}

private:
	QPointF scenePoint(const QPointF &point) const
	{
		const auto region = PortraitCanvas::portraitPreviewRegion(nativePreview->width(), nativePreview->height());
		const float ratio = previewState.pixelRatio.load();
		const float scale = float(region.width) / float(portraitWidth);
		if (scale <= 0.f) return {-1, -1};
		return {(point.x() * ratio - float(region.x)) / scale,
			(point.y() * ratio - float(region.y)) / scale};
	}
	QPointer<OBSBasicPreview> nativePreview;

public:
	static void render(obs_source_t *source, uint32_t cx, uint32_t cy)
	{
		if (!source || !cx || !cy) return;
		const auto region = PortraitCanvas::portraitPreviewRegion(int(cx), int(cy));
		if (region.width <= 0 || region.height <= 0) return;
		gs_projection_push();
		gs_viewport_push();
		gs_set_viewport(region.x, region.y, region.width, region.height);
		gs_ortho(0.f, float(portraitWidth), 0.f, float(portraitHeight), -100.f, 100.f);
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		vec4 background{};
		vec4_set(&background, 0.f, 0.f, 0.f, 1.f);
		gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &background);
		while (gs_effect_loop(effect, "Solid")) gs_draw_quadf(nullptr, 0, float(portraitWidth), float(portraitHeight));
		obs_source_video_render(source);
		if (auto *native = OBSBasicPreview::Get(); native && native->IsPortraitEditing()) {
			// Draw OBS's own source handles in the portrait viewport.
			gs_ortho(0.f, float(region.width), 0.f, float(region.height), -100.f, 100.f);
			native->DrawSceneEditing();
		}
		gs_viewport_pop();
		gs_projection_pop();
	}
};

} // namespace

float PortraitCanvas::portraitDisplayFactor(int landscapeHeight)
{
	return float(landscapeHeight) / float(portraitHeight);
}

int PortraitCanvas::workspaceWidth(int landscapeWidth)
{
	obs_video_info video{};
	const int height = obs_get_video_info(&video) ? int(video.base_height) : 1440;
	return landscapeWidth + workspaceGap + int(std::lround(float(portraitWidth) * portraitDisplayFactor(height)));
}

int PortraitCanvas::workspaceHeight(int landscapeHeight)
{
	return landscapeHeight;
}

OBSScene PortraitCanvas::editingScene() const
{
	std::lock_guard<std::mutex> guard(previewSourceMutex);
	return OBSScene(scene);
}

PortraitCanvas::PreviewRegion PortraitCanvas::portraitPreviewRegion(int, int)
{
	// Coordinates are in the SAME physical GPU pixels as OBSBasic::GetDisplayRect.
	// Both previews have equal visible heights, although their source/output
	// dimensions remain 2560x1440 and 1080x1920 independently.
	auto *main = OBSBasic::Get();
	obs_video_info video{};
	if (!main || !obs_get_video_info(&video) || video.base_width == 0) return {0, 0, 0, 0};
	int x = 0, y = 0, w = 0, h = 0;
	main->GetDisplayRect(x, y, w, h);
	const float scale = float(w) / float(video.base_width);
	const int px = x + w + int(std::lround(float(workspaceGap) * scale));
	const int py = y;
	const float factor = portraitDisplayFactor(int(video.base_height));
	return {px, py, int(std::lround(float(portraitWidth) * factor * scale)),
		int(std::lround(float(portraitHeight) * factor * scale))};
}

bool PortraitCanvas::previewVisible() const
{
	return previewState.visible.load(std::memory_order_acquire);
}

void PortraitCanvas::renderPreview(uint32_t displayWidth, uint32_t displayHeight) const
{
	if (!previewVisible()) return;
	obs_source_t *source = nullptr;
	{
		std::lock_guard<std::mutex> guard(previewSourceMutex);
		if (previewVisible() && scene)
			source = obs_source_get_ref(obs_scene_get_source(scene));
	}
	if (!source) return;
	PortraitDisplay::render(source, displayWidth, displayHeight);
	obs_source_release(source);
}

PortraitCanvas &PortraitCanvas::instance()
{
	static PortraitCanvas manager;
	return manager;
}

bool PortraitCanvas::ensure()
{
	if (canvas) return true;
	obs_video_info video{};
	if (!obs_get_video_info(&video)) return false;
	video.base_width = portraitWidth;
	video.base_height = portraitHeight;
	video.output_width = portraitWidth;
	video.output_height = portraitHeight;
	canvas = obs_canvas_create_private("XINKX Portrait", &video, ACTIVATE | SCENE_REF | EPHEMERAL);
	if (!canvas || !obs_canvas_has_video(canvas)) {
		writeDiagnostic(QStringLiteral("portrait"), QStringLiteral("Unable to create independent video mix"));
		shutdown();
		return false;
	}
	scene = obs_canvas_scene_create(canvas, "XINKX Portrait Composition");
	if (!scene) { shutdown(); return false; }
	obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
	return true;
}

video_t *PortraitCanvas::video()
{
	if (!ensure()) return nullptr;
	sync();
	return obs_canvas_get_video(canvas);
}

void PortraitCanvas::duplicateLayout(const QString &fromScene, const QString &toScene)
{
	if (fromScene.isEmpty() || toScene.isEmpty() || fromScene == toScene) return;
	QSettings settings(studioSettingsPath(), QSettings::IniFormat);
	// A source layout may exist without extra sources, but a duplicate must
	// never replace an independently edited destination scene.
	if (settings.contains(storeKey(toScene)) || settings.contains(extraKey(toScene))) return;
	const QString sourceLayout = storeKey(fromScene);
	const QString sourceExtras = extraKey(fromScene);
	if (settings.contains(sourceLayout)) settings.setValue(storeKey(toScene), settings.value(sourceLayout));
	if (settings.contains(sourceExtras)) settings.setValue(extraKey(toScene), settings.value(sourceExtras));
	settings.sync();
}

void PortraitCanvas::sync()
{
	if (!ensure()) return;
	if (editing) return;
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	if (!current) return;
	obs_scene_t *activeScene = obs_scene_from_source(current);
	if (!activeScene) return;
	const QString name = QString::fromUtf8(obs_source_get_name(current));
	std::vector<Captured> items;
	obs_scene_enum_items(activeScene, captureItem, &items);
	QString identifier = name;
	for (const auto &item : items) {
		identifier += QStringLiteral("|%1:%2:%3:%4").arg(item.key, item.name)
			.arg(obs_source_get_width(item.source)).arg(obs_source_get_height(item.source));
	}
	if (identifier != signature) rebuild(activeScene, name, identifier);
}

void PortraitCanvas::rebuild(obs_scene_t *mainScene, const QString &name, const QString &newSignature)
{
	if (!scene || !mainScene) return;
	std::vector<Captured> mainSources;
	obs_scene_enum_items(mainScene, captureItem, &mainSources);
	QSettings store(studioSettingsPath(), QSettings::IniFormat);
	QJsonObject saved = QJsonDocument::fromJson(store.value(storeKey(name)).toByteArray()).object();
	QJsonArray extras = QJsonDocument::fromJson(store.value(extraKey(name)).toByteArray()).array();
	// Previous versions mirrored every item in the horizontal OBS scene. Migrate
	// existing edited layouts once, but never mirror newly added OBS sources.
	// A new portrait scene starts empty until the user adds its own layers.
	if (!store.contains(extraKey(name))) {
		for (const auto &item : mainSources) {
			if (!saved.contains(item.key)) continue;
			const QString uuid = QString::fromUtf8(obs_source_get_uuid(item.source));
			if (uuid.isEmpty()) continue;
			extras.append(uuid);
			saved.insert(QStringLiteral("portrait:%1").arg(uuid), saved.value(item.key));
		}
		store.setValue(extraKey(name), QJsonDocument(extras).toJson(QJsonDocument::Compact));
		store.setValue(storeKey(name), QJsonDocument(saved).toJson(QJsonDocument::Compact));
	}
	std::vector<Captured> originals;
	for (const auto &entry : extras) {
		const QJsonObject storedSource = entry.toObject();
		const QString uuid = entry.isString() ? entry.toString() : storedSource.value(QStringLiteral("uuid")).toString();
		if (uuid.isEmpty() || std::any_of(originals.cbegin(), originals.cend(), [&](const Captured &item) {
			return QString::fromUtf8(obs_source_get_uuid(item.source)) == uuid;
		})) continue;
		obs_source_t *source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (!source && storedSource.contains(QStringLiteral("source"))) {
			const QByteArray serialized = QJsonDocument(storedSource.value(QStringLiteral("source")).toObject())
				.toJson(QJsonDocument::Compact);
			OBSDataAutoRelease sourceData = obs_data_create_from_json(serialized.constData());
			if (sourceData) source = obs_load_source(sourceData);
		}
		if (!source) continue;
		Captured captured;
		captured.source = source;
		captured.key = QStringLiteral("portrait:%1").arg(uuid);
		captured.name = QString::fromUtf8(obs_source_get_name(source));
		const auto original = std::find_if(mainSources.cbegin(), mainSources.cend(), [&](const Captured &item) {
			return QString::fromUtf8(obs_source_get_uuid(item.source)) == uuid;
		});
		if (original != mainSources.cend()) {
			captured.info = original->info;
			captured.crop = original->crop;
			captured.visible = original->visible;
		} else {
			captured.info.bounds_type = OBS_BOUNDS_SCALE_INNER;
			captured.info.bounds = {float(portraitWidth) * .75f, float(portraitHeight) * .40f};
			captured.info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
			captured.info.bounds_alignment = OBS_ALIGN_CENTER;
			captured.info.scale = {1.f, 1.f};
		}
		originals.push_back(std::move(captured));
		obs_source_release(source);
	}
	// Preserve the independent portrait z-order after a restart. OBS scene
	// items are appended bottom-to-top, matching the layers vector order.
	std::stable_sort(originals.begin(), originals.end(), [&](const Captured &a, const Captured &b) {
		return saved.value(a.key).toObject().value(QStringLiteral("order")).toInt(INT_MAX) <
			saved.value(b.key).toObject().value(QStringLiteral("order")).toInt(INT_MAX);
	});
	std::vector<obs_sceneitem_t *> old;
	obs_scene_enum_items(scene, collectItemRefs, &old);
	for (auto *item : old) { obs_sceneitem_remove(item); obs_sceneitem_release(item); }
	layers.clear();
	obs_video_info base{};
	if (!obs_get_video_info(&base) || !base.base_width || !base.base_height) return;
	const float ratioX = float(portraitWidth) / base.base_width;
	const float ratioY = float(portraitHeight) / base.base_height;
	for (const auto &source : originals) {
		obs_sceneitem_t *item = obs_scene_add(scene, source.source);
		if (!item) continue;
		obs_transform_info info = source.info;
		float width = info.bounds_type == OBS_BOUNDS_NONE
			? obs_source_get_width(source.source) * std::abs(info.scale.x) : info.bounds.x;
		float height = info.bounds_type == OBS_BOUNDS_NONE
			? obs_source_get_height(source.source) * std::abs(info.scale.y) : info.bounds.y;
		// Some capture sources report 0x0 while the game/camera is starting.
		// Never persist an unusable 1x1 scene item as a result.
		if (!std::isfinite(width) || width < 16) width = base.base_width * .40f;
		if (!std::isfinite(height) || height < 16) height = base.base_height * .40f;
		const bool fullScreen = width >= base.base_width * .85f && height >= base.base_height * .85f;
		info.pos.x *= ratioX;
		info.pos.y *= ratioY;
		info.bounds = {std::max(16.f, width * ratioX), std::max(16.f, height * ratioY)};
		info.bounds_type = fullScreen ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		info.crop_to_bounds = fullScreen;
		info.scale = {1.f, 1.f};
		const QJsonObject savedLayer = saved.value(source.key).toObject();
		if (!savedLayer.isEmpty()) {
			info.pos = {float(savedLayer.value(QStringLiteral("x")).toDouble(info.pos.x)),
				float(savedLayer.value(QStringLiteral("y")).toDouble(info.pos.y))};
			const float savedWidth = float(savedLayer.value(QStringLiteral("w")).toDouble(info.bounds.x));
			const float savedHeight = float(savedLayer.value(QStringLiteral("h")).toDouble(info.bounds.y));
			if (savedWidth >= 16 && savedHeight >= 16) info.bounds = {savedWidth, savedHeight};
			info.bounds_type = savedLayer.value(QStringLiteral("fill")).toBool(fullScreen)
				? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER;
			info.crop_to_bounds = info.bounds_type == OBS_BOUNDS_SCALE_OUTER;
			info.rot = float(savedLayer.value(QStringLiteral("rotation")).toDouble(info.rot));
			info.scale = {float(savedLayer.value(QStringLiteral("scaleX")).toDouble(info.scale.x)),
				float(savedLayer.value(QStringLiteral("scaleY")).toDouble(info.scale.y))};
			info.alignment = uint32_t(savedLayer.value(QStringLiteral("alignment")).toInt(info.alignment));
			info.bounds_alignment = uint32_t(savedLayer.value(QStringLiteral("boundsAlignment")).toInt(info.bounds_alignment));
		}
		obs_sceneitem_set_info2(item, &info);
		obs_sceneitem_crop crop = source.crop;
		if (savedLayer.contains(QStringLiteral("cropLeft"))) {
			crop.left = savedLayer.value(QStringLiteral("cropLeft")).toInt();
			crop.top = savedLayer.value(QStringLiteral("cropTop")).toInt();
			crop.right = savedLayer.value(QStringLiteral("cropRight")).toInt();
			crop.bottom = savedLayer.value(QStringLiteral("cropBottom")).toInt();
		}
		obs_sceneitem_set_crop(item, &crop);
		obs_sceneitem_set_visible(item, savedLayer.value(QStringLiteral("visible")).toBool(source.visible));
		obs_sceneitem_set_locked(item, savedLayer.value(QStringLiteral("locked")).toBool(false));
		layers.push_back({source.key, source.name, item});
	}
	sceneName = name;
	signature = newSignature;
	writeDiagnostic(QStringLiteral("portrait"), QStringLiteral("Scene updated: %1, sources=%2")
		.arg(name).arg(layers.size()));
}

void PortraitCanvas::save()
{
	if (sceneName.isEmpty() || !scene) return;
	// Native SourceTree supports drag-and-drop reordering. Persist the actual
	// libobs z-order, never the order of the obsolete private QListWidget.
	std::vector<obs_sceneitem_t *> actualItems;
	obs_scene_enum_items(scene, collectItemRefs, &actualItems);
	QVector<Layer> reordered;
	for (auto *item : actualItems) {
		const auto found = std::find_if(layers.cbegin(), layers.cend(),
			[item](const Layer &layer) { return layer.item == item; });
		if (found != layers.cend()) reordered.push_back(*found);
		obs_sceneitem_release(item);
	}
	layers = std::move(reordered);
	QJsonObject objects;
	for (int index = 0; index < layers.size(); ++index) {
		const auto &layer = layers.at(index);
		obs_transform_info info{};
		obs_sceneitem_get_info2(layer.item, &info);
		obs_sceneitem_crop crop{};
		obs_sceneitem_get_crop(layer.item, &crop);
		objects.insert(layer.key, QJsonObject{
			{QStringLiteral("x"), info.pos.x}, {QStringLiteral("y"), info.pos.y},
			{QStringLiteral("w"), info.bounds.x}, {QStringLiteral("h"), info.bounds.y},
			{QStringLiteral("fill"), info.bounds_type == OBS_BOUNDS_SCALE_OUTER},
			{QStringLiteral("rotation"), info.rot},
			{QStringLiteral("scaleX"), info.scale.x}, {QStringLiteral("scaleY"), info.scale.y},
			{QStringLiteral("alignment"), int(info.alignment)},
			{QStringLiteral("boundsAlignment"), int(info.bounds_alignment)},
			{QStringLiteral("cropLeft"), crop.left}, {QStringLiteral("cropTop"), crop.top},
			{QStringLiteral("cropRight"), crop.right}, {QStringLiteral("cropBottom"), crop.bottom},
			{QStringLiteral("order"), index},
			{QStringLiteral("locked"), obs_sceneitem_locked(layer.item)},
			{QStringLiteral("visible"), obs_sceneitem_visible(layer.item)}});
	}
	QSettings store(studioSettingsPath(), QSettings::IniFormat);
	store.setValue(storeKey(sceneName), QJsonDocument(objects).toJson(QJsonDocument::Compact));
	// Portrait-only native sources are absent from OBS's scene collection.
	// Keep a complete source snapshot so they survive an application restart.
	auto extras = QJsonDocument::fromJson(store.value(extraKey(sceneName)).toByteArray()).array();
	bool updated = false;
	QSet<QString> present;
	for (const auto &layer : std::as_const(layers)) {
		obs_source_t *source = obs_sceneitem_get_source(layer.item);
		if (source) present.insert(QString::fromUtf8(obs_source_get_uuid(source)));
	}
	for (int index = extras.size() - 1; index >= 0; --index) {
		const auto record = extras.at(index).toObject();
		const QString uuid = extras.at(index).isString() ? extras.at(index).toString()
			: record.value(QStringLiteral("uuid")).toString();
		if (present.contains(uuid)) continue;
		extras.removeAt(index);
		updated = true;
	}
	for (int index = 0; index < extras.size(); ++index) {
		const QJsonObject record = extras.at(index).toObject();
		const QString uuid = extras.at(index).isString() ? extras.at(index).toString()
			: record.value(QStringLiteral("uuid")).toString();
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (!source) continue;
		OBSDataAutoRelease sourceData = obs_save_source(source);
		const char *json = obs_data_get_json(sourceData);
		QJsonObject copy = record;
		copy.insert(QStringLiteral("uuid"), uuid);
		copy.insert(QStringLiteral("source"), QJsonDocument::fromJson(QByteArray(json ? json : "{}" )).object());
		extras.replace(index, copy);
		updated = true;
	}
	if (updated) store.setValue(extraKey(sceneName), QJsonDocument(extras).toJson(QJsonDocument::Compact));
	store.sync();
	if (store.status() != QSettings::NoError)
		writeDiagnostic(QStringLiteral("portrait"), QStringLiteral("Failed to save portrait layout"));
}

void PortraitCanvas::updateLayer(const QString &key, float x, float y, float width, float height, bool fill)
{
	for (const auto &layer : std::as_const(layers)) {
		if (layer.key != key) continue;
		obs_transform_info info{};
		obs_sceneitem_get_info2(layer.item, &info);
		info.pos = {x, y};
		info.bounds = {std::max(1.f, width), std::max(1.f, height)};
		info.bounds_type = fill ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER;
		info.crop_to_bounds = fill;
		obs_sceneitem_set_info2(layer.item, &info);
		save();
		return;
	}
}

void PortraitCanvas::edit(QWidget *parent)
{
	Q_UNUSED(parent);
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow) return;
	if (auto *existing = mainWindow->findChild<QWidget *>(QStringLiteral("xinkxPortraitEditor"))) {
		return;
	}
	if (!ensure()) return;
	sync();
	auto *nativePreview = mainWindow->findChild<OBSBasicPreview *>(QStringLiteral("preview"));
	if (!nativePreview) {
		writeDiagnostic(QStringLiteral("portrait"), QStringLiteral("Main OBS preview unavailable"));
		return;
	}
	editing = true;
	// Lifecycle owner only: the visible scene is drawn by OBS's own preview.
	auto *side = new QWidget(mainWindow);
	side->setObjectName(QStringLiteral("xinkxPortraitEditor"));
	side->setAttribute(Qt::WA_DontShowOnScreen, true);
	side->hide();
	auto *column = new QVBoxLayout(side);
	column->setContentsMargins(0, 0, 0, 0);
	column->setSpacing(0);
	auto *preview = new PortraitDisplay(nativePreview, side);
	auto *toolbarWidget = new QWidget(side);
	auto *toolbar = new QHBoxLayout(toolbarWidget);
	toolbar->setContentsMargins(0, 0, 0, 0);
	auto *add = new QPushButton(QStringLiteral("+ Источник"), side);
	auto *remove = new QPushButton(QStringLiteral("−"), side);
	auto *properties = new QPushButton(QStringLiteral("Свойства"), side);
	auto *snap = new QCheckBox(QStringLiteral("Привязка"), side);
	snap->setChecked(true);
	toolbar->addWidget(add);
	toolbar->addWidget(remove);
	toolbar->addWidget(properties);
	toolbar->addWidget(snap);
	auto *list = new QListWidget(side);
	list->setObjectName(QStringLiteral("xinkxPortraitSourceList"));
	list->hide(); // Internal selection bridge; the visible list is OBS's native SourceTree.
	SourceTree *nativePortraitTree = nullptr;
	list->setToolTip(QStringLiteral("Только вертикальная композиция. Удаление здесь не удаляет источник из 16:9."));
	// OBS owns the horizontal source tree and its native editing commands.
	// This list owns only portrait scene items, directly below that tree.
	// There are no global canvas/source toggles; each preview and its list
	// are unambiguously associated with one composition.
	if (auto *sourcesDock = mainWindow->findChild<QDockWidget *>(QStringLiteral("sourcesDock"))) {
		if (auto *nativeFrame = sourcesDock->findChild<QWidget *>(QStringLiteral("sourcesFrame"))) {
			QWidget *contents = nativeFrame->parentWidget();
			auto *dockLayout = contents ? static_cast<QVBoxLayout *>(contents->layout()) : nullptr;
			if (dockLayout) {
				auto *portraitSources = new QWidget(contents);
				portraitSources->setObjectName(QStringLiteral("xinkxPortraitSourcesWorkspace"));
				auto *sourceColumn = new QVBoxLayout(portraitSources);
				sourceColumn->setContentsMargins(1, 2, 1, 1);
				sourceColumn->setSpacing(3);
				auto *sourceHeading = new QLabel(QStringLiteral("9:16 · Вертикальные источники"), portraitSources);
				sourceHeading->setStyleSheet(QStringLiteral("font-weight:600"));
				sourceColumn->addWidget(sourceHeading);
				nativePortraitTree = new SourceTree(portraitSources, [this] { return OBSScene(scene); });
				nativePortraitTree->setObjectName(QStringLiteral("xinkxPortraitNativeSourceTree"));
				nativePortraitTree->setSelectionMode(QAbstractItemView::SingleSelection);
				if (auto *landscapeTree = nativeFrame->findChild<SourceTree *>())
					landscapeTree->setSelectionMode(QAbstractItemView::SingleSelection);
				nativePortraitTree->setDragDropMode(QAbstractItemView::InternalMove);
				sourceColumn->addWidget(nativePortraitTree, 1);
				auto *portraitToolbar = new QToolBar(portraitSources);
				portraitToolbar->setObjectName(QStringLiteral("xinkxPortraitSourcesToolbar"));
				portraitToolbar->setIconSize(QSize(16, 16));
				portraitToolbar->setFloatable(false);
				for (const char *name : {"actionAddSource", "actionRemoveSource", "", "actionSourceProperties", "",
				                         "actionSourceUp", "actionSourceDown"}) {
					if (!*name) { portraitToolbar->addSeparator(); continue; }
					if (QAction *original = mainWindow->findChild<QAction *>(QString::fromLatin1(name))) {
						auto *action = portraitToolbar->addAction(original->icon(), original->text());
						action->setToolTip(original->toolTip());
						action->setObjectName(QStringLiteral("portrait_") + QString::fromLatin1(name));
						action->setProperty("class", original->property("class"));
						// OBS themes tint toolbar SVGs through a QToolButton class property.
						if (auto *button = qobject_cast<QToolButton *>(portraitToolbar->widgetForAction(action))) {
							button->setProperty("class", original->property("class"));
							button->style()->unpolish(button);
							button->style()->polish(button);
						}
					}
				}
				sourceColumn->addWidget(portraitToolbar);
				dockLayout->removeWidget(nativeFrame);
				auto *splitter = new QSplitter(Qt::Vertical, contents);
				splitter->setObjectName(QStringLiteral("xinkxSourcesSplitter"));
				splitter->setChildrenCollapsible(false);
				splitter->addWidget(nativeFrame);
				splitter->addWidget(portraitSources);
				splitter->setSizes({240, 240});
				dockLayout->insertWidget(0, splitter, 1);
				QPointer<QWidget> guardedContents(contents);
				QPointer<QWidget> guardedFrame(nativeFrame);
				QPointer<QSplitter> guardedSplitter(splitter);
				QObject::connect(side, &QObject::destroyed, contents, [=] {
					if (!guardedContents || !guardedFrame || !guardedSplitter) return;
					auto *layout = static_cast<QVBoxLayout *>(guardedContents->layout());
					if (!layout) return;
					layout->removeWidget(guardedSplitter);
					guardedFrame->setParent(guardedContents);
					layout->insertWidget(0, guardedFrame);
					delete guardedSplitter;
				});
			}
		}
	}
	if (!nativePortraitTree)
		writeDiagnostic(QStringLiteral("portrait"), QStringLiteral("Native Sources dock unavailable"));
	// No preview splitter, second OBSQTDisplay or adjacent GPU child window.
	// RenderMain composes 16:9 and 9:16 on the existing native display.
	// Dragging changes only this scene item's transform. Do not write an INI file
	// on every mouse-move; persist once the user stops interacting.
	auto *saveDelay = new QTimer(side);
	saveDelay->setSingleShot(true);
	saveDelay->setInterval(350);
	QObject::connect(saveDelay, &QTimer::timeout, side, [this] { save(); });
	if (nativePortraitTree)
		QObject::connect(nativePortraitTree->model(), &QAbstractItemModel::rowsMoved, side,
			[=] { saveDelay->start(); });
	const auto sourceUuid = [](obs_sceneitem_t *item) {
		obs_source_t *source = item ? obs_sceneitem_get_source(item) : nullptr;
		return source ? QString::fromUtf8(obs_source_get_uuid(source)) : QString{};
	};
	const auto selectedUuid = [=] {
		return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString{};
	};
	// The same OBS input can be attached independently to either composition;
	// the list tracks portrait scene-item keys rather than shared input UUIDs.
	const auto selectedLayer = [=] {
		const QString key = list->currentItem() ? list->currentItem()->data(Qt::UserRole + 1).toString() : QString{};
		for (int index = 0; index < layers.size(); ++index)
			if (layers.at(index).key == key && !key.isEmpty()) return index;
		return -1;
	};
	const auto selectLayer = [=](int layer) {
		const QString key = layer >= 0 && layer < layers.size() ? layers.at(layer).key : QString{};
		for (int index = 0; index < list->count(); ++index) {
			if (list->item(index)->data(Qt::UserRole + 1).toString() == key && !key.isEmpty()) {
				list->setCurrentRow(index);
				return;
			}
		}
		list->setCurrentRow(-1);
	};
	if (nativePortraitTree) {
		QObject::connect(nativePortraitTree->selectionModel(), &QItemSelectionModel::selectionChanged,
			side, [=](const QItemSelection &selected, const QItemSelection &) {
				const auto indexes = selected.indexes();
				if (indexes.isEmpty()) return;
				nativePreview->SetPortraitEditing(true);
				const QModelIndex index = indexes.first();
				OBSSceneItem item = nativePortraitTree->Get(index.row());
				for (int row = 0; row < layers.size(); ++row) {
					if (layers.at(row).item == item) { selectLayer(row); return; }
				}
			});
	}

	QObject::connect(nativePreview, &OBSBasicPreview::EditingSelectionChanged, side, [=] {
		if (!nativePreview->IsPortraitEditing()) return;
		for (int row = 0; row < layers.size(); ++row) {
			if (!obs_sceneitem_selected(layers.at(row).item)) continue;
			selectLayer(row);
			return;
		}
		selectLayer(-1);
	});

	const auto refreshList = [=] {
		const QString selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole + 1).toString() : QString{};
		const QSignalBlocker blocked(list);
		list->clear();
		for (int i = layers.size() - 1; i >= 0; --i) {
			const auto &layer = layers.at(i);
			const QString uuid = sourceUuid(layer.item);
			auto *entry = new QListWidgetItem(layer.name, list);
			entry->setData(Qt::UserRole, uuid);
			entry->setData(Qt::UserRole + 1, layer.key);
			entry->setToolTip(QStringLiteral("Элемент только 9:16. Настройки общего OBS-источника могут использоваться и в 16:9."));
			entry->setFlags(entry->flags() | Qt::ItemIsUserCheckable);
			entry->setCheckState(obs_sceneitem_visible(layer.item) ? Qt::Checked : Qt::Unchecked);
		}
		for (int i = 0; i < list->count(); ++i) {
			if (list->item(i)->data(Qt::UserRole + 1).toString() != selected) continue;
			list->setCurrentRow(i);
			if (nativePortraitTree) nativePortraitTree->RefreshItems();
			return;
		}
		list->setCurrentRow(-1);
		if (nativePortraitTree) nativePortraitTree->RefreshItems();
	};
	const auto updateSelection = [] {}; // Native OBS owns the selection outline.
	add->setToolTip(QStringLiteral("Добавить источник на вертикальный холст"));
	remove->setToolTip(QStringLiteral("Удалить только из вертикальной композиции"));
	// Native OBSBasicPreview owns selection for both independent scenes.
	const auto bounds = [=](int row, float *left, float *top, float *right, float *bottom) {
		if (row < 0 || row >= layers.size()) return;
		matrix4 box{};
		obs_sceneitem_get_box_transform(layers.at(row).item, &box);
		*left = *top = 1e7f; *right = *bottom = -1e7f;
		for (int index = 0; index < 4; ++index) {
			vec3 unit{}, point{};
			vec3_set(&unit, index & 1 ? 1.f : 0.f, index & 2 ? 1.f : 0.f, 0.f);
			vec3_transform(&point, &unit, &box);
			*left = std::min(*left, point.x); *top = std::min(*top, point.y);
			*right = std::max(*right, point.x); *bottom = std::max(*bottom, point.y);
		}
	};
	const auto insideItem = [=](int row, float px, float py) {
		if (row < 0 || row >= layers.size()) return false;
		auto *item = layers.at(row).item;
		if (!obs_sceneitem_visible(item) || obs_sceneitem_locked(item)) return false;
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (!source || !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO)) return false;
		matrix4 box{}, inverse{};
		obs_sceneitem_get_box_transform(item, &box);
		matrix4_inv(&inverse, &box);
		vec3 point{}, local{}, restored{};
		vec3_set(&point, px, py, 0.f);
		vec3_transform(&local, &point, &inverse);
		vec3_transform(&restored, &local, &box);
		return std::abs(restored.x - px) < .01f && std::abs(restored.y - py) < .01f &&
			local.x >= 0.f && local.x <= 1.f && local.y >= 0.f && local.y <= 1.f;
	};
	QObject::connect(list, &QListWidget::currentRowChanged, side, [=] {
		if (nativePortraitTree && nativePortraitTree->selectionModel()) {
			const int row = selectedLayer();
			if (row >= 0) {
				nativePreview->SetPortraitEditing(true);
				for (int i = 0; i < nativePortraitTree->model()->rowCount(); ++i) {
					if (nativePortraitTree->Get(i) != layers.at(row).item) continue;
					const QModelIndex index = nativePortraitTree->model()->index(i, 0);
					nativePortraitTree->selectionModel()->setCurrentIndex(
						index, QItemSelectionModel::ClearAndSelect);
					break;
				}
			} else {
				nativePortraitTree->selectionModel()->clearSelection();
				nativePortraitTree->setCurrentIndex(QModelIndex());
			}
		}
		updateSelection();
	});
	QObject::connect(list, &QListWidget::itemChanged, side, [=](QListWidgetItem *entry) {
		const QString key = entry->data(Qt::UserRole + 1).toString();
		for (const auto &layer : layers) {
			if (layer.key != key) continue;
			obs_sceneitem_set_visible(layer.item, entry->checkState() == Qt::Checked);
			save();
			break;
		}
		updateSelection();
	});
	QObject::connect(properties, &QPushButton::clicked, side, [=] {
		obs_sceneitem_t *item = selectedLayer() >= 0 ? layers.at(selectedLayer()).item : nullptr;
		OBSSourceAutoRelease fallback = item ? nullptr : obs_get_source_by_uuid(selectedUuid().toUtf8().constData());
		obs_source_t *source = item ? obs_sceneitem_get_source(item) : fallback.Get();
		if (source) obs_frontend_open_source_properties(source);
	});
	QObject::connect(remove, &QPushButton::clicked, side, [=] {
		const int row = selectedLayer();
		if (row < 0 || row >= layers.size()) return;
		const auto layer = layers.at(row);
		const QString uuid = layer.key.mid(9);
		QSettings settings(studioSettingsPath(), QSettings::IniFormat);
		auto extras = QJsonDocument::fromJson(settings.value(extraKey(sceneName)).toByteArray()).array();
		for (int i = extras.size() - 1; i >= 0; --i)
			if (extras.at(i).toString() == uuid ||
			    extras.at(i).toObject().value(QStringLiteral("uuid")).toString() == uuid)
				extras.removeAt(i);
		settings.setValue(extraKey(sceneName), QJsonDocument(extras).toJson(QJsonDocument::Compact));
		obs_sceneitem_remove(layer.item);
		layers.removeAt(row);
		save();
		refreshList();
		updateSelection();
	});
	preview->deleteRequested = [=] { remove->click(); };
	const auto addSource = [=](obs_source_t *source, bool /* newSource */) {
		if (!source) return;
		obs_sceneitem_t *item = obs_scene_add(scene, source);
		if (!item) return;
		obs_transform_info info{};
		info.pos = {120.f, 440.f};
		info.bounds = {840.f, 1020.f};
		info.scale = {1.f, 1.f};
		info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		info.bounds_type = OBS_BOUNDS_SCALE_INNER;
		obs_sceneitem_set_info2(item, &info);
		const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
		const QString name = QString::fromUtf8(obs_source_get_name(source));
		layers.push_back({QStringLiteral("portrait:%1").arg(uuid), name, item});
		QSettings settings(studioSettingsPath(), QSettings::IniFormat);
		auto extras = QJsonDocument::fromJson(settings.value(extraKey(sceneName)).toByteArray()).array();
		// Snapshot even shared inputs: deleting the horizontal scene item
		// must not erase a portrait-only item on the next app launch.
		OBSDataAutoRelease snapshot = obs_save_source(source);
		const char *json = snapshot ? obs_data_get_json(snapshot) : nullptr;
		extras.append(QJsonObject{{QStringLiteral("uuid"), uuid},
			{QStringLiteral("source"), QJsonDocument::fromJson(QByteArray(json ? json : "{}")).object()}});
		settings.setValue(extraKey(sceneName), QJsonDocument(extras).toJson(QJsonDocument::Compact));
		save();
		refreshList();
		selectLayer(layers.size() - 1);
	};
	const auto nativeAddDialog = [=] {
		auto *main = OBSBasic::Get();
		if (!main) return;
		auto *dialog = new OBSBasicSourceSelect(main, main->undo_s,
			[=](obs_source_t *source, bool newSource) { addSource(source, newSource); });
		dialog->setAttribute(Qt::WA_DeleteOnClose, true);
		QObject::connect(side, &QObject::destroyed, dialog, &QObject::deleteLater);
		dialog->show();
	};
	if (auto *actions = mainWindow->findChild<QToolBar *>(QStringLiteral("xinkxPortraitSourcesToolbar"))) {
		const auto connectAction = [=](const char *name, const std::function<void()> &command) {
			if (auto *action = actions->findChild<QAction *>(QStringLiteral("portrait_") + QString::fromLatin1(name)))
				QObject::connect(action, &QAction::triggered, side, command);
		};
		connectAction("actionAddSource", nativeAddDialog);
		connectAction("actionRemoveSource", [=] { remove->click(); });
		connectAction("actionSourceProperties", [=] { properties->click(); });
		const auto moveLayer = [=](bool up) {
			const int row = selectedLayer();
			const int target = row + (up ? 1 : -1);
			if (row < 0 || target < 0 || target >= layers.size()) return;
			obs_sceneitem_set_order(layers.at(row).item, up ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
			layers.swapItemsAt(row, target);
			save(); refreshList(); selectLayer(target); updateSelection();
		};
		connectAction("actionSourceUp", [=] { moveLayer(true); });
		connectAction("actionSourceDown", [=] { moveLayer(false); });
	}
	QObject::connect(add, &QPushButton::clicked, side, [=] {
		QMenu menu(side);
		// Copy the selected OBS scene item into the portrait composition. The
		// input can be shared, but each scene owns its own item, visibility,
		// transform, lock state and removal operation.
		OBSSourceAutoRelease horizontalScene = obs_frontend_get_current_scene();
		obs_scene_t *horizontal = horizontalScene ? obs_scene_from_source(horizontalScene) : nullptr;
		struct Selected { obs_source_t *source = nullptr; } selected;
		if (horizontal) obs_scene_enum_items(horizontal, [](obs_scene_t *, obs_sceneitem_t *item, void *context) {
			if (!obs_sceneitem_selected(item)) return true;
			static_cast<Selected *>(context)->source = obs_sceneitem_get_source(item);
			return false;
		}, &selected);
		auto *copyHorizontal = menu.addAction(QStringLiteral("Добавить выделенный источник 16:9 → 9:16"));
		const QString selectedSourceUuid = selected.source ? QString::fromUtf8(obs_source_get_uuid(selected.source)) : QString{};
		const bool alreadyLinked = std::any_of(layers.cbegin(), layers.cend(), [&](const Layer &layer) {
			return sourceUuid(layer.item) == selectedSourceUuid;
		});
		copyHorizontal->setEnabled(!selectedSourceUuid.isEmpty() && !alreadyLinked);
		QObject::connect(copyHorizontal, &QAction::triggered, side, [=] {
			OBSSourceAutoRelease shared = obs_get_source_by_uuid(selectedSourceUuid.toUtf8().constData());
			if (shared) addSource(shared, false);
		});
		menu.addSeparator();
		// Game Capture explicitly forbids generic source duplication. A new
		// instance must be created, with independent process/window properties.
		auto *newGameCapture = menu.addAction(QStringLiteral("Создать новый захват игры · 9:16"));
		QObject::connect(newGameCapture, &QAction::triggered, side, [=] {
			const QString name = QStringLiteral("9:16 · Захват игры · %1")
				.arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(6));
			OBSDataAutoRelease settings = obs_data_create();
			obs_source_t *created = obs_source_create("game_capture", name.toUtf8().constData(), settings, nullptr);
			if (!created) return;
			addSource(created, true);
			obs_frontend_open_source_properties(created);
			obs_source_release(created);
		});
		menu.addSeparator();
		const std::array<std::pair<const char *, const char *>, 6> nativeTypes = {{
			{"xinkx_chat", "Чат"}, {"xinkx_reactions", "Реакции"},
			{"xinkx_alerts", "Оповещения"}, {"xinkx_timer", "Таймер"},
			{"xinkx_viewers", "Зрители"}, {"xinkx_frame", "Рамка камеры"},
		}};
		for (const auto &type : nativeTypes) {
			auto *action = menu.addAction(QStringLiteral("Создать XINKX · %1").arg(QString::fromUtf8(type.second)));
			QObject::connect(action, &QAction::triggered, side, [=] {
				const QString name = QStringLiteral("XINKX · %1 · %2").arg(QString::fromUtf8(type.second),
					QUuid::createUuid().toString(QUuid::WithoutBraces).left(6));
				OBSDataAutoRelease settings = obs_data_create();
				obs_source_t *source = obs_source_create(type.first, name.toUtf8().constData(), settings, nullptr);
				if (source) { addSource(source, true); obs_source_release(source); }
			});
		}
		menu.addSeparator();
		auto *reuseMenu = menu.addMenu(QStringLiteral("Использовать источник OBS · общие свойства"));
		auto *duplicateMenu = menu.addMenu(QStringLiteral("Создать независимую копию источника"));
		struct Choice { QString uuid, name; };
		QVector<Choice> choices;
		obs_enum_sources([](void *context, obs_source_t *source) {
			if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT) return true;
			const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
			if (uuid.isEmpty()) return true;
			static_cast<QVector<Choice> *>(context)->push_back(
				{uuid, QString::fromUtf8(obs_source_get_name(source))});
			return true;
		}, &choices);
		for (const auto &candidate : choices) {
			OBSSourceAutoRelease existingSource = obs_get_source_by_uuid(candidate.uuid.toUtf8().constData());
			const bool exists = std::any_of(layers.cbegin(), layers.cend(), [&](const Layer &layer) {
				return QString::fromUtf8(obs_source_get_uuid(obs_sceneitem_get_source(layer.item))) == candidate.uuid;
			});
			if (!exists) {
				auto *reuse = reuseMenu->addAction(candidate.name);
				QObject::connect(reuse, &QAction::triggered, side, [=] {
					OBSSourceAutoRelease source = obs_get_source_by_uuid(candidate.uuid.toUtf8().constData());
					if (source) addSource(source, false);
				});
			}
			if (existingSource && !(obs_source_get_output_flags(existingSource) & OBS_SOURCE_DO_NOT_DUPLICATE)) {
				auto *clone = duplicateMenu->addAction(candidate.name);
				QObject::connect(clone, &QAction::triggered, side, [=] {
					OBSSourceAutoRelease original = obs_get_source_by_uuid(candidate.uuid.toUtf8().constData());
					if (!original) return;
					const QString newName = QStringLiteral("9:16 · %1 · %2").arg(candidate.name,
						QUuid::createUuid().toString(QUuid::WithoutBraces).left(6));
					obs_source_t *copy = obs_source_duplicate(original, newName.toUtf8().constData(), false);
					if (copy) { addSource(copy, true); obs_source_release(copy); }
				});
			}
		}
		if (reuseMenu->isEmpty()) reuseMenu->setEnabled(false);
		if (duplicateMenu->isEmpty()) duplicateMenu->setEnabled(false);
		menu.exec(add->mapToGlobal(QPoint(0, add->height())));
	});
	// These commands target the portrait scene item directly. Calling OBSBasic's
	// source context menu here would silently operate on the horizontal scene.
	const auto showContext = [=](int row, const QPoint &globalPoint) {
		QMenu menu(side);
		auto *fitPreview = menu.addAction(QStringLiteral("Масштаб предпросмотра · по размеру"));
		QObject::connect(fitPreview, &QAction::triggered, side, [=] { preview->resetZoom(); });
		menu.addSeparator();
		if (row < 0 || row >= layers.size()) {
			auto *addAction = menu.addAction(QStringLiteral("Добавить источник…"));
			QObject::connect(addAction, &QAction::triggered, side, nativeAddDialog);
			menu.exec(globalPoint);
			return;
		}
		selectLayer(row);
		auto *item = layers.at(row).item;
		auto *source = obs_sceneitem_get_source(item);
		auto *propertiesAction = menu.addAction(QStringLiteral("Свойства"));
		QObject::connect(propertiesAction, &QAction::triggered, side, [=] { properties->click(); });
		auto *filtersAction = menu.addAction(QStringLiteral("Фильтры"));
		QObject::connect(filtersAction, &QAction::triggered, side, [=] {
			if (source) obs_frontend_open_source_filters(source);
		});
		menu.addSeparator();
		auto *visibleAction = menu.addAction(QStringLiteral("Видимый источник"));
		visibleAction->setCheckable(true);
		visibleAction->setChecked(obs_sceneitem_visible(item));
		QObject::connect(visibleAction, &QAction::toggled, side, [=](bool visible) {
			obs_sceneitem_set_visible(item, visible);
			save(); refreshList(); updateSelection();
		});
		auto *lockedAction = menu.addAction(QStringLiteral("Заблокировать"));
		lockedAction->setCheckable(true);
		lockedAction->setChecked(obs_sceneitem_locked(item));
		QObject::connect(lockedAction, &QAction::toggled, side, [=](bool locked) {
			obs_sceneitem_set_locked(item, locked);
			save(); updateSelection();
		});
		auto *orderMenu = menu.addMenu(QStringLiteral("Порядок"));
		const auto changeOrder = [=](bool upwards) {
			const int next = row + (upwards ? 1 : -1);
			if (next < 0 || next >= layers.size()) return;
			obs_sceneitem_set_order(item, upwards ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN);
			layers.swapItemsAt(row, next);
			save(); refreshList(); selectLayer(next); updateSelection();
		};
		auto *up = orderMenu->addAction(QStringLiteral("Поднять выше"));
		up->setEnabled(row + 1 < layers.size());
		QObject::connect(up, &QAction::triggered, side, [=] { changeOrder(true); });
		auto *down = orderMenu->addAction(QStringLiteral("Опустить ниже"));
		down->setEnabled(row > 0);
		QObject::connect(down, &QAction::triggered, side, [=] { changeOrder(false); });
		auto *transformMenu = menu.addMenu(QStringLiteral("Трансформировать"));
		const auto changeTransform = [=](int operation) {
			obs_transform_info info{};
			obs_sceneitem_get_info2(item, &info);
			if (operation == 0 || operation == 1) {
				info.pos = {0.f, 0.f};
				info.bounds = {float(portraitWidth), float(portraitHeight)};
				info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
				info.bounds_alignment = OBS_ALIGN_CENTER;
				info.bounds_type = operation == 0 ? OBS_BOUNDS_SCALE_INNER : OBS_BOUNDS_SCALE_OUTER;
				info.crop_to_bounds = operation == 1;
			} else if (operation == 2) {
				float left, top, right, bottom;
				bounds(row, &left, &top, &right, &bottom);
				info.pos.x += (float(portraitWidth) - left - right) / 2.f;
				info.pos.y += (float(portraitHeight) - top - bottom) / 2.f;
			} else if (operation == 3) info.rot -= 90.f;
			else if (operation == 4) info.rot += 90.f;
			else if (operation == 5) info.scale.x = -info.scale.x;
			else if (operation == 6) info.scale.y = -info.scale.y;
			else if (operation == 7) {
				info.rot = 0.f;
				info.scale = {1.f, 1.f};
				info.pos = {0.f, 0.f};
				info.bounds = {float(portraitWidth), float(portraitHeight)};
				info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
				info.bounds_type = OBS_BOUNDS_SCALE_INNER;
				info.crop_to_bounds = false;
			}
			obs_sceneitem_set_info2(item, &info);
			if (operation == 7) {
				obs_sceneitem_crop crop{};
				obs_sceneitem_set_crop(item, &crop);
			}
			save(); updateSelection();
		};
		const std::array<const char *, 8> transformNames = {{
			"Уместить на экран", "Заполнить экран (обрезать)", "По центру",
			"Повернуть на 90° влево", "Повернуть на 90° вправо",
			"Отразить по горизонтали", "Отразить по вертикали", "Сбросить трансформацию",
		}};
		for (int operation = 0; operation < int(transformNames.size()); ++operation) {
			auto *action = transformMenu->addAction(QString::fromUtf8(transformNames[operation]));
			QObject::connect(action, &QAction::triggered, side, [=] { changeTransform(operation); });
		}
		menu.addSeparator();
		auto *removeAction = menu.addAction(QStringLiteral("Удалить из 9:16"));
		QObject::connect(removeAction, &QAction::triggered, side, [=] { remove->click(); });
		menu.exec(globalPoint);
	};
	preview->contextRequested = [=](float px, float py, const QPoint &globalPoint) {
		int row = selectedLayer();
		if (!insideItem(row, px, py)) {
			row = -1;
			for (int i = layers.size() - 1; i >= 0; --i)
				if (insideItem(i, px, py)) { row = i; break; }
		}
		showContext(row, globalPoint);
	};
	preview->doubleClicked = [=](float px, float py) {
		for (int i = layers.size() - 1; i >= 0; --i) {
			if (!insideItem(i, px, py)) continue;
			selectLayer(i);
			properties->click();
			break;
		}
	};
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(list, &QListWidget::customContextMenuRequested, side, [=](const QPoint &position) {
		if (auto *clicked = list->itemAt(position)) list->setCurrentItem(clicked);
		showContext(selectedLayer(), list->viewport()->mapToGlobal(position));
	});
	if (nativePortraitTree) {
		nativePortraitTree->setContextMenuPolicy(Qt::CustomContextMenu);
		QObject::connect(nativePortraitTree, &QWidget::customContextMenuRequested, side, [=](const QPoint &position) {
			const QModelIndex index = nativePortraitTree->indexAt(position);
			if (index.isValid()) {
				OBSSceneItem item = nativePortraitTree->Get(index.row());
				for (int row = 0; row < layers.size(); ++row) {
					if (layers.at(row).item == item) { selectLayer(row); break; }
				}
			}
			showContext(selectedLayer(), nativePortraitTree->viewport()->mapToGlobal(position));
		});
	}
	QObject::connect(side, &QObject::destroyed, mainWindow, [this] { editing = false; previewState.visible.store(false); });
	const auto sourceListSignature = [=] {
		QString state = sceneName;
		for (const auto &layer : layers)
			state += QStringLiteral("|V:%1:%2:%3")
				.arg(sourceUuid(layer.item), layer.name,
					obs_sceneitem_visible(layer.item) ? QStringLiteral("1") : QStringLiteral("0"));
		return state;
	};
	auto lastSourceListSignature = std::make_shared<QString>();
	auto *watchScene = new QTimer(side);
	watchScene->setInterval(600);
	QObject::connect(watchScene, &QTimer::timeout, side, [=] {
		const QString before = signature;
		OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
		if (currentScene && QString::fromUtf8(obs_source_get_name(currentScene)) != sceneName)
			save(); // Preserve native eye/lock/order edits before rebuilding for a different OBS scene.
		if (saveDelay->isActive()) { saveDelay->stop(); save(); }
		editing = false;
		sync();
		editing = true;
		for (auto &layer : layers) {
			obs_source_t *source = obs_sceneitem_get_source(layer.item);
			if (source) layer.name = QString::fromUtf8(obs_source_get_name(source));
		}
		const QString modelState = sourceListSignature();
		if (before != signature || *lastSourceListSignature != modelState) {
			*lastSourceListSignature = modelState;
			refreshList();
			updateSelection();
		}
	});
	watchScene->start();
	// Persist changes made through the native OBS source Properties dialog,
	// even if the layer position was not moved afterwards.
	auto *saveSources = new QTimer(side);
	saveSources->setInterval(3500);
	QObject::connect(saveSources, &QTimer::timeout, side, [this] { save(); });
	saveSources->start();
	refreshList();
	*lastSourceListSignature = sourceListSignature();
	updateSelection();
	previewState.visible.store(true, std::memory_order_release);
	if (auto *main = OBSBasic::Get()) main->RefreshXinkxPreviewLayout();
}
void PortraitCanvas::shutdown()
{
	if (auto *main = OBSBasic::Get())
		if (auto *preview = main->findChild<OBSBasicPreview *>(QStringLiteral("preview")))
			preview->SetPortraitEditing(false);
	previewState.visible.store(false, std::memory_order_release);
	if (scene) save();
	// Stop display callbacks and scene-switch timers before releasing the libobs canvas.
	if (auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("xinkxLandscapePane")))
			landscape->show();
		else if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("previewContainer")))
			landscape->show();
		if (auto *editor = window->findChild<QWidget *>(QStringLiteral("xinkxPortraitEditor")))
			delete editor;
	}
	obs_canvas_t *oldCanvas = nullptr;
	{
		// Do not wait for OBS graphics operations while holding this lock.
		std::lock_guard<std::mutex> guard(previewSourceMutex);
		oldCanvas = canvas;
		canvas = nullptr;
		scene = nullptr;
	}
	if (oldCanvas) {
		obs_canvas_set_channel(oldCanvas, 0, nullptr);
		obs_canvas_remove(oldCanvas);
		obs_canvas_release(oldCanvas);
	}
	layers.clear();
	signature.clear();
	sceneName.clear();
	editing = false;
}

void PortraitCanvas::setViewMode(int mode)
{
	Q_UNUSED(mode);
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!window) return;
	if (!window->findChild<QWidget *>(QStringLiteral("xinkxPortraitEditor")))
		edit(window);
	if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("xinkxLandscapePane")))
		landscape->show();
	else if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("previewContainer")))
		landscape->show();
	// The hidden editor object owns input routing and the native SourceTree.
	// Rendering uses the existing OBS preview widget, not this object.
}

void PortraitCanvas::closeEditor()
{
	if (auto *main = OBSBasic::Get())
		if (auto *preview = main->findChild<OBSBasicPreview *>(QStringLiteral("preview")))
			preview->SetPortraitEditing(false);
	previewState.visible.store(false, std::memory_order_release);
	if (auto *main = OBSBasic::Get()) main->RefreshXinkxPreviewLayout();
	if (auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("xinkxLandscapePane")))
			landscape->show();
		else if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("previewContainer")))
			landscape->show();
		if (auto *editor = window->findChild<QWidget *>(QStringLiteral("xinkxPortraitEditor"))) {
			save();
			editor->deleteLater();
		}
	}
}

} // namespace xinkx
