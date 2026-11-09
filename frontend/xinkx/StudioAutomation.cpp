#include "StudioAutomation.hpp"

#include "OverlayRuntime.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>

namespace xinkx {

namespace {

QJsonObject result(bool success, const QString &message)
{
	return {{QStringLiteral("ok"), success}, {QStringLiteral("message"), message}};
}

bool safeName(const QString &value)
{
	return !value.trimmed().isEmpty() && value.size() <= 100 &&
	       !value.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")));
}

} // namespace

StudioAutomation::StudioAutomation(OBSBasic *mainWindow) : QObject(mainWindow), window(mainWindow)
{
	setObjectName(QStringLiteral("xinkxStudioAutomation"));
}

QJsonObject StudioAutomation::describe() const
{
	QJsonArray scenes;
	obs_frontend_source_list list{};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; ++i)
		scenes.append(QString::fromUtf8(obs_source_get_name(list.sources.array[i])));
	obs_frontend_source_list_free(&list);
	return {{QStringLiteral("scenes"), scenes},
		{QStringLiteral("commands"), QJsonArray{QStringLiteral("scene.create"),
			QStringLiteral("scene.activate"), QStringLiteral("overlay.add"),
			QStringLiteral("source.text.add"), QStringLiteral("source.visible"),
			QStringLiteral("effect.trigger"), QStringLiteral("timer.start"),
			QStringLiteral("timer.pause"), QStringLiteral("timer.hide")}}};
}

QJsonObject StudioAutomation::execute(const QJsonObject &command)
{
	if (QThread::currentThread() != thread())
		return result(false, QStringLiteral("Команды OBS должны выполняться в UI-потоке"));
	const auto type = command.value(QStringLiteral("command")).toString();
	const auto name = command.value(QStringLiteral("name")).toString();
	if (type == QStringLiteral("scene.create")) {
		if (!safeName(name)) return result(false, QStringLiteral("Некорректное имя сцены"));
		OBSSourceAutoRelease existing = obs_get_source_by_name(name.toUtf8().constData());
		if (existing) return result(false, QStringLiteral("Имя источника/сцены уже занято"));
		OBSSceneAutoRelease scene = obs_scene_create(name.toUtf8().constData());
		if (!scene) return result(false, QStringLiteral("Не удалось создать сцену"));
		obs_frontend_set_current_scene(obs_scene_get_source(scene));
		return result(true, QStringLiteral("Сцена создана: ") + name);
	}
	if (type == QStringLiteral("scene.activate")) {
		OBSSourceAutoRelease source = obs_get_source_by_name(name.toUtf8().constData());
		if (!source || obs_source_get_type(source) != OBS_SOURCE_TYPE_SCENE)
			return result(false, QStringLiteral("Сцена не найдена"));
		obs_frontend_set_current_scene(source);
		return result(true, QStringLiteral("Сцена активирована: ") + name);
	}
	auto *overlay = window->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime"));
	if (type == QStringLiteral("overlay.add")) {
		if (!overlay) return result(false, QStringLiteral("Движок оверлея недоступен"));
		QString message;
		const bool success = overlay->addToScene(window, &message,
			command.value(QStringLiteral("layer")).toString(QStringLiteral("chat")));
		return result(success, message);
	}
	if (type == QStringLiteral("effect.trigger")) {
		if (!overlay) return result(false, QStringLiteral("Движок недоступен"));
		const auto effect = command.value(QStringLiteral("effect")).toString();
		if (effect != QStringLiteral("gg") && effect != QStringLiteral("boom") &&
		    effect != QStringLiteral("low-hp") && effect != QStringLiteral("confetti"))
			return result(false, QStringLiteral("Неизвестный эффект"));
		overlay->reaction(effect);
		return result(true, QStringLiteral("Эффект отправлен"));
	}
	if (type == QStringLiteral("timer.start") || type == QStringLiteral("timer.pause") ||
	    type == QStringLiteral("timer.hide")) {
		if (!overlay) return result(false, QStringLiteral("Движок недоступен"));
		if (type == QStringLiteral("timer.start")) {
			const int seconds = command.value(QStringLiteral("seconds")).toInt();
			if (seconds < 1 || seconds > 3600) return result(false, QStringLiteral("Таймер: 1–3600 секунд"));
			overlay->startTimer(seconds);
		} else if (type == QStringLiteral("timer.pause")) overlay->pauseTimer();
		else overlay->resetTimer();
		return result(true, QStringLiteral("Таймер обновлён"));
	}
	OBSScene scene = window->GetCurrentScene();
	if (!scene) return result(false, QStringLiteral("Сначала выбери сцену OBS"));
	if (type == QStringLiteral("source.text.add")) {
		const auto content = command.value(QStringLiteral("text")).toString();
		if (!safeName(name) || content.isEmpty() || content.size() > 4096)
			return result(false, QStringLiteral("Имя или текст некорректны"));
		OBSSourceAutoRelease existing = obs_get_source_by_name(name.toUtf8().constData());
		if (existing) return result(false, QStringLiteral("Имя источника уже занято"));
		OBSData settings = obs_data_create();
		obs_data_set_string(settings, "text", content.toUtf8().constData());
		obs_source_t *source = obs_source_create("text_gdiplus", name.toUtf8().constData(), settings, nullptr);
		obs_data_release(settings);
		if (!source) return result(false, QStringLiteral("Текстовый источник GDI+ недоступен"));
		OBSData before = OBSBasic::BackupScene(scene);
		auto *item = obs_scene_add(scene, source);
		obs_source_release(source);
		if (!item) return result(false, QStringLiteral("Не удалось добавить текст"));
		obs_sceneitem_select(item, true);
		OBSData after = OBSBasic::BackupScene(scene);
		window->CreateSceneUndoRedoAction(QStringLiteral("XINKX: добавить текст"), before, after);
		return result(true, QStringLiteral("Текстовый источник создан"));
	}
	if (type == QStringLiteral("source.visible")) {
		if (!safeName(name) || !command.value(QStringLiteral("visible")).isBool())
			return result(false, QStringLiteral("Нужно имя источника и true/false"));
		auto *item = obs_scene_find_source(scene, name.toUtf8().constData());
		if (!item) return result(false, QStringLiteral("Источник не найден в текущей сцене"));
		obs_sceneitem_set_visible(item, command.value(QStringLiteral("visible")).toBool());
		return result(true, QStringLiteral("Видимость обновлена"));
	}
	return result(false, QStringLiteral("Команда не разрешена"));
}

} // namespace xinkx
