#include "NativeSources.hpp"

#include <obs.hpp>
#include <util/bmem.h>

#include <QUrl>
#include <QUrlQuery>
#include <QString>

#include <array>
#include <cstring>

namespace xinkx {
namespace {

struct LayerType {
	const char *id;
	const char *layer;
	const char *title;
	int width;
	int height;
};

constexpr std::array<LayerType, 6> layers = {{
	{"xinkx_chat", "chat", "XINKX · Чат зрителей", 720, 560},
	{"xinkx_reactions", "effects", "XINKX · Реакции и эффекты", 1920, 1080},
	{"xinkx_alerts", "alerts", "XINKX · Оповещения и донаты", 960, 440},
	{"xinkx_timer", "timer", "XINKX · Таймер", 480, 180},
	{"xinkx_viewers", "viewers", "XINKX · Зрители", 520, 100},
	{"xinkx_frame", "frame", "XINKX · Рамка камеры", 800, 450},
}};

const LayerType &kind(const char *id)
{
	for (const auto &layer : layers) {
		if (id && std::strcmp(layer.id, id) == 0)
			return layer;
	}
	return layers.front();
}

struct NativeSource {
	obs_source_t *parent = nullptr;
	obs_source_t *browser = nullptr;
	const LayerType *type = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
};

void update(void *data, obs_data_t *settings)
{
	auto *source = static_cast<NativeSource *>(data);
	if (!source || !source->browser)
		return;
	const auto width = static_cast<int>(obs_data_get_int(settings, "width"));
	const auto height = static_cast<int>(obs_data_get_int(settings, "height"));
	source->width = static_cast<uint32_t>(width > 0 ? width : source->type->width);
	source->height = static_cast<uint32_t>(height > 0 ? height : source->type->height);

	QUrl url(QStringLiteral("http://127.0.0.1:8766/overlay"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("layer"), QString::fromLatin1(source->type->layer));
	if (obs_data_get_bool(settings, "customize")) {
	const auto rgb = [settings](const char *key) {
		const uint32_t value = static_cast<uint32_t>(obs_data_get_int(settings, key));
		return QStringLiteral("#%1%2%3")
			.arg(static_cast<int>(value & 0xff), 2, 16, QLatin1Char('0'))
			.arg(static_cast<int>((value >> 8) & 0xff), 2, 16, QLatin1Char('0'))
			.arg(static_cast<int>((value >> 16) & 0xff), 2, 16, QLatin1Char('0'));
	};
	query.addQueryItem(QStringLiteral("accent"), rgb("accent"));
	query.addQueryItem(QStringLiteral("opacity"), QString::number(obs_data_get_int(settings, "opacity")));
	query.addQueryItem(QStringLiteral("scale"), QString::number(obs_data_get_int(settings, "scale")));
	query.addQueryItem(QStringLiteral("background"), rgb("background"));
	query.addQueryItem(QStringLiteral("text"), rgb("text"));
	query.addQueryItem(QStringLiteral("fontSize"), QString::number(obs_data_get_int(settings, "fontSize")));
	query.addQueryItem(QStringLiteral("radius"), QString::number(obs_data_get_int(settings, "radius")));
	}
	url.setQuery(query);

	OBSDataAutoRelease browserSettings = obs_data_create();
	obs_data_set_string(browserSettings, "url", url.toString(QUrl::FullyEncoded).toUtf8().constData());
	obs_data_set_int(browserSettings, "width", source->width);
	obs_data_set_int(browserSettings, "height", source->height);
	obs_data_set_int(browserSettings, "fps", 30);
	obs_data_set_bool(browserSettings, "shutdown", false);
	obs_data_set_bool(browserSettings, "restart_when_active", true);
	obs_source_update(source->browser, browserSettings);
}

void *create(obs_data_t *settings, obs_source_t *parent)
{
	auto *source = new NativeSource();
	source->parent = parent;
	source->type = &kind(obs_source_get_id(parent));
	OBSDataAutoRelease browserSettings = obs_data_create();
	source->browser = obs_source_create_private("browser_source", nullptr, browserSettings);
	if (!source->browser) {
		blog(LOG_ERROR, "XINKX: obs-browser is required for %s", source->type->id);
		delete source;
		return nullptr;
	}
	obs_source_add_active_child(parent, source->browser);
	update(source, settings);
	return source;
}

void destroy(void *data)
{
	auto *source = static_cast<NativeSource *>(data);
	if (!source)
		return;
	if (source->browser) {
		obs_source_remove_active_child(source->parent, source->browser);
		obs_source_release(source->browser);
	}
	delete source;
}

uint32_t width(void *data) { return static_cast<NativeSource *>(data)->width; }
uint32_t height(void *data) { return static_cast<NativeSource *>(data)->height; }

void render(void *data, gs_effect_t *)
{
	auto *source = static_cast<NativeSource *>(data);
	if (source->browser)
		obs_source_video_render(source->browser);
}

void enumActiveSources(void *data, obs_source_enum_proc_t callback, void *param)
{
	auto *source = static_cast<NativeSource *>(data);
	if (source->browser)
		callback(source->parent, source->browser, param);
}

void defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 720);
	obs_data_set_default_int(settings, "height", 560);
	obs_data_set_default_int(settings, "accent", 0xefe46b);
	obs_data_set_default_int(settings, "background", 0x38180a);
	obs_data_set_default_int(settings, "text", 0xfffaf6);
	obs_data_set_default_int(settings, "fontSize", 26);
	obs_data_set_default_int(settings, "radius", 12);
	obs_data_set_default_bool(settings, "customize", false);
	obs_data_set_default_int(settings, "opacity", 100);
	obs_data_set_default_int(settings, "scale", 100);
}

void defaultsForType(void *type_data, obs_data_t *settings)
{
	defaults(settings);
	const auto *type = static_cast<const LayerType *>(type_data);
	obs_data_set_default_int(settings, "width", type->width);
	obs_data_set_default_int(settings, "height", type->height);
}

obs_properties_t *properties(void *data)
{
	auto *properties = obs_properties_create();
	(void)data;
	obs_properties_add_int(properties, "width", "Ширина элемента (пиксели)", 80, 3840, 1);
	obs_properties_add_int(properties, "height", "Высота элемента (пиксели)", 50, 2160, 1);
	obs_properties_add_bool(properties, "customize", "Своё оформление для этого источника (иначе настройки по умолчанию)");
	obs_properties_add_color(properties, "accent", "Акцентный цвет");
	obs_properties_add_color(properties, "background", "Фон уведомлений");
	obs_properties_add_color(properties, "text", "Цвет текста");
	obs_properties_add_int_slider(properties, "fontSize", "Размер текста (px)", 10, 100, 1);
	obs_properties_add_int_slider(properties, "radius", "Скругление рамки (px)", 0, 60, 1);
	obs_properties_add_int_slider(properties, "opacity", "Непрозрачность (%)", 0, 100, 1);
	obs_properties_add_int_slider(properties, "scale", "Масштаб содержимого (%)", 25, 200, 1);
	return properties;
}

} // namespace

void registerNativeSources()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	for (const auto &type : layers) {
		obs_source_info info{};
		info.id = type.id;
		info.type = OBS_SOURCE_TYPE_INPUT;
		// Video-only input. OBS rejects COMPOSITE sources without an audio_render
		// callback, so none of the XINKX source types appeared in Sources -> +.
		info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
		info.get_name = [](void *type_data) -> const char * {
			return static_cast<const LayerType *>(type_data)->title;
		};
		info.type_data = const_cast<LayerType *>(&type);
		info.create = create;
		info.destroy = destroy;
		info.update = update;
		info.video_render = render;
		info.enum_active_sources = enumActiveSources;
		info.get_width = width;
		info.get_height = height;
		info.get_defaults2 = defaultsForType;
		info.get_properties = properties;
		info.icon_type = OBS_ICON_TYPE_CUSTOM;
		info.get_dark_icon = [](void *) -> const char * { return bstrdup(":/xinkx/xinkx-source.svg"); };
		info.get_light_icon = [](void *) -> const char * { return bstrdup(":/xinkx/xinkx-source.svg"); };
		obs_register_source(&info);
		if (!obs_get_latest_input_type_id(type.id))
			blog(LOG_ERROR, "XINKX: source type %s was not registered", type.id);
	}
}

} // namespace xinkx
