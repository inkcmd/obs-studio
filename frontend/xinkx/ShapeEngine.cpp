#include "ShapeEngine.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs.hpp>
#include <graphics/graphics.h>

#include <QColorDialog>
#include <QBrush>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QImage>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>

namespace xinkx {

namespace {

struct ShapeState {
	QMutex lock;
	QImage bitmap;
	gs_texture_t *texture = nullptr;
	bool dirty = true;
	std::atomic<uint32_t> width{480};
	std::atomic<uint32_t> height{240};
};

QColor obsColor(obs_data_t *settings, const char *key)
{
	const auto value = static_cast<uint32_t>(obs_data_get_int(settings, key));
	return QColor(static_cast<int>(value & 0xff), static_cast<int>((value >> 8) & 0xff),
		static_cast<int>((value >> 16) & 0xff), static_cast<int>((value >> 24) & 0xff));
}

uint32_t toObsColor(const QColor &color)
{
	return (static_cast<uint32_t>(color.alpha()) << 24) |
		(static_cast<uint32_t>(color.blue()) << 16) |
		(static_cast<uint32_t>(color.green()) << 8) | static_cast<uint32_t>(color.red());
}

void updateShape(void *context, obs_data_t *settings)
{
	auto *shape = static_cast<ShapeState *>(context);
	if (!shape) return;
	const int width = std::clamp(static_cast<int>(obs_data_get_int(settings, "width")), 64, 3840);
	const int height = std::clamp(static_cast<int>(obs_data_get_int(settings, "height")), 64, 2160);
	const int stroke = std::clamp(static_cast<int>(obs_data_get_int(settings, "strokeWidth")),
		0, std::min({80, width - 4, height - 4}));
	const int radius = std::clamp(static_cast<int>(obs_data_get_int(settings, "radius")), 0, 250);
	const int progress = std::clamp(static_cast<int>(obs_data_get_int(settings, "progress")), 0, 100);
	const QString kind = QString::fromUtf8(obs_data_get_string(settings, "shape"));
	const bool gradient = obs_data_get_bool(settings, "gradient");

	QImage image(width, height, QImage::Format_ARGB32);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const qreal half = stroke / 2.0;
	const QRectF bounds(half + 1, half + 1, width - stroke - 2.0, height - stroke - 2.0);
	const QColor first = obsColor(settings, "fillColor");
	QBrush brush(first);
	if (gradient) {
		QLinearGradient fade(0, 0, width, height);
		fade.setColorAt(0.0, first);
		fade.setColorAt(1.0, obsColor(settings, "endColor"));
		brush = QBrush(fade);
	}
	QPen pen(obsColor(settings, "strokeColor"), stroke);
	pen.setJoinStyle(Qt::RoundJoin);
	painter.setPen(stroke ? pen : QPen(Qt::NoPen));
	painter.setBrush(brush);
	if (kind == QStringLiteral("ellipse")) {
		painter.drawEllipse(bounds);
	} else if (kind == QStringLiteral("line")) {
		painter.setPen(QPen(first, std::max(1, stroke), Qt::SolidLine, Qt::RoundCap));
		painter.drawLine(QPointF(half + 2, height - half - 2), QPointF(width - half - 2, half + 2));
	} else if (kind == QStringLiteral("triangle")) {
		QPainterPath triangle;
		triangle.moveTo(width / 2.0, half + 1);
		triangle.lineTo(width - half - 1, height - half - 1);
		triangle.lineTo(half + 1, height - half - 1);
		triangle.closeSubpath();
		painter.drawPath(triangle);
	} else if (kind == QStringLiteral("progress")) {
		painter.setPen(Qt::NoPen);
		painter.setBrush(obsColor(settings, "endColor"));
		painter.drawRoundedRect(bounds, radius, radius);
		painter.setBrush(brush);
		painter.drawRoundedRect(QRectF(bounds.x(), bounds.y(), bounds.width() * progress / 100.0,
			bounds.height()), radius, radius);
	} else {
		painter.drawRoundedRect(bounds, radius, radius);
	}
	painter.end();
	{
		const QMutexLocker guard(&shape->lock);
		shape->bitmap = std::move(image);
		shape->width.store(static_cast<uint32_t>(width), std::memory_order_relaxed);
		shape->height.store(static_cast<uint32_t>(height), std::memory_order_relaxed);
		shape->dirty = true;
	}
}

void *createShape(obs_data_t *settings, obs_source_t *)
{
	auto *shape = new ShapeState;
	updateShape(shape, settings);
	return shape;
}

void destroyShape(void *context)
{
	auto *shape = static_cast<ShapeState *>(context);
	if (!shape) return;
	obs_enter_graphics();
	if (shape->texture) gs_texture_destroy(shape->texture);
	obs_leave_graphics();
	delete shape;
}

void renderShape(void *context, gs_effect_t *)
{
	auto *shape = static_cast<ShapeState *>(context);
	if (!shape) return;
	QImage next;
	{
		const QMutexLocker guard(&shape->lock);
		if (shape->dirty) {
			next = shape->bitmap;
			shape->dirty = false;
		}
	}
	if (!next.isNull()) {
		if (shape->texture) gs_texture_destroy(shape->texture);
		const uint8_t *pixels = next.constBits();
		shape->texture = gs_texture_create(static_cast<uint32_t>(next.width()),
			static_cast<uint32_t>(next.height()), GS_BGRA, 1, &pixels, 0);
	}
	if (shape->texture) obs_source_draw(shape->texture, 0, 0,
		gs_texture_get_width(shape->texture), gs_texture_get_height(shape->texture), false);
}

uint32_t shapeWidth(void *context)
{
	return static_cast<ShapeState *>(context)->width.load(std::memory_order_relaxed);
}

uint32_t shapeHeight(void *context)
{
	return static_cast<ShapeState *>(context)->height.load(std::memory_order_relaxed);
}

void shapeDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "shape", "rectangle");
	obs_data_set_default_int(settings, "width", 480);
	obs_data_set_default_int(settings, "height", 240);
	obs_data_set_default_int(settings, "fillColor", 0xffd0bd2a);
	obs_data_set_default_int(settings, "endColor", 0xff332a20);
	obs_data_set_default_int(settings, "strokeColor", 0xffffffff);
	obs_data_set_default_int(settings, "strokeWidth", 0);
	obs_data_set_default_int(settings, "radius", 20);
	obs_data_set_default_int(settings, "progress", 50);
	obs_data_set_default_bool(settings, "gradient", false);
}

obs_properties_t *shapeProperties(void *)
{
	auto *props = obs_properties_create();
	auto *shape = obs_properties_add_list(props, "shape", "Фигура", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	for (const auto &entry : {std::pair{"Прямоугольник", "rectangle"}, {"Эллипс", "ellipse"},
		{"Линия", "line"}, {"Треугольник", "triangle"}, {"Прогресс", "progress"}})
		obs_property_list_add_string(shape, entry.first, entry.second);
	obs_properties_add_int(props, "width", "Ширина", 64, 3840, 1);
	obs_properties_add_int(props, "height", "Высота", 64, 2160, 1);
	obs_properties_add_color_alpha(props, "fillColor", "Основной цвет");
	obs_properties_add_bool(props, "gradient", "Градиент");
	obs_properties_add_color_alpha(props, "endColor", "Второй цвет");
	obs_properties_add_color_alpha(props, "strokeColor", "Цвет контура");
	obs_properties_add_int_slider(props, "strokeWidth", "Контур (px)", 0, 80, 1);
	obs_properties_add_int_slider(props, "radius", "Скругление (px)", 0, 250, 1);
	obs_properties_add_int_slider(props, "progress", "Прогресс (%)", 0, 100, 1);
	return props;
}

} // namespace

void registerShapeSource()
{
	static bool registered = false;
	if (registered) return;
	registered = true;
	obs_source_info info{};
	info.id = "xinkx_shape";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) -> const char * { return "XINKX · Фигура / элемент"; };
	info.create = createShape;
	info.destroy = destroyShape;
	info.update = updateShape;
	info.video_render = renderShape;
	info.get_width = shapeWidth;
	info.get_height = shapeHeight;
	info.get_defaults = shapeDefaults;
	info.get_properties = shapeProperties;
	info.icon_type = OBS_ICON_TYPE_IMAGE;
	obs_register_source(&info);
}

void showShapeCreator(OBSBasic *window, QWidget *parent)
{
	if (!window || !window->GetCurrentScene()) return;
	QDialog dialog(parent);
	dialog.setWindowTitle(QStringLiteral("Новый элемент сцены · XINKX"));
	auto *form = new QFormLayout(&dialog);
	auto *name = new QLineEdit(QStringLiteral("XINKX · Фигура"), &dialog);
	auto *kind = new QComboBox(&dialog);
	for (const auto &entry : {std::pair{"Прямоугольник", "rectangle"}, {"Эллипс", "ellipse"},
		{"Линия", "line"}, {"Треугольник", "triangle"}, {"Прогресс", "progress"}})
		kind->addItem(QString::fromUtf8(entry.first), QString::fromUtf8(entry.second));
	auto *width = new QSpinBox(&dialog);
	width->setRange(64, 3840);
	width->setValue(480);
	auto *height = new QSpinBox(&dialog);
	height->setRange(64, 2160);
	height->setValue(240);
	QColor color(QStringLiteral("#26cddf"));
	auto *choose = new QPushButton(QStringLiteral("Выбрать цвет"), &dialog);
	QObject::connect(choose, &QPushButton::clicked, &dialog, [&] {
		const QColor selected = QColorDialog::getColor(color, &dialog);
		if (selected.isValid()) color = selected;
	});
	form->addRow(QStringLiteral("Имя источника"), name);
	form->addRow(QStringLiteral("Фигура"), kind);
	form->addRow(QStringLiteral("Ширина"), width);
	form->addRow(QStringLiteral("Высота"), height);
	form->addRow(QStringLiteral("Заливка"), choose);
	auto *create = new QPushButton(QStringLiteral("Добавить на сцену"), &dialog);
	form->addRow(create);
	QObject::connect(create, &QPushButton::clicked, &dialog, [&] {
		const QString title = name->text().trimmed();
		if (title.isEmpty() || title.size() > 100) return;
		OBSSourceAutoRelease existing = obs_get_source_by_name(title.toUtf8().constData());
		if (existing) {
			QMessageBox::warning(&dialog, QStringLiteral("Фигура"), QStringLiteral("Такое имя источника уже занято"));
			return;
		}
		dialog.accept();
	});
	if (dialog.exec() != QDialog::Accepted) return;
	OBSScene scene = window->GetCurrentScene();
	if (!scene) return;
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "shape", kind->currentData().toString().toUtf8().constData());
	obs_data_set_int(settings, "width", width->value());
	obs_data_set_int(settings, "height", height->value());
	obs_data_set_int(settings, "fillColor", toObsColor(color));
	const QByteArray title = name->text().trimmed().toUtf8();
	obs_source_t *source = obs_source_create("xinkx_shape", title.constData(), settings, nullptr);
	if (!source) { QMessageBox::warning(parent, QStringLiteral("Фигура"),
		QStringLiteral("Не удалось создать графический источник")); return; }
	OBSData before = OBSBasic::BackupScene(scene);
	auto *item = obs_scene_add(scene, source);
	obs_source_release(source);
	if (!item) return;
	obs_sceneitem_select(item, true);
	OBSData after = OBSBasic::BackupScene(scene);
	window->CreateSceneUndoRedoAction(QStringLiteral("XINKX: добавить фигуру"), before, after);
}

} // namespace xinkx
