#include <widgets/OBSBasic.hpp>
#include <obs.hpp>

#include <QDockWidget>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <QVector>

#include <cmath>

namespace xinkx {

namespace {

struct Item {
	obs_sceneitem_t *item = nullptr;
	qint64 id = 0;
	QString source;
};

bool collect(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto *result = static_cast<QVector<Item> *>(data);
	if (result->size() >= 256) return false;
	obs_source_t *source = obs_sceneitem_get_source(item);
	result->push_back({item, obs_sceneitem_get_id(item),
		QString::fromUtf8(source ? obs_source_get_name(source) : "")});
	return true;
}

QVector<Item> sceneItems(obs_scene_t *scene)
{
	QVector<Item> result;
	obs_scene_enum_items(scene, collect, &result);
	return result;
}

QJsonObject exportScene(obs_scene_t *scene)
{
	QJsonArray entries;
	for (const auto &entry : sceneItems(scene)) {
		vec2 pos{}, scale{};
		obs_sceneitem_get_pos(entry.item, &pos);
		obs_sceneitem_get_scale(entry.item, &scale);
		obs_sceneitem_crop crop{};
		obs_sceneitem_get_crop(entry.item, &crop);
		entries.append(QJsonObject{
			{QStringLiteral("id"), QString::number(entry.id)},
			{QStringLiteral("source"), entry.source},
			{QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), pos.x}, {QStringLiteral("y"), pos.y}}},
			{QStringLiteral("scale"), QJsonObject{{QStringLiteral("x"), scale.x}, {QStringLiteral("y"), scale.y}}},
			{QStringLiteral("rotation"), obs_sceneitem_get_rot(entry.item)},
			{QStringLiteral("crop"), QJsonObject{{QStringLiteral("left"), crop.left},
				{QStringLiteral("top"), crop.top}, {QStringLiteral("right"), crop.right},
				{QStringLiteral("bottom"), crop.bottom}}},
			{QStringLiteral("visible"), obs_sceneitem_visible(entry.item)},
			{QStringLiteral("locked"), obs_sceneitem_locked(entry.item)},
		});
	}
	return {{QStringLiteral("version"), 1},
		{QStringLiteral("scene"), QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene)))},
		{QStringLiteral("items"), entries}};
}

bool finiteNumber(const QJsonObject &obj, const QString &key, double min, double max)
{
	const auto value = obj.value(key);
	return value.isDouble() && std::isfinite(value.toDouble()) &&
		value.toDouble() >= min && value.toDouble() <= max;
}

bool validateScene(const QJsonObject &data, obs_scene_t *scene, QString &error)
{
	if (data.value(QStringLiteral("version")).toInt() != 1 ||
	    data.value(QStringLiteral("scene")).toString() !=
		QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene)))) {
		error = QStringLiteral("JSON относится к другой сцене OBS либо имеет неверную версию"); return false;
	}
	const auto actual = sceneItems(scene);
	const auto items = data.value(QStringLiteral("items")).toArray();
	if (actual.size() != items.size()) {
		error = QStringLiteral("Количество объектов изменилось: сначала заново загрузи сцену"); return false;
	}
	QSet<QString> ids;
	for (const auto &value : items) {
		if (!value.isObject()) { error = QStringLiteral("Ожидался объект JSON"); return false; }
		const auto item = value.toObject();
		const auto id = item.value(QStringLiteral("id")).toString();
		if (ids.contains(id)) { error = QStringLiteral("Повторяющийся ID источника"); return false; }
		ids.insert(id);
		const auto source = item.value(QStringLiteral("source")).toString();
		bool found = false;
		for (const auto &actualItem : actual) {
			if (QString::number(actualItem.id) == id && actualItem.source == source) { found = true; break; }
		}
		if (!found) { error = QStringLiteral("Источник отсутствует или изменился: ") + source; return false; }
		const auto pos = item.value(QStringLiteral("position")).toObject();
		const auto scale = item.value(QStringLiteral("scale")).toObject();
		const auto crop = item.value(QStringLiteral("crop")).toObject();
		if (!finiteNumber(pos, QStringLiteral("x"), -100000, 100000) ||
		    !finiteNumber(pos, QStringLiteral("y"), -100000, 100000) ||
		    !finiteNumber(scale, QStringLiteral("x"), -100, 100) ||
		    !finiteNumber(scale, QStringLiteral("y"), -100, 100) ||
		    !finiteNumber(item, QStringLiteral("rotation"), -3600, 3600)) {
			error = QStringLiteral("Недопустимые координаты или масштаб: ") + source; return false;
		}
		for (const auto &key : {QStringLiteral("left"), QStringLiteral("top"),
			QStringLiteral("right"), QStringLiteral("bottom")}) {
			if (!finiteNumber(crop, key, 0, 32768) || std::floor(crop.value(key).toDouble()) != crop.value(key).toDouble()) {
				error = QStringLiteral("Неверная обрезка: ") + source; return false;
			}
		}
		if (!item.value(QStringLiteral("visible")).isBool() ||
		    !item.value(QStringLiteral("locked")).isBool()) {
			error = QStringLiteral("visible/locked должны быть bool"); return false;
		}
	}
	return true;
}

} // namespace

void installSceneJsonWorkspace(OBSBasic *mainWindow)
{
	auto *dock = new QDockWidget(QStringLiteral("JSON сцены OBS"), mainWindow);
	dock->setObjectName(QStringLiteral("xinkxSceneJsonDock"));
	auto *panel = new QWidget(dock);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 10, 10, 10);
	auto *heading = new QLabel(QStringLiteral("Трансформации, видимость и блокировка всех объектов текущей сцены OBS. Нельзя добавлять/удалять источники через JSON — это делается штатными инструментами OBS."), panel);
	heading->setWordWrap(true);
	layout->addWidget(heading);
	auto *editor = new QPlainTextEdit(panel);
	editor->setPlaceholderText(QStringLiteral("Нажми «Загрузить сцену OBS»"));
	layout->addWidget(editor, 1);
	auto *buttons = new QHBoxLayout();
	auto *load = new QPushButton(QStringLiteral("Загрузить сцену OBS"), panel);
	auto *apply = new QPushButton(QStringLiteral("Применить JSON · с Ctrl+Z"), panel);
	buttons->addWidget(load);
	buttons->addWidget(apply);
	layout->addLayout(buttons);
	auto *status = new QLabel(panel);
	status->setWordWrap(true);
	layout->addWidget(status);
	QObject::connect(load, &QPushButton::clicked, dock, [=] {
		OBSScene scene = mainWindow->GetCurrentScene();
		if (!scene) { status->setText(QStringLiteral("Текущей сцены OBS нет")); return; }
		editor->setPlainText(QString::fromUtf8(QJsonDocument(exportScene(scene)).toJson(QJsonDocument::Indented)));
		status->setText(QStringLiteral("Загружено · правки применятся только к той же сцене и тому же набору источников"));
	});
	QObject::connect(apply, &QPushButton::clicked, dock, [=] {
		OBSScene scene = mainWindow->GetCurrentScene();
		if (!scene) { status->setText(QStringLiteral("Текущей сцены OBS нет")); return; }
		QJsonParseError parseError;
		const auto document = QJsonDocument::fromJson(editor->toPlainText().toUtf8(), &parseError);
		QString error;
		if (!document.isObject() || !validateScene(document.object(), scene, error)) {
			status->setText(document.isObject() ? error : parseError.errorString()); return;
		}
		const auto before = OBSBasic::BackupScene(scene);
		const auto actual = sceneItems(scene);
		for (const auto &entry : document.object().value(QStringLiteral("items")).toArray()) {
			const auto data = entry.toObject();
			for (const auto &current : actual) {
				if (QString::number(current.id) != data.value(QStringLiteral("id")).toString()) continue;
				const auto p = data.value(QStringLiteral("position")).toObject();
				const auto s = data.value(QStringLiteral("scale")).toObject();
				const auto c = data.value(QStringLiteral("crop")).toObject();
				vec2 position{static_cast<float>(p.value(QStringLiteral("x")).toDouble()),
					static_cast<float>(p.value(QStringLiteral("y")).toDouble())};
				vec2 scale{static_cast<float>(s.value(QStringLiteral("x")).toDouble()),
					static_cast<float>(s.value(QStringLiteral("y")).toDouble())};
				obs_sceneitem_crop crop{c.value(QStringLiteral("left")).toInt(), c.value(QStringLiteral("top")).toInt(),
					c.value(QStringLiteral("right")).toInt(), c.value(QStringLiteral("bottom")).toInt()};
				obs_sceneitem_set_pos(current.item, &position);
				obs_sceneitem_set_scale(current.item, &scale);
				obs_sceneitem_set_rot(current.item, static_cast<float>(data.value(QStringLiteral("rotation")).toDouble()));
				obs_sceneitem_set_crop(current.item, &crop);
				obs_sceneitem_set_visible(current.item, data.value(QStringLiteral("visible")).toBool());
				obs_sceneitem_set_locked(current.item, data.value(QStringLiteral("locked")).toBool());
				break;
			}
		}
		const auto after = OBSBasic::BackupScene(scene);
		mainWindow->CreateSceneUndoRedoAction(QStringLiteral("XINKX: применить JSON сцены"), before, after);
		status->setText(QStringLiteral("JSON применён к текущей сцене OBS · Ctrl+Z для отмены"));
	});
	dock->setWidget(panel);
	mainWindow->AddDockWidget(dock, Qt::RightDockWidgetArea);
	if (auto *profiles = mainWindow->findChild<QDockWidget *>(QStringLiteral("xinkxGameProfilesDock")))
		mainWindow->tabifyDockWidget(profiles, dock);
	dock->show();
}

} // namespace xinkx
