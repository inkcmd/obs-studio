#include "NativeSources.hpp"
#include "ShapeEngine.hpp"

#include <OBSApp.hpp>
#include <obs.hpp>
#include <util/bmem.h>

#include <QCryptographicHash>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>
#include <QString>
#include <QVariant>

#include <array>
#include <cstring>
#include <utility>

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

QString userPresetId(const QJsonObject &entry)
{
	return QStringLiteral("xinkx-user-") + QString::fromLatin1(
		QCryptographicHash::hash(QJsonDocument(entry).toJson(QJsonDocument::Compact),
			QCryptographicHash::Sha256).toHex().left(16));
}

QString catalogPath()
{
	char path[512]{};
	const int length = GetAppConfigPath(path, sizeof(path), "xinkx-studio/studio.ini");
	return length > 0 && static_cast<size_t>(length) < sizeof(path) ? QString::fromUtf8(path) : QString{};
}

QJsonArray savedPresets()
{
	const QString path = catalogPath();
	if (path.isEmpty()) return {};
	QSettings storage(path, QSettings::IniFormat);
	return QJsonDocument::fromJson(storage.value(QStringLiteral("catalog/presets")).toByteArray()).array();
}

bool presetModified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	const QString selected = QString::fromUtf8(obs_data_get_string(settings, "preset"));
	if (selected == QString::fromUtf8(obs_data_get_string(settings, "xinkxAppliedPreset"))) return false;
	obs_data_set_string(settings, "xinkxAppliedPreset", selected.toUtf8().constData());
	if (!selected.startsWith(QStringLiteral("xinkx-user-"))) {
		obs_data_set_string(settings, "xinkxAppearance", selected.toUtf8().constData());
		return false;
	}
	for (const auto &value : savedPresets()) {
		const QJsonObject entry = value.toObject();
		if (userPresetId(entry) != selected) continue;
		const QJsonObject configuration = entry.value(QStringLiteral("settings")).toObject();
		// Keep the selected template identity, but materialize its settings so
		// an OBS source keeps working even if that template is later removed.
		for (auto it = configuration.begin(); it != configuration.end(); ++it) {
			const QByteArray property = it.key().toUtf8();
			if (it.key() == QStringLiteral("preset")) {
				obs_data_set_string(settings, "xinkxAppearance", it.value().toString().toUtf8().constData());
			} else if (it.key() == QStringLiteral("customize")) {
				obs_data_set_bool(settings, "customize", true);
			} else if (it.key() == QStringLiteral("reaction") || it.key() == QStringLiteral("customText")) {
				obs_data_set_string(settings, property.constData(), it.value().toString().toUtf8().constData());
			} else if (it.key() == QStringLiteral("width") || it.key() == QStringLiteral("height") ||
				   it.key() == QStringLiteral("accent") || it.key() == QStringLiteral("background") ||
				   it.key() == QStringLiteral("text") || it.key() == QStringLiteral("fontSize") ||
				   it.key() == QStringLiteral("radius") || it.key() == QStringLiteral("opacity") ||
				   it.key() == QStringLiteral("scale") || it.key() == QStringLiteral("panelColor") ||
				   it.key() == QStringLiteral("messageColor")) {
				obs_data_set_int(settings, property.constData(), it.value().toVariant().toLongLong());
			}
		}
		obs_data_set_bool(settings, "customize", true);
		return true;
	}
	return false;
}

bool saveNativePreset(obs_properties_t *, obs_property_t *, void *data)
{
	auto *source = static_cast<NativeSource *>(data);
	if (!source || !source->parent || !source->type) return false;
	const QString path = catalogPath();
	if (path.isEmpty()) return false;
	OBSDataAutoRelease settings = obs_source_get_settings(source->parent);
	const char *json = obs_data_get_json(settings);
	QJsonObject configuration = QJsonDocument::fromJson(QByteArray(json ? json : "{}")).object();
	const QString appearance = QString::fromUtf8(obs_data_get_string(settings, "preset"));
	if (appearance.startsWith(QStringLiteral("xinkx-user-")))
		configuration.insert(QStringLiteral("preset"), QString::fromUtf8(obs_data_get_string(settings, "xinkxAppearance")));
	configuration.remove(QStringLiteral("xinkxAppearance"));
	configuration.remove(QStringLiteral("xinkxAppliedPreset"));
	configuration.insert(QStringLiteral("customize"), true);
	bool accepted = false;
	const QString name = QInputDialog::getText(nullptr, QStringLiteral("XINKX · Сохранить пресет"),
		QStringLiteral("Имя нового пресета (после применения настроек источника)"),
		QLineEdit::Normal, QString::fromUtf8(obs_source_get_name(source->parent)), &accepted).trimmed().left(80);
	if (!accepted || name.isEmpty()) return false;
	QJsonArray entries = savedPresets();
	if (entries.size() >= 100) return false;
	entries.append(QJsonObject{{QStringLiteral("name"), name},
		{QStringLiteral("layer"), QString::fromLatin1(source->type->layer)},
		{QStringLiteral("settings"), configuration}});
	QSettings storage(path, QSettings::IniFormat);
	storage.setValue(QStringLiteral("catalog/presets"), QJsonDocument(entries).toJson(QJsonDocument::Compact));
	storage.sync();
	return storage.status() == QSettings::NoError;
}

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
	const QString requested = QString::fromUtf8(obs_data_get_string(settings, "preset"));
	const QString appearance = requested.startsWith(QStringLiteral("xinkx-user-"))
		? QString::fromUtf8(obs_data_get_string(settings, "xinkxAppearance")) : requested;
	query.addQueryItem(QStringLiteral("preset"), appearance.isEmpty() ? QStringLiteral("default") : appearance);
	query.addQueryItem(QStringLiteral("reaction"), QString::fromUtf8(obs_data_get_string(settings, "reaction")));
	query.addQueryItem(QStringLiteral("customText"), QString::fromUtf8(obs_data_get_string(settings, "customText")));
	if (obs_data_get_bool(settings, "customize")) {
	const auto rgb = [settings](const char *key, bool alpha = false) {
		const uint32_t value = static_cast<uint32_t>(obs_data_get_int(settings, key));
		QString color = QStringLiteral("#%1%2%3")
			.arg(static_cast<int>(value & 0xff), 2, 16, QLatin1Char('0'))
			.arg(static_cast<int>((value >> 8) & 0xff), 2, 16, QLatin1Char('0'))
			.arg(static_cast<int>((value >> 16) & 0xff), 2, 16, QLatin1Char('0'));
		if (alpha) color += QStringLiteral("%1").arg(static_cast<int>((value >> 24) & 0xff), 2, 16, QLatin1Char('0'));
		return color;
	};
	query.addQueryItem(QStringLiteral("accent"), rgb("accent"));
	query.addQueryItem(QStringLiteral("opacity"), QString::number(obs_data_get_int(settings, "opacity")));
	query.addQueryItem(QStringLiteral("scale"), QString::number(obs_data_get_int(settings, "scale")));
	query.addQueryItem(QStringLiteral("background"), rgb("background"));
	query.addQueryItem(QStringLiteral("text"), rgb("text"));
	query.addQueryItem(QStringLiteral("fontSize"), QString::number(obs_data_get_int(settings, "fontSize")));
	query.addQueryItem(QStringLiteral("radius"), QString::number(obs_data_get_int(settings, "radius")));
	query.addQueryItem(QStringLiteral("panelColor"), rgb("panelColor", true));
	query.addQueryItem(QStringLiteral("messageColor"), rgb("messageColor", true));
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
	obs_data_set_default_string(settings, "preset", "default");
	obs_data_set_default_string(settings, "reaction", "all");
	obs_data_set_default_string(settings, "customText", "");
	obs_data_set_default_int(settings, "panelColor", 0x000a152c);
	obs_data_set_default_int(settings, "messageColor", 0xdc2b1508);
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
	const auto *source = static_cast<NativeSource *>(data);
	const QString layer = source && source->type ? QString::fromLatin1(source->type->layer) : QString{};
	obs_properties_add_int(properties, "width", "Ширина элемента (пиксели)", 80, 3840, 1);
	obs_properties_add_int(properties, "height", "Высота элемента (пиксели)", 50, 2160, 1);
	obs_properties_add_bool(properties, "customize", "Своё оформление для этого источника (иначе настройки по умолчанию)");
	// The same local preset library is available directly in OBS Sources -> +
	// -> Properties. The selected user preset materializes as independent source
	// settings so editing one instance never changes another.
	auto *preset = obs_properties_add_list(properties, "preset", "Шаблон оформления",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(preset, "Стандартное", "default");
	if (layer == QStringLiteral("chat") || layer == QStringLiteral("frame") || layer == QStringLiteral("effects")) {
		for (const auto &entry : {std::pair{"compact", "Компактное"}, {"glass", "Стекло"},
			{"neon", "Неон"}, {"minimal", "Минимализм"}, {"gold", "Золотое"},
			{"round", "Скруглённое"}})
			obs_property_list_add_string(preset, entry.second, entry.first);
	}
	for (const auto &value : savedPresets()) {
		const auto entry = value.toObject();
		if (entry.value(QStringLiteral("layer")).toString() != layer) continue;
		const QByteArray title = (QStringLiteral("Мой · ") + entry.value(QStringLiteral("name")).toString())
			.toUtf8();
		const QByteArray id = userPresetId(entry).toUtf8();
		obs_property_list_add_string(preset, title.constData(), id.constData());
	}
	obs_property_set_modified_callback(preset, presetModified);
	if (source && source->parent) {
		auto *save = obs_properties_add_button2(properties, "saveXinkxPreset",
			"Сохранить настроенный источник как мой пресет", saveNativePreset, const_cast<NativeSource *>(source));
		obs_property_set_long_description(save, "Сначала нажмите «Применить», затем сохраните копию настроек. "
			"Новый пресет появится при следующем открытии свойств источника.");
	}
	if (layer == QStringLiteral("effects")) {
		auto *reaction = obs_properties_add_list(properties, "reaction", "Показывать реакцию",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		for (const auto &entry : {std::pair{"all", "Все реакции"}, {"gg", "GG"}, {"boom", "BOOM"},
			{"low-hp", "LOW HP"}, {"confetti", "Конфетти"}})
			obs_property_list_add_string(reaction, entry.second, entry.first);
		if (source && source->parent) {
			OBSDataAutoRelease saved = obs_source_get_settings(source->parent);
			const QString current = QString::fromUtf8(obs_data_get_string(saved, "reaction"));
			if (current.startsWith(QStringLiteral("custom-")))
				obs_property_list_add_string(reaction, "Своя реакция", current.toUtf8().constData());
		}
		obs_properties_add_text(properties, "customText", "Свой текст поверх реакции", OBS_TEXT_DEFAULT);
	}
	obs_properties_add_color(properties, "accent", "Акцентный цвет");
	obs_properties_add_color(properties, "background", "Фон уведомлений");
	if (layer == QStringLiteral("chat")) {
		obs_properties_add_color_alpha(properties, "panelColor", "Фон всей области чата (с прозрачностью)");
		obs_properties_add_color_alpha(properties, "messageColor", "Фон сообщений (с прозрачностью)");
	}
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
	registerShapeSource();
}

} // namespace xinkx
