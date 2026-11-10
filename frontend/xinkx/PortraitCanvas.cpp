#include "PortraitCanvas.hpp"

#include "StreamAccounts.hpp"
#include "StudioDiagnostics.hpp"

#include <utility/display-helpers.hpp>
#include <widgets/OBSQTDisplay.hpp>

#include <obs-frontend-api.h>
#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>

#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <functional>
#include <memory>
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

// This is a real libobs render of the portrait scene, not a scaled screenshot
// of the horizontal canvas. The output encoder uses the same canvas's video mix.
class PortraitDisplay : public OBSQTDisplay {
public:
	explicit PortraitDisplay(obs_source_t *source, QWidget *parent) : OBSQTDisplay(parent), sceneSource(source)
	{
		setMinimumSize(170, 260);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		setFocusPolicy(Qt::StrongFocus);
		connect(this, &OBSQTDisplay::DisplayCreated, this, [this] {
			obs_display_add_draw_callback(GetDisplay(), render, this);
		});
	}
	~PortraitDisplay() override
	{
		if (GetDisplay()) obs_display_remove_draw_callback(GetDisplay(), render, this);
	}
	std::function<void(float, float)> moved;
	std::function<void(int, float, float)> resized;
	std::function<void(float, float, const QPoint &)> contextRequested;
	std::function<void(float, float)> doubleClicked;
	std::function<int(float, float)> hovering;
	std::function<void(float, float)> nudge;
	std::function<void()> deleteRequested;
	std::function<void()> deselectRequested;
	// -1: no layer; 0: move; 1..8: OBS-style corner/edge resize handles.
	std::function<int(float, float)> pressed;
	void setSelection(const std::array<QPointF, 4> &corners)
	{
		for (int i = 0; i < 4; ++i) {
			selectionPoints[i * 2].store(float(corners[i].x()));
			selectionPoints[i * 2 + 1].store(float(corners[i].y()));
		}
		hasSelection.store(true);
	}
	void clearSelection() { hasSelection.store(false); }
	float zoomFactor() const { return zoom.load(); }
	void setZoomFactor(float factor)
	{
		zoom.store(std::clamp(factor, 1.f, 8.f));
		if (factor <= 1.f) { panX.store(0.f); panY.store(0.f); }
	}
	float displayScale() const
	{
		return std::min(width() / float(portraitWidth), height() / float(portraitHeight)) * zoomFactor();
	}

protected:
	QPointF scenePoint(const QPointF &point) const
	{
		const float scale = displayScale();
		if (scale <= 0.f) return QPointF(-1, -1);
		return {(point.x() - (width() - float(portraitWidth) * scale) / 2.f - panX.load()) / scale,
			(point.y() - (height() - float(portraitHeight) * scale) / 2.f - panY.load()) / scale};
	}
	void mousePressEvent(QMouseEvent *event) override
	{
		setFocus();
		if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && spaceDown)) {
			panning = true;
			start = event->position();
			panStart = {panX.load(), panY.load()};
			setCursor(Qt::ClosedHandCursor);
			event->accept();
			return;
		}
		if (event->button() == Qt::LeftButton) {
			start = event->position();
			handle = -1;
			const QPointF point = scenePoint(start);
			if (pressed) handle = pressed(float(point.x()), float(point.y()));
		} else if (event->button() == Qt::RightButton && contextRequested) {
			const QPointF point = scenePoint(event->position());
			contextRequested(float(point.x()), float(point.y()), event->globalPosition().toPoint());
		}
		OBSQTDisplay::mousePressEvent(event);
	}
	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && doubleClicked) {
			const QPointF point = scenePoint(event->position());
			doubleClicked(float(point.x()), float(point.y()));
		}
		OBSQTDisplay::mouseDoubleClickEvent(event);
	}
	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (panning) {
			const QPointF change = event->position() - start;
			panX.store(float(panStart.x() + change.x()));
			panY.store(float(panStart.y() + change.y()));
			event->accept();
			return;
		}
		if (handle < 0 && hovering) {
			const QPointF point = scenePoint(event->position());
			const int hovered = hovering(float(point.x()), float(point.y()));
			if (hovered == 1 || hovered == 5) setCursor(Qt::SizeFDiagCursor);
			else if (hovered == 3 || hovered == 7) setCursor(Qt::SizeBDiagCursor);
			else if (hovered == 2 || hovered == 6) setCursor(Qt::SizeVerCursor);
			else if (hovered == 4 || hovered == 8) setCursor(Qt::SizeHorCursor);
			else if (hovered == 0) setCursor(Qt::SizeAllCursor);
			else unsetCursor();
		}
		if (handle >= 0 && width() && height()) {
			const QPointF delta = event->position() - start;
			// Deltas are measured from mouse-down, like the native OBS editor.
			// A per-frame delta plus 10px snapping would otherwise discard small drags.
			const float scale = displayScale();
			if (scale > 0) {
				const float dx = float(delta.x() / scale), dy = float(delta.y() / scale);
				if (handle > 0 && resized) resized(handle, dx, dy);
				else if (moved) moved(dx, dy);
			}
		}
		OBSQTDisplay::mouseMoveEvent(event);
	}
	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (panning && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
			panning = false;
			setCursor(spaceDown ? Qt::OpenHandCursor : Qt::ArrowCursor);
			event->accept();
			return;
		}
		if (event->button() == Qt::LeftButton) handle = -1;
		OBSQTDisplay::mouseReleaseEvent(event);
	}
	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
			if (deleteRequested) deleteRequested();
			event->accept();
			return;
		}
		if (event->key() == Qt::Key_Escape) {
			if (deselectRequested) deselectRequested();
			event->accept();
			return;
		}
		float horizontal = 0.f, vertical = 0.f;
		const float step = event->modifiers() & Qt::ShiftModifier ? 10.f : 1.f;
		if (event->key() == Qt::Key_Left) horizontal = -step;
		else if (event->key() == Qt::Key_Right) horizontal = step;
		else if (event->key() == Qt::Key_Up) vertical = -step;
		else if (event->key() == Qt::Key_Down) vertical = step;
		if ((horizontal != 0.f || vertical != 0.f) && nudge) {
			nudge(horizontal, vertical);
			event->accept();
			return;
		}
		if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
			spaceDown = true;
			setCursor(Qt::OpenHandCursor);
			event->accept();
			return;
		}
		OBSQTDisplay::keyPressEvent(event);
	}
	void keyReleaseEvent(QKeyEvent *event) override
	{
		if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
			spaceDown = false;
			unsetCursor();
			event->accept();
			return;
		}
		OBSQTDisplay::keyReleaseEvent(event);
	}
	void wheelEvent(QWheelEvent *event) override
	{
		if (spaceDown || (event->modifiers() & Qt::ControlModifier)) {
			setZoomFactor(zoomFactor() * (event->angleDelta().y() > 0 ? 1.25f : .8f));
			if (zoomChanged) zoomChanged();
			event->accept();
			return;
		}
		OBSQTDisplay::wheelEvent(event);
	}

public:
	std::function<void()> zoomChanged;

private:
	obs_source_t *sceneSource = nullptr;
	QPointF start;
	QPointF panStart;
	int handle = -1;
	bool spaceDown = false;
	bool panning = false;
	std::atomic<float> zoom{1.f}, panX{0.f}, panY{0.f};
	std::array<std::atomic<float>, 8> selectionPoints{};
	std::atomic<bool> hasSelection{false};
	static void solidRect(float x, float y, float w, float h)
	{
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0);
		gs_draw_quadf(nullptr, 0, w, h);
		gs_matrix_pop();
	}
	static void solidLine(float x1, float y1, float x2, float y2, float thickness)
	{
		gs_matrix_push();
		gs_matrix_translate3f(x1, y1, 0);
		gs_matrix_rotaa4f(0, 0, 1, std::atan2(y2 - y1, x2 - x1));
		gs_draw_quadf(nullptr, 0, std::hypot(x2 - x1, y2 - y1), thickness);
		gs_matrix_pop();
	}
	static void render(void *context, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<PortraitDisplay *>(context);
		if (!self->sceneSource || !cx || !cy) return;
		int x, y;
		float scale;
		GetScaleAndCenterPos(portraitWidth, portraitHeight, cx, cy, x, y, scale);
		scale *= self->zoomFactor();
		x = int((float(cx) - float(portraitWidth) * scale) / 2.f + self->panX.load());
		y = int((float(cy) - float(portraitHeight) * scale) / 2.f + self->panY.load());
		const int w = int(scale * portraitWidth);
		const int h = int(scale * portraitHeight);
		gs_projection_push();
		gs_viewport_push();
		// Draw an explicit canvas edge, even when every source is transparent or
		// black. This guide lives only in the editor display, never in the output.
		gs_set_viewport(0, 0, cx, cy);
		gs_ortho(0.f, float(cx), 0.f, float(cy), -100.f, 100.f);
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		vec4 outline{};
		vec4_set(&outline, .38f, .49f, .60f, 1.f);
		gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &outline);
		while (gs_effect_loop(effect, "Solid")) solidRect(float(x - 3), float(y - 3), float(w + 6), float(h + 6));
		gs_set_viewport(x, y, w, h);
		gs_ortho(0.f, float(portraitWidth), 0.f, float(portraitHeight), -100.f, 100.f);
		obs_source_video_render(self->sceneSource);
		// These four visible inside edges are editor-only and cannot be cropped
		// by a source covering the entire output video.
		vec4 guide{};
		vec4_set(&guide, .48f, .64f, .81f, 1.f);
		gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &guide);
		while (gs_effect_loop(effect, "Solid")) {
			const float thickness = 5.f;
			solidRect(0.f, 0.f, float(portraitWidth), thickness);
			solidRect(0.f, float(portraitHeight) - thickness, float(portraitWidth), thickness);
			solidRect(0.f, 0.f, thickness, float(portraitHeight));
			solidRect(float(portraitWidth) - thickness, 0.f, thickness, float(portraitHeight));
		}
		if (self->hasSelection.load()) {
			std::array<QPointF, 4> corners;
			for (int i = 0; i < 4; ++i)
				corners[i] = {self->selectionPoints[i * 2].load(), self->selectionPoints[i * 2 + 1].load()};
			vec4 color{};
			vec4_set(&color, 0.17f, 0.72f, 1.0f, 1.f);
			gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
			while (gs_effect_loop(effect, "Solid")) {
				const float line = 4.f, radius = 9.f;
				for (int i = 0; i < 4; ++i) {
					const QPointF &a = corners[i], &b = corners[(i + 1) % 4];
					solidLine(float(a.x()), float(a.y()), float(b.x()), float(b.y()), line);
					for (const float t : {0.f, .5f}) {
						const float px = float(a.x() + (b.x() - a.x()) * t);
						const float py = float(a.y() + (b.y() - a.y()) * t);
						solidRect(px - radius, py - radius, radius * 2, radius * 2);
					}
				}
			}
		}
		gs_viewport_pop();
		gs_projection_pop();
	}
};

class LandscapeTabFocus final : public QObject {
public:
	LandscapeTabFocus(QTabWidget *tabs, QObject *parent) : QObject(parent), tabs(tabs) {}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (tabs && event->type() == QEvent::MouseButtonPress) tabs->setCurrentIndex(0);
		return QObject::eventFilter(watched, event);
	}

private:
	QPointer<QTabWidget> tabs;
};

} // namespace

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
	if (sceneName.isEmpty()) return;
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
	for (int index = 0; index < extras.size(); ++index) {
		const QJsonObject record = extras.at(index).toObject();
		if (!record.contains(QStringLiteral("source"))) continue;
		const QString uuid = record.value(QStringLiteral("uuid")).toString();
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toUtf8().constData());
		if (!source) continue;
		OBSDataAutoRelease sourceData = obs_save_source(source);
		const char *json = obs_data_get_json(sourceData);
		QJsonObject copy = record;
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
		existing->show();
		existing->raise();
		return;
	}
	if (!ensure()) return;
	sync();
	auto *canvasEditor = mainWindow->findChild<QWidget *>(QStringLiteral("canvasEditor"));
	auto *previewLayout = canvasEditor ? static_cast<QHBoxLayout *>(canvasEditor->layout()) : nullptr;
	if (!previewLayout) {
		writeDiagnostic(QStringLiteral("portrait"), QStringLiteral("Main OBS canvas layout unavailable"));
		return;
	}
	editing = true;
	auto *side = new QWidget(canvasEditor);
	side->setObjectName(QStringLiteral("xinkxPortraitEditor"));
	side->setMinimumWidth(200);
	side->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	auto *column = new QVBoxLayout(side);
	column->setContentsMargins(3, 3, 3, 3);
	auto *header = new QHBoxLayout();
	header->addWidget(new QLabel(QStringLiteral("9:16 · 1080×1920 · текущая сцена OBS"), side));
	column->addLayout(header);
	auto *preview = new PortraitDisplay(obs_scene_get_source(scene), side);
	column->addWidget(preview, 1);
	auto *zoomBar = new QHBoxLayout();
	auto *zoomOut = new QPushButton(QStringLiteral("−"), side);
	auto *zoomIn = new QPushButton(QStringLiteral("+"), side);
	auto *zoomText = new QLabel(QStringLiteral("По размеру окна"), side);
	auto *zoomFit = new QPushButton(QStringLiteral("По размеру"), side);
	zoomOut->setMaximumWidth(30);
	zoomIn->setMaximumWidth(30);
	zoomBar->addWidget(zoomOut);
	zoomBar->addWidget(zoomText);
	zoomBar->addWidget(zoomIn);
	zoomBar->addStretch();
	zoomBar->addWidget(zoomFit);
	column->addLayout(zoomBar);
	const auto updateZoom = [=] {
		zoomText->setText(preview->zoomFactor() <= 1.f ? QStringLiteral("По размеру окна") :
			QStringLiteral("%1%").arg(int(std::round(preview->zoomFactor() * 100.f))));
	};
	preview->zoomChanged = updateZoom;
	QObject::connect(zoomOut, &QPushButton::clicked, side, [=] {
		preview->setZoomFactor(preview->zoomFactor() / 1.25f); updateZoom();
	});
	QObject::connect(zoomIn, &QPushButton::clicked, side, [=] {
		preview->setZoomFactor(preview->zoomFactor() * 1.25f); updateZoom();
	});
	QObject::connect(zoomFit, &QPushButton::clicked, side, [=] {
		preview->setZoomFactor(1.f); updateZoom();
	});
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
	column->addWidget(toolbarWidget);
	auto *list = new QListWidget(side);
	list->setObjectName(QStringLiteral("xinkxPortraitSourceList"));
	list->setMinimumHeight(105);
	list->setMaximumHeight(175);
	column->addWidget(list);
	// Reuse OBS's existing Sources dock: its source tree and controls become the
	// 16:9 tab, while portrait has its own source tree/controls in the 9:16 tab.
	// The central area therefore holds previews only, not a second settings pane.
	QTabWidget *sourceTabs = nullptr;
	if (auto *sourcesDock = mainWindow->findChild<QDockWidget *>(QStringLiteral("sourcesDock"))) {
		if (auto *nativeFrame = sourcesDock->findChild<QWidget *>(QStringLiteral("sourcesFrame"))) {
			QWidget *contents = nativeFrame->parentWidget();
			auto *dockLayout = contents ? static_cast<QVBoxLayout *>(contents->layout()) : nullptr;
			if (dockLayout) {
				sourceTabs = new QTabWidget(contents);
				sourceTabs->setObjectName(QStringLiteral("xinkxSourceCanvasTabs"));
				auto *portraitSources = new QWidget(sourceTabs);
				auto *sourceColumn = new QVBoxLayout(portraitSources);
				sourceColumn->setContentsMargins(0, 0, 0, 0);
				column->removeWidget(list);
				column->removeWidget(toolbarWidget);
				list->setMaximumHeight(QWIDGETSIZE_MAX);
				sourceColumn->addWidget(list, 1);
				sourceColumn->addWidget(toolbarWidget);
				dockLayout->removeWidget(nativeFrame);
				sourceTabs->addTab(nativeFrame, QStringLiteral("16:9"));
				sourceTabs->addTab(portraitSources, QStringLiteral("9:16"));
				dockLayout->insertWidget(0, sourceTabs);
				sourceTabs->setCurrentIndex(1);
				if (auto *nativePreview = mainWindow->findChild<QWidget *>(QStringLiteral("preview")))
					nativePreview->installEventFilter(new LandscapeTabFocus(sourceTabs, side));
				QPointer<QWidget> guardedContents(contents);
				QPointer<QWidget> guardedFrame(nativeFrame);
				QPointer<QTabWidget> guardedTabs(sourceTabs);
				QObject::connect(side, &QObject::destroyed, sourceTabs, [=] {
					if (!guardedContents || !guardedFrame || !guardedTabs) return;
					auto *layout = static_cast<QVBoxLayout *>(guardedContents->layout());
					if (!layout) return;
					layout->removeWidget(guardedTabs);
					guardedTabs->removeTab(0);
					guardedFrame->setParent(guardedContents);
					layout->insertWidget(0, guardedFrame);
					guardedTabs->deleteLater();
				});
			}
		}
	}
	previewLayout->addWidget(side);
	// Dragging changes only this scene item's transform. Do not write an INI file
	// on every mouse-move; persist once the user stops interacting.
	auto *saveDelay = new QTimer(side);
	saveDelay->setSingleShot(true);
	saveDelay->setInterval(350);
	QObject::connect(saveDelay, &QTimer::timeout, side, [this] { save(); });
	auto initialTransform = std::make_shared<obs_transform_info>();
	auto initialBounds = std::make_shared<std::array<float, 4>>();
	// OBS's Sources dock is topmost-first; libobs stores scene items
	// bottommost-first. Keep the same ordering in the portrait tab.
	const auto selectedLayer = [=] {
		return list->currentRow() < 0 ? -1 : layers.size() - 1 - list->currentRow();
	};
	const auto selectLayer = [=](int layer) {
		list->setCurrentRow(layer < 0 ? -1 : layers.size() - 1 - layer);
	};

	const auto refreshList = [=] {
		const int selected = selectedLayer();
		const QSignalBlocker blocked(list);
		list->clear();
		for (int i = layers.size() - 1; i >= 0; --i) {
			const auto &layer = layers.at(i);
			auto *entry = new QListWidgetItem(layer.name, list);
			entry->setFlags(entry->flags() | Qt::ItemIsUserCheckable);
			entry->setCheckState(obs_sceneitem_visible(layer.item) ? Qt::Checked : Qt::Unchecked);
		}
		if (selected >= 0 && selected < layers.size()) selectLayer(selected);
		else if (!layers.isEmpty()) selectLayer(layers.size() - 1);
	};
	const auto updateSelection = [=] {
		const int row = selectedLayer();
		if (row < 0 || row >= layers.size() || !obs_sceneitem_visible(layers.at(row).item) ||
		    obs_sceneitem_locked(layers.at(row).item)) {
			preview->clearSelection();
			return;
		}
		matrix4 box{};
		obs_sceneitem_get_box_transform(layers.at(row).item, &box);
		std::array<QPointF, 4> corners;
		for (int index = 0; index < 4; ++index) {
			vec3 unit{}, point{};
			const int x = index == 1 || index == 2 ? 1 : 0;
			const int y = index >= 2 ? 1 : 0;
			vec3_set(&unit, float(x), float(y), 0.f);
			vec3_transform(&point, &unit, &box);
			corners[index] = QPointF(point.x, point.y);
		}
		preview->setSelection(corners);
	};
	const auto saveCurrent = [=](float px, float py, float width, float height) {
		const int row = selectedLayer();
		if (row < 0 || row >= layers.size()) return;
		obs_transform_info info{};
		obs_sceneitem_get_info2(layers.at(row).item, &info);
		info.pos = {px, py};
		info.bounds = {std::max(32.f, width), std::max(32.f, height)};
		obs_sceneitem_set_info2(layers.at(row).item, &info);
		saveDelay->start();
		updateSelection();
	};
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
	// OBS-style snapping: canvas edges, canvas centre and other layer edges.
	// Threshold is in displayed pixels, so it stays usable at every preview size.
	const auto snapAxis = [=](float first, float second, bool horizontal) {
		if (!snap->isChecked()) return 0.f;
		const float scale = preview->displayScale();
		const float distance = 9.f / std::max(scale, .05f);
		const float limit = horizontal ? float(portraitWidth) : float(portraitHeight);
		std::vector<float> targets{0.f, limit, limit / 2.f};
		for (int row = 0; row < layers.size(); ++row) {
			if (row == selectedLayer() || !obs_sceneitem_visible(layers.at(row).item)) continue;
			float left, top, right, bottom;
			bounds(row, &left, &top, &right, &bottom);
			const float a = horizontal ? left : top, b = horizontal ? right : bottom;
			targets.push_back(a); targets.push_back(b); targets.push_back((a + b) / 2.f);
		}
		float offset = 0.f, best = distance;
		for (const float edge : {first, second, (first + second) / 2.f}) {
			for (const float target : targets) {
				const float difference = target - edge;
				if (std::abs(difference) < best) { best = std::abs(difference); offset = difference; }
			}
		}
		return offset;
	};
	// Use OBS's inverse box transform for hit testing. Axis-aligned bounding
	// rectangles select empty corners of rotated or cropped source items.
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
	const auto handleAt = [=](int row, float px, float py) {
		if (row < 0 || row >= layers.size()) return -1;
		if (!obs_sceneitem_visible(layers.at(row).item) || obs_sceneitem_locked(layers.at(row).item))
			return -1;
		const float scale = preview->displayScale();
		const float distance = 12.f / std::max(scale, .05f);
		matrix4 box{};
		obs_sceneitem_get_box_transform(layers.at(row).item, &box);
		const std::array<std::pair<float, float>, 8> units = {{
			{0.f, 0.f}, {.5f, 0.f}, {1.f, 0.f}, {1.f, .5f},
			{1.f, 1.f}, {.5f, 1.f}, {0.f, 1.f}, {0.f, .5f},
		}};
		for (int i = 0; i < int(units.size()); ++i) {
			vec3 unit{}, target{};
			vec3_set(&unit, units[i].first, units[i].second, 0.f);
			vec3_transform(&target, &unit, &box);
			if (std::hypot(px - target.x, py - target.y) <= distance) return i + 1;
		}
		return -1;
	};
	preview->hovering = [=](float px, float py) {
		const int selected = selectedLayer();
		const int hitHandle = handleAt(selected, px, py);
		if (hitHandle > 0) return hitHandle;
		if (insideItem(selected, px, py)) return 0;
		for (int i = layers.size() - 1; i >= 0; --i)
			if (insideItem(i, px, py)) return 0;
		return -1;
	};
	preview->pressed = [=](float px, float py) {
		if (sourceTabs) sourceTabs->setCurrentIndex(1);
		// In OBS, a selected handle wins even if a different, higher scene
		// item overlaps it. Only then pick the topmost item's contents.
		const int selectedRow = selectedLayer();
		const int selectedHandle = handleAt(selectedRow, px, py);
		if (selectedHandle > 0) {
			obs_sceneitem_get_info2(layers.at(selectedRow).item, initialTransform.get());
			bounds(selectedRow, &(*initialBounds)[0], &(*initialBounds)[1],
				&(*initialBounds)[2], &(*initialBounds)[3]);
			return selectedHandle;
		}
		for (int row = int(layers.size()) - 1; row >= 0; --row) {
			if (!insideItem(row, px, py)) continue;
			selectLayer(row);
			obs_sceneitem_get_info2(layers.at(row).item, initialTransform.get());
			bounds(row, &(*initialBounds)[0], &(*initialBounds)[1],
				&(*initialBounds)[2], &(*initialBounds)[3]);
			return 0;
		}
		selectLayer(-1);
		return -1;
	};
	preview->moved = [=](float dx, float dy) {
		const int row = selectedLayer();
		if (row < 0 || row >= layers.size()) return;
		const auto &info = *initialTransform;
		float x = info.pos.x + dx, y = info.pos.y + dy;
		x += snapAxis((*initialBounds)[0] + dx, (*initialBounds)[2] + dx, true);
		y += snapAxis((*initialBounds)[1] + dy, (*initialBounds)[3] + dy, false);
		saveCurrent(x, y, info.bounds.x, info.bounds.y);
	};
	preview->resized = [=](int handle, float dx, float dy) {
		const int row = selectedLayer();
		if (row < 0 || row >= layers.size()) return;
		const auto &info = *initialTransform;
		const float ax = info.alignment & OBS_ALIGN_RIGHT ? 1.f :
			info.alignment & OBS_ALIGN_LEFT ? 0.f : .5f;
		const float ay = info.alignment & OBS_ALIGN_BOTTOM ? 1.f :
			info.alignment & OBS_ALIGN_TOP ? 0.f : .5f;
		const float radians = info.rot * 3.14159265358979323846f / 180.f;
		const float c = std::cos(radians), s = std::sin(radians);
		// Resize in the source's local axes, not the screen axes. A rotated
		// rectangle must stay under the handle being dragged.
		const float localDx = c * dx + s * dy;
		const float localDy = -s * dx + c * dy;
		float left = -info.bounds.x * ax, top = -info.bounds.y * ay;
		float width = info.bounds.x, height = info.bounds.y;
		const bool moveLeft = handle == 1 || handle == 7 || handle == 8;
		const bool moveRight = handle == 3 || handle == 4 || handle == 5;
		const bool moveTop = handle == 1 || handle == 2 || handle == 3;
		const bool moveBottom = handle == 5 || handle == 6 || handle == 7;
		if (moveLeft && width - localDx >= 32.f) { left += localDx; width -= localDx; }
		if (moveRight) width = std::max(32.f, width + localDx);
		if (moveTop && height - localDy >= 32.f) { top += localDy; height -= localDy; }
		if (moveBottom) height = std::max(32.f, height + localDy);
		if (std::abs(info.rot) < .01f && info.scale.x > 0 && info.scale.y > 0) {
			if (moveLeft) { const float offset = snapAxis(info.pos.x + left, info.pos.x + left, true); if (width - offset >= 32.f) { left += offset; width -= offset; } }
			if (moveRight) width = std::max(32.f, width + snapAxis(info.pos.x + left + width, info.pos.x + left + width, true));
			if (moveTop) { const float offset = snapAxis(info.pos.y + top, info.pos.y + top, false); if (height - offset >= 32.f) { top += offset; height -= offset; } }
			if (moveBottom) height = std::max(32.f, height + snapAxis(info.pos.y + top + height, info.pos.y + top + height, false));
		}
		const float anchorX = left + width * ax, anchorY = top + height * ay;
		saveCurrent(info.pos.x + c * anchorX - s * anchorY,
			info.pos.y + s * anchorX + c * anchorY, width, height);
	};
	QObject::connect(list, &QListWidget::currentRowChanged, side, updateSelection);
	QObject::connect(list, &QListWidget::itemChanged, side, [=](QListWidgetItem *entry) {
		const int row = layers.size() - 1 - list->row(entry);
		if (row < 0 || row >= layers.size()) return;
		obs_sceneitem_set_visible(layers.at(row).item, entry->checkState() == Qt::Checked);
		save();
		updateSelection();
	});
	QObject::connect(properties, &QPushButton::clicked, side, [=] {
		const int row = selectedLayer();
		if (row >= 0 && row < layers.size())
			obs_frontend_open_source_properties(obs_sceneitem_get_source(layers.at(row).item));
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
	preview->deselectRequested = [=] { selectLayer(-1); };
	preview->nudge = [=](float dx, float dy) {
		const int row = selectedLayer();
		if (row < 0 || row >= layers.size() || obs_sceneitem_locked(layers.at(row).item)) return;
		obs_transform_info info{};
		obs_sceneitem_get_info2(layers.at(row).item, &info);
		info.pos.x += dx;
		info.pos.y += dy;
		obs_sceneitem_set_info2(layers.at(row).item, &info);
		saveDelay->start();
		updateSelection();
	};
	const auto addSource = [=](obs_source_t *source, bool separateProperties) {
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
		if (separateProperties) {
			OBSDataAutoRelease snapshot = obs_save_source(source);
			const char *json = snapshot ? obs_data_get_json(snapshot) : nullptr;
			extras.append(QJsonObject{{QStringLiteral("uuid"), uuid},
				{QStringLiteral("source"), QJsonDocument::fromJson(QByteArray(json ? json : "{}")).object()}});
		} else {
			// The input is shared, but its scene-item geometry is independent.
			extras.append(uuid);
		}
		settings.setValue(extraKey(sceneName), QJsonDocument(extras).toJson(QJsonDocument::Compact));
		save();
		refreshList();
		selectLayer(layers.size() - 1);
	};
	QObject::connect(add, &QPushButton::clicked, side, [=] {
		QMenu menu(side);
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
		if (reuseMenu->isEmpty()) reuseMenu->setEnabled(false);
		if (duplicateMenu->isEmpty()) duplicateMenu->setEnabled(false);
		menu.exec(add->mapToGlobal(QPoint(0, add->height())));
	});
	// These commands target the portrait scene item directly. Calling OBSBasic's
	// source context menu here would silently operate on the horizontal scene.
	const auto showContext = [=](int row, const QPoint &globalPoint) {
		QMenu menu(side);
		if (row < 0 || row >= layers.size()) {
			auto *addAction = menu.addAction(QStringLiteral("Добавить источник…"));
			QObject::connect(addAction, &QAction::triggered, side, [=] { add->click(); });
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
		if (sourceTabs) sourceTabs->setCurrentIndex(1);
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
		const auto *entry = list->itemAt(position);
		const int row = entry ? layers.size() - 1 - list->row(entry) : -1;
		showContext(row, list->viewport()->mapToGlobal(position));
	});
	QObject::connect(side, &QObject::destroyed, mainWindow, [this] { editing = false; });
	auto *watchScene = new QTimer(side);
	watchScene->setInterval(600);
	QObject::connect(watchScene, &QTimer::timeout, side, [=] {
		const QString before = signature;
		if (saveDelay->isActive()) { saveDelay->stop(); save(); }
		editing = false;
		sync();
		editing = true;
		if (before != signature) { refreshList(); updateSelection(); }
	});
	watchScene->start();
	// Persist changes made through the native OBS source Properties dialog,
	// even if the layer position was not moved afterwards.
	auto *saveSources = new QTimer(side);
	saveSources->setInterval(3500);
	QObject::connect(saveSources, &QTimer::timeout, side, [this] { save(); });
	saveSources->start();
	refreshList();
	updateSelection();
	side->show();
}
void PortraitCanvas::shutdown()
{
	// Stop display callbacks and scene-switch timers before releasing the libobs canvas.
	if (auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("previewContainer")))
			landscape->show();
		if (auto *editor = window->findChild<QWidget *>(QStringLiteral("xinkxPortraitEditor")))
			delete editor;
	}
	if (canvas) {
		obs_canvas_set_channel(canvas, 0, nullptr);
		obs_canvas_remove(canvas);
		obs_canvas_release(canvas);
	}
	canvas = nullptr;
	scene = nullptr;
	layers.clear();
	signature.clear();
	sceneName.clear();
	editing = false;
}

void PortraitCanvas::setViewMode(int mode)
{
	mode = std::clamp(mode, 0, 2);
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!window) return;
	if (mode != 0 && !window->findChild<QWidget *>(QStringLiteral("xinkxPortraitEditor")))
		edit(window);
	if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("previewContainer")))
		landscape->setVisible(mode != 1);
	if (auto *portrait = window->findChild<QWidget *>(QStringLiteral("xinkxPortraitEditor")))
		portrait->setVisible(mode != 0);
	if (auto *tabs = window->findChild<QTabWidget *>(QStringLiteral("xinkxSourceCanvasTabs"))) {
		if (mode != 2) tabs->setCurrentIndex(mode == 0 ? 0 : 1);
	}
}

void PortraitCanvas::closeEditor()
{
	if (auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		if (auto *landscape = window->findChild<QWidget *>(QStringLiteral("previewContainer")))
			landscape->show();
		if (auto *editor = window->findChild<QWidget *>(QStringLiteral("xinkxPortraitEditor"))) {
			save();
			editor->deleteLater();
		}
	}
}

} // namespace xinkx
