#include "PortraitCanvas.hpp"

#include "StreamAccounts.hpp"
#include "StudioDiagnostics.hpp"

#include <utility/display-helpers.hpp>
#include <widgets/OBSQTDisplay.hpp>

#include <obs-frontend-api.h>
#include <obs.h>
#include <graphics/graphics.h>

#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
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
		setMinimumSize(235, 420);
		connect(this, &OBSQTDisplay::DisplayCreated, this, [this] {
			obs_display_add_draw_callback(GetDisplay(), render, this);
		});
	}
	~PortraitDisplay() override
	{
		if (GetDisplay()) obs_display_remove_draw_callback(GetDisplay(), render, this);
	}
	std::function<void(float, float)> moved;

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) { start = event->position(); dragging = true; }
		OBSQTDisplay::mousePressEvent(event);
	}
	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (dragging && moved && width() && height()) {
			const QPointF delta = event->position() - start;
			start = event->position();
			// Preview dimensions preserve 9:16; convert pixels to 1080x1920 coordinates.
			const float scale = std::min(width() / float(portraitWidth), height() / float(portraitHeight));
			if (scale > 0) moved(float(delta.x() / scale), float(delta.y() / scale));
		}
		OBSQTDisplay::mouseMoveEvent(event);
	}
	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) dragging = false;
		OBSQTDisplay::mouseReleaseEvent(event);
	}

private:
	obs_source_t *sceneSource = nullptr;
	QPointF start;
	bool dragging = false;
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
		gs_viewport_pop();
		gs_projection_pop();
	}
};

QDoubleSpinBox *dimension(QWidget *parent, int max, const QString &suffix)
{
	auto *spin = new QDoubleSpinBox(parent);
	spin->setRange(0, max);
	spin->setDecimals(0);
	spin->setSingleStep(10);
	spin->setSuffix(suffix);
	return spin;
}

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
	std::vector<obs_sceneitem_t *> old;
	obs_scene_enum_items(scene, collectItemRefs, &old);
	for (auto *item : old) { obs_sceneitem_remove(item); obs_sceneitem_release(item); }
	layers.clear();
	obs_video_info base{};
	if (!obs_get_video_info(&base) || !base.base_width || !base.base_height) return;
	const float ratioX = float(portraitWidth) / base.base_width;
	const float ratioY = float(portraitHeight) / base.base_height;
	QSettings store(studioSettingsPath(), QSettings::IniFormat);
	const QJsonObject saved = QJsonDocument::fromJson(store.value(storeKey(name)).toByteArray()).object();
	for (const auto &source : originals) {
		obs_sceneitem_t *item = obs_scene_add(scene, source.source);
		if (!item) continue;
		obs_transform_info info = source.info;
		float width = info.bounds_type == OBS_BOUNDS_NONE
			? obs_source_get_width(source.source) * std::abs(info.scale.x) : info.bounds.x;
		float height = info.bounds_type == OBS_BOUNDS_NONE
			? obs_source_get_height(source.source) * std::abs(info.scale.y) : info.bounds.y;
		const bool fullScreen = width >= base.base_width * .85f && height >= base.base_height * .85f;
		info.pos.x *= ratioX;
		info.pos.y *= ratioY;
		info.bounds = {std::max(1.f, width * ratioX), std::max(1.f, height * ratioY)};
		info.bounds_type = fullScreen ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		info.crop_to_bounds = fullScreen;
		info.scale = {1.f, 1.f};
		const QJsonObject savedLayer = saved.value(source.key).toObject();
		if (!savedLayer.isEmpty()) {
			info.pos = {float(savedLayer.value(QStringLiteral("x")).toDouble(info.pos.x)),
				float(savedLayer.value(QStringLiteral("y")).toDouble(info.pos.y))};
			info.bounds = {float(savedLayer.value(QStringLiteral("w")).toDouble(info.bounds.x)),
				float(savedLayer.value(QStringLiteral("h")).toDouble(info.bounds.y))};
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
	if (auto *existing = mainWindow->findChild<QDockWidget *>(QStringLiteral("xinkxPortraitEditor"))) {
		existing->show();
		existing->raise();
		return;
	}
	if (!ensure()) return;
	sync();
	editing = true;
	auto *dock = new QDockWidget(QStringLiteral("XINKX · Вертикальный холст · 1080×1920"), mainWindow);
	dock->setObjectName(QStringLiteral("xinkxPortraitEditor"));
	dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
	dock->setAttribute(Qt::WA_DeleteOnClose);
	auto *body = new QWidget(dock);
	dock->setWidget(body);
	auto *content = new QHBoxLayout(body);
	content->setContentsMargins(6, 6, 6, 6);
	auto *list = new QListWidget(body);
	list->setMinimumWidth(220);
	content->addWidget(list, 1);
	auto *preview = new PortraitDisplay(obs_scene_get_source(scene), body);
	content->addWidget(preview, 2);
	auto *properties = new QVBoxLayout();
	properties->addWidget(new QLabel(QStringLiteral("Раскладка независима от основной сцены.\n"
		"Перетаскивай выделенный слой на предпросмотре."), body));
	auto *form = new QFormLayout();
	auto *x = dimension(body, 4320, QStringLiteral(" px"));
	auto *y = dimension(body, 4320, QStringLiteral(" px"));
	auto *w = dimension(body, 4320, QStringLiteral(" px"));
	auto *h = dimension(body, 4320, QStringLiteral(" px"));
	for (const auto &pair : {std::pair{QStringLiteral("X"), x}, std::pair{QStringLiteral("Y"), y},
			std::pair{QStringLiteral("Ширина"), w}, std::pair{QStringLiteral("Высота"), h}})
		form->addRow(pair.first, pair.second);
	auto *fill = new QCheckBox(QStringLiteral("Заполнить область (с обрезкой)"), body);
	form->addRow(fill);
	auto *visible = new QCheckBox(QStringLiteral("Видимый слой"), body);
	form->addRow(visible);
	properties->addLayout(form);
	properties->addStretch(1);
	auto *close = new QDialogButtonBox(QDialogButtonBox::Close, body);
	properties->addWidget(close);
	content->addLayout(properties, 2);

	for (const auto &layer : std::as_const(layers))
		list->addItem(layer.name);
	const auto loadCurrent = [=, this] {
		const int row = list->currentRow();
		const bool good = row >= 0 && row < layers.size();
		for (auto *field : {static_cast<QWidget *>(x), static_cast<QWidget *>(y),
			static_cast<QWidget *>(w), static_cast<QWidget *>(h),
			static_cast<QWidget *>(fill), static_cast<QWidget *>(visible)}) field->setEnabled(good);
		if (!good) return;
		obs_transform_info info{};
		obs_sceneitem_get_info2(layers.at(row).item, &info);
		const QSignalBlocker sx(x), sy(y), sw(w), sh(h), sf(fill), sv(visible);
		x->setValue(info.pos.x); y->setValue(info.pos.y);
		w->setValue(info.bounds.x); h->setValue(info.bounds.y);
		fill->setChecked(info.bounds_type == OBS_BOUNDS_SCALE_OUTER);
		visible->setChecked(obs_sceneitem_visible(layers.at(row).item));
	};
	const auto apply = [=, this] {
		const int row = list->currentRow();
		if (row < 0 || row >= layers.size()) return;
		const auto &layer = layers.at(row);
		updateLayer(layer.key, x->value(), y->value(), w->value(), h->value(), fill->isChecked());
		obs_sceneitem_set_visible(layer.item, visible->isChecked());
		save();
	};
	QObject::connect(list, &QListWidget::currentRowChanged, dock, loadCurrent);
	for (auto *spin : {x, y, w, h})
		QObject::connect(spin, &QDoubleSpinBox::valueChanged, dock, apply);
	QObject::connect(fill, &QCheckBox::toggled, dock, apply);
	QObject::connect(visible, &QCheckBox::toggled, dock, apply);
	preview->moved = [=](float dx, float dy) {
		if (list->currentRow() < 0) return;
		x->setValue(std::clamp(x->value() + dx, 0., 4320.));
		y->setValue(std::clamp(y->value() + dy, 0., 4320.));
	};
	QObject::connect(close, &QDialogButtonBox::rejected, dock, &QDockWidget::close);
	QObject::connect(dock, &QObject::destroyed, mainWindow, [this] { editing = false; });
	// OBS scenes may change while the dock remains open. Rebuild the independent
	// 9:16 canvas and its layer list without closing the main OBS preview.
	auto *watchScene = new QTimer(dock);
	watchScene->setInterval(600);
	QObject::connect(watchScene, &QTimer::timeout, dock, [=, this] {
		const QString before = signature;
		editing = false;
		sync();
		editing = true;
		if (before == signature) return;
		{
			const QSignalBlocker blocked(list);
			list->clear();
			for (const auto &layer : std::as_const(layers)) list->addItem(layer.name);
		}
		list->setCurrentRow(layers.isEmpty() ? -1 : 0);
	});
	watchScene->start();
	if (!layers.isEmpty()) list->setCurrentRow(0);
	mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
	mainWindow->resizeDocks({dock}, {720}, Qt::Horizontal);
	dock->show();
}

void PortraitCanvas::shutdown()
{
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
