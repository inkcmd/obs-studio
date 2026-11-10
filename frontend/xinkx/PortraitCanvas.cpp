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
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
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
	// -1: no layer; 0: move; 1..8: OBS-style corner/edge resize handles.
	std::function<int(float, float)> pressed;
	void setSelection(float x, float y, float w, float h)
	{
		selectionX.store(x);
		selectionY.store(y);
		selectionW.store(w);
		selectionH.store(h);
		hasSelection.store(w > 1 && h > 1);
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			start = event->position();
			handle = -1;
			const float scale = std::min(width() / float(portraitWidth), height() / float(portraitHeight));
			if (scale > 0 && pressed) {
				const float x = (width() - float(portraitWidth) * scale) / 2.f;
				const float y = (height() - float(portraitHeight) * scale) / 2.f;
				handle = pressed(float((start.x() - x) / scale), float((start.y() - y) / scale));
			}
		}
		OBSQTDisplay::mousePressEvent(event);
	}
	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (handle >= 0 && width() && height()) {
			const QPointF delta = event->position() - start;
			start = event->position();
			// Preview dimensions preserve 9:16; convert pixels to 1080x1920 coordinates.
			const float scale = std::min(width() / float(portraitWidth), height() / float(portraitHeight));
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
		if (event->button() == Qt::LeftButton) handle = -1;
		OBSQTDisplay::mouseReleaseEvent(event);
	}

private:
	obs_source_t *sceneSource = nullptr;
	QPointF start;
	int handle = -1;
	std::atomic<float> selectionX{0}, selectionY{0}, selectionW{0}, selectionH{0};
	std::atomic<bool> hasSelection{false};
	static void solidRect(float x, float y, float w, float h)
	{
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0);
		gs_draw_quadf(nullptr, 0, w, h);
		gs_matrix_pop();
	}
	static void render(void *context, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<PortraitDisplay *>(context);
		if (!self->sceneSource || !cx || !cy) return;
		int x, y;
		float scale;
		GetScaleAndCenterPos(portraitWidth, portraitHeight, cx, cy, x, y, scale);
		const int w = int(scale * portraitWidth);
		const int h = int(scale * portraitHeight);
		gs_projection_push();
		gs_viewport_push();
		gs_set_viewport(x, y, w, h);
		gs_ortho(0.f, float(portraitWidth), 0.f, float(portraitHeight), -100.f, 100.f);
		obs_source_video_render(self->sceneSource);
		if (self->hasSelection.load()) {
			const float bx = self->selectionX.load(), by = self->selectionY.load();
			const float bw = self->selectionW.load(), bh = self->selectionH.load();
			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
			vec4 color{};
			vec4_set(&color, 0.17f, 0.72f, 1.0f, 1.f);
			gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
			while (gs_effect_loop(effect, "Solid")) {
				const float line = 4.f, radius = 9.f;
				solidRect(bx, by, bw, line);
				solidRect(bx, by + bh - line, bw, line);
				solidRect(bx, by, line, bh);
				solidRect(bx + bw - line, by, line, bh);
				for (int ax = 0; ax < 3; ++ax)
					for (int ay = 0; ay < 3; ++ay) {
						if (ax == 1 && ay == 1) continue;
						solidRect(bx + bw * ax / 2.f - radius, by + bh * ay / 2.f - radius,
							radius * 2, radius * 2);
					}
			}
		}
		gs_viewport_pop();
		gs_projection_pop();
	}
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
	std::vector<Captured> originals;
	obs_scene_enum_items(mainScene, captureItem, &originals);
	QSettings store(studioSettingsPath(), QSettings::IniFormat);
	const QJsonArray extras = QJsonDocument::fromJson(store.value(extraKey(name)).toByteArray()).array();
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
		captured.info.bounds_type = OBS_BOUNDS_SCALE_INNER;
		captured.info.bounds = {float(portraitWidth) * .75f, float(portraitHeight) * .40f};
		captured.info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		captured.info.bounds_alignment = OBS_ALIGN_CENTER;
		captured.info.scale = {1.f, 1.f};
		originals.push_back(std::move(captured));
		obs_source_release(source);
	}
	std::vector<obs_sceneitem_t *> old;
	obs_scene_enum_items(scene, collectItemRefs, &old);
	for (auto *item : old) { obs_sceneitem_remove(item); obs_sceneitem_release(item); }
	layers.clear();
	obs_video_info base{};
	if (!obs_get_video_info(&base) || !base.base_width || !base.base_height) return;
	const float ratioX = float(portraitWidth) / base.base_width;
	const float ratioY = float(portraitHeight) / base.base_height;
	const QJsonObject saved = QJsonDocument::fromJson(store.value(storeKey(name)).toByteArray()).object();
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
		}
		obs_sceneitem_set_info2(item, &info);
		obs_sceneitem_set_crop(item, &source.crop);
		obs_sceneitem_set_visible(item, savedLayer.value(QStringLiteral("visible")).toBool(source.visible));
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
	for (const auto &layer : std::as_const(layers)) {
		obs_transform_info info{};
		obs_sceneitem_get_info2(layer.item, &info);
		objects.insert(layer.key, QJsonObject{
			{QStringLiteral("x"), info.pos.x}, {QStringLiteral("y"), info.pos.y},
			{QStringLiteral("w"), info.bounds.x}, {QStringLiteral("h"), info.bounds.y},
			{QStringLiteral("fill"), info.bounds_type == OBS_BOUNDS_SCALE_OUTER},
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
	side->setMinimumWidth(245);
	auto *column = new QVBoxLayout(side);
	column->setContentsMargins(3, 3, 3, 3);
	auto *header = new QHBoxLayout();
	header->addWidget(new QLabel(QStringLiteral("XINKX · Вертикальный холст 9:16"), side));
	auto *close = new QPushButton(QStringLiteral("×"), side);
	close->setToolTip(QStringLiteral("Скрыть вертикальный холст"));
	close->setMaximumWidth(30);
	header->addWidget(close);
	column->addLayout(header);
	auto *preview = new PortraitDisplay(obs_scene_get_source(scene), side);
	column->addWidget(preview, 1);
	auto *toolbar = new QHBoxLayout();
	auto *add = new QPushButton(QStringLiteral("+ Источник"), side);
	auto *remove = new QPushButton(QStringLiteral("−"), side);
	auto *properties = new QPushButton(QStringLiteral("Свойства"), side);
	auto *snap = new QCheckBox(QStringLiteral("Сетка"), side);
	snap->setChecked(true);
	toolbar->addWidget(add);
	toolbar->addWidget(remove);
	toolbar->addWidget(properties);
	toolbar->addWidget(snap);
	column->addLayout(toolbar);
	auto *list = new QListWidget(side);
	list->setObjectName(QStringLiteral("xinkxPortraitSourceList"));
	list->setMinimumHeight(105);
	list->setMaximumHeight(175);
	column->addWidget(list);
	previewLayout->addWidget(side, 1);

	const auto refreshList = [=] {
		const int selected = list->currentRow();
		const QSignalBlocker blocked(list);
		list->clear();
		for (const auto &layer : std::as_const(layers)) {
			auto *entry = new QListWidgetItem(layer.name, list);
			entry->setFlags(entry->flags() | Qt::ItemIsUserCheckable);
			entry->setCheckState(obs_sceneitem_visible(layer.item) ? Qt::Checked : Qt::Unchecked);
		}
		if (selected >= 0 && selected < layers.size()) list->setCurrentRow(selected);
		else if (!layers.isEmpty()) list->setCurrentRow(0);
	};
	const auto updateSelection = [=] {
		const int row = list->currentRow();
		if (row < 0 || row >= layers.size()) {
			preview->setSelection(0, 0, 0, 0);
			return;
		}
		matrix4 box{};
		obs_sceneitem_get_box_transform(layers.at(row).item, &box);
		float left = float(portraitWidth), top = float(portraitHeight);
		float right = 0, bottom = 0;
		for (int index = 0; index < 4; ++index) {
			vec3 unit{}, point{};
			vec3_set(&unit, index & 1 ? 1.f : 0.f, index & 2 ? 1.f : 0.f, 0.f);
			vec3_transform(&point, &unit, &box);
			left = std::min(left, point.x); top = std::min(top, point.y);
			right = std::max(right, point.x); bottom = std::max(bottom, point.y);
		}
		preview->setSelection(left, top, right - left, bottom - top);
	};
	const auto saveCurrent = [=](float px, float py, float width, float height) {
		const int row = list->currentRow();
		if (row < 0 || row >= layers.size()) return;
		obs_transform_info info{};
		obs_sceneitem_get_info2(layers.at(row).item, &info);
		const auto align = [=](float value) {
			return snap->isChecked() ? std::round(value / 10.f) * 10.f : value;
		};
		updateLayer(layers.at(row).key, align(px), align(py), std::max(32.f, align(width)),
			std::max(32.f, align(height)), info.bounds_type == OBS_BOUNDS_SCALE_OUTER);
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
	preview->pressed = [=](float px, float py) {
		const float scale = std::min(preview->width() / float(portraitWidth),
			preview->height() / float(portraitHeight));
		const float distance = 13.f / std::max(scale, .05f);
		for (int row = int(layers.size()) - 1; row >= 0; --row) {
			if (!obs_sceneitem_visible(layers.at(row).item)) continue;
			float left, top, right, bottom;
			bounds(row, &left, &top, &right, &bottom);
			const float cx = (left + right) / 2.f, cy = (top + bottom) / 2.f;
			const std::array<std::pair<float, float>, 8> handles = {{
				{left, top}, {cx, top}, {right, top}, {right, cy},
				{right, bottom}, {cx, bottom}, {left, bottom}, {left, cy},
			}};
			for (int i = 0; i < int(handles.size()); ++i) {
				if (std::hypot(px - handles[i].first, py - handles[i].second) <= distance) {
					list->setCurrentRow(row);
					return i + 1;
				}
			}
			if (px >= left && px <= right && py >= top && py <= bottom) {
				list->setCurrentRow(row);
				return 0;
			}
		}
		list->setCurrentRow(-1);
		return -1;
	};
	preview->moved = [=](float dx, float dy) {
		const int row = list->currentRow();
		if (row < 0 || row >= layers.size()) return;
		obs_transform_info info{};
		obs_sceneitem_get_info2(layers.at(row).item, &info);
		saveCurrent(info.pos.x + dx, info.pos.y + dy, info.bounds.x, info.bounds.y);
	};
	preview->resized = [=](int handle, float dx, float dy) {
		const int row = list->currentRow();
		if (row < 0 || row >= layers.size()) return;
		obs_transform_info info{};
		obs_sceneitem_get_info2(layers.at(row).item, &info);
		const float ax = info.alignment & OBS_ALIGN_RIGHT ? 1.f :
			info.alignment & OBS_ALIGN_LEFT ? 0.f : .5f;
		const float ay = info.alignment & OBS_ALIGN_BOTTOM ? 1.f :
			info.alignment & OBS_ALIGN_TOP ? 0.f : .5f;
		float left = info.pos.x - info.bounds.x * ax, top = info.pos.y - info.bounds.y * ay;
		float width = info.bounds.x, height = info.bounds.y;
		const bool moveLeft = handle == 1 || handle == 7 || handle == 8;
		const bool moveRight = handle == 3 || handle == 4 || handle == 5;
		const bool moveTop = handle == 1 || handle == 2 || handle == 3;
		const bool moveBottom = handle == 5 || handle == 6 || handle == 7;
		if (moveLeft && width - dx >= 32.f) { left += dx; width -= dx; }
		if (moveRight) width = std::max(32.f, width + dx);
		if (moveTop && height - dy >= 32.f) { top += dy; height -= dy; }
		if (moveBottom) height = std::max(32.f, height + dy);
		saveCurrent(left + width * ax, top + height * ay, width, height);
	};
	QObject::connect(list, &QListWidget::currentRowChanged, side, updateSelection);
	QObject::connect(list, &QListWidget::itemChanged, side, [=](QListWidgetItem *entry) {
		const int row = list->row(entry);
		if (row < 0 || row >= layers.size()) return;
		obs_sceneitem_set_visible(layers.at(row).item, entry->checkState() == Qt::Checked);
		save();
		updateSelection();
	});
	QObject::connect(properties, &QPushButton::clicked, side, [=] {
		const int row = list->currentRow();
		if (row >= 0 && row < layers.size())
			obs_frontend_open_source_properties(obs_sceneitem_get_source(layers.at(row).item));
	});
	QObject::connect(remove, &QPushButton::clicked, side, [=] {
		const int row = list->currentRow();
		if (row < 0 || row >= layers.size()) return;
		const auto layer = layers.at(row);
		if (!layer.key.startsWith(QStringLiteral("portrait:"))) {
			obs_sceneitem_set_visible(layer.item, false);
			save();
		} else {
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
		}
		refreshList();
		updateSelection();
	});
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
				if (!source) return;
				obs_sceneitem_t *item = obs_scene_add(scene, source);
				if (!item) { obs_source_release(source); return; }
				obs_transform_info info{};
				info.pos = {100.f, 500.f};
				info.bounds = {800.f, 500.f};
				info.scale = {1.f, 1.f};
				info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
				info.bounds_alignment = OBS_ALIGN_CENTER;
				info.bounds_type = OBS_BOUNDS_SCALE_INNER;
				obs_sceneitem_set_info2(item, &info);
				const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
				layers.push_back({QStringLiteral("portrait:%1").arg(uuid), name, item});
				QSettings store(studioSettingsPath(), QSettings::IniFormat);
				auto extras = QJsonDocument::fromJson(store.value(extraKey(sceneName)).toByteArray()).array();
				OBSDataAutoRelease saved = obs_save_source(source);
				const char *json = obs_data_get_json(saved);
				extras.append(QJsonObject{{QStringLiteral("uuid"), uuid},
					{QStringLiteral("source"), QJsonDocument::fromJson(QByteArray(json ? json : "{}")).object()}});
				store.setValue(extraKey(sceneName), QJsonDocument(extras).toJson(QJsonDocument::Compact));
				obs_source_release(source);
				save();
				refreshList();
				list->setCurrentRow(layers.size() - 1);
			});
		}
		menu.addSeparator();
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
			if (exists) continue;
			auto *action = menu.addAction(candidate.name);
			QObject::connect(action, &QAction::triggered, side, [=] {
				OBSSourceAutoRelease source = obs_get_source_by_uuid(candidate.uuid.toUtf8().constData());
				if (!source) return;
				obs_sceneitem_t *item = obs_scene_add(scene, source);
				if (!item) return;
				obs_transform_info info{};
				info.pos = {135.f, 480.f};
				info.bounds = {810.f, 960.f};
				info.scale = {1.f, 1.f};
				info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
				info.bounds_alignment = OBS_ALIGN_CENTER;
				info.bounds_type = OBS_BOUNDS_SCALE_INNER;
				obs_sceneitem_set_info2(item, &info);
				layers.push_back({QStringLiteral("portrait:%1").arg(candidate.uuid), candidate.name, item});
				QSettings settings(studioSettingsPath(), QSettings::IniFormat);
				auto extras = QJsonDocument::fromJson(settings.value(extraKey(sceneName)).toByteArray()).array();
				extras.append(candidate.uuid);
				settings.setValue(extraKey(sceneName), QJsonDocument(extras).toJson(QJsonDocument::Compact));
				save();
				refreshList();
				list->setCurrentRow(layers.size() - 1);
			});
		}
		if (menu.isEmpty()) menu.addAction(QStringLiteral("Нет других источников OBS"))->setEnabled(false);
		menu.exec(add->mapToGlobal(QPoint(0, add->height())));
	});
	QObject::connect(close, &QPushButton::clicked, side, &QWidget::deleteLater);
	QObject::connect(side, &QObject::destroyed, mainWindow, [this] { editing = false; });
	auto *watchScene = new QTimer(side);
	watchScene->setInterval(600);
	QObject::connect(watchScene, &QTimer::timeout, side, [=] {
		const QString before = signature;
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

} // namespace xinkx
