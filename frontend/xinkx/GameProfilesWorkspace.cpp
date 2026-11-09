#include "ChatGateway.hpp"
#include "StudioWorkspace.hpp"
#include "StreamAccounts.hpp"

#include <widgets/OBSBasic.hpp>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {
namespace {

QString currentScene()
{
	obs_source_t *source = obs_frontend_get_current_scene();
	if (!source) return {};
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	obs_source_release(source);
	return name;
}

bool useScene(const QString &name)
{
	obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
	if (!source) return false;
	const bool valid = obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE;
	if (valid) obs_frontend_set_current_scene(source);
	obs_source_release(source);
	return valid;
}

void refreshScenes(QComboBox *select)
{
	const QString before = select->currentData().toString();
	const QSignalBlocker blocked(select);
	select->clear();
	select->addItem(QStringLiteral("Сцена не привязана"), QString{});
	obs_frontend_source_list scenes{};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		const QString name = QString::fromUtf8(obs_source_get_name(scenes.sources.array[i]));
		select->addItem(name, name);
	}
	obs_frontend_source_list_free(&scenes);
	if (const int index = select->findData(before); index >= 0) select->setCurrentIndex(index);
}

QString filePath()
{
	return QFileInfo(studioSettingsPath()).absoluteDir().filePath(QStringLiteral("game-profiles.json"));
}

bool validProfiles(const QJsonObject &root)
{
	if (root.value(QStringLiteral("version")).toInt() != 1 || !root.value(QStringLiteral("automatic")).isBool())
		return false;
	const auto profiles = root.value(QStringLiteral("profiles")).toArray();
	if (profiles.size() > 60) return false;
	for (const auto &item : profiles) {
		const auto profile = item.toObject();
		if (!item.isObject() || profile.value(QStringLiteral("name")).toString().trimmed().isEmpty()) return false;
		for (const auto &key : {"name", "scene", "title", "description", "twitchCategory", "kickCategory",
			"youtubeCategory", "youtubeBroadcast"}) {
			if (!profile.value(QString::fromLatin1(key)).isString() ||
			    profile.value(QString::fromLatin1(key)).toString().size() > 5000)
				return false;
		}
		for (const auto &key : {"twitchCategoryName", "kickCategoryName", "youtubeCategoryName", "youtubeBroadcastName"}) {
			const auto value = profile.value(QString::fromLatin1(key));
			if (!value.isUndefined() && (!value.isString() || value.toString().size() > 200)) return false;
		}
	}
	return true;
}

bool storeProfiles(const QJsonObject &root)
{
	if (!validProfiles(root)) return false;
	QSaveFile file(filePath());
	if (!file.open(QIODevice::WriteOnly)) return false;
	const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
	return bytes.size() < 65536 && file.write(bytes) == bytes.size() && file.commit();
}

void selectSaved(QComboBox *combo, const QString &id, const QString &label)
{
	if (!combo) return;
	const QSignalBlocker blocked(combo);
	const int found = combo->findData(id);
	if (found >= 0) { combo->setCurrentIndex(found); return; }
	if (!id.isEmpty()) combo->addItem(label.isEmpty() ? QStringLiteral("Сохранённая категория") : label, id);
	combo->setCurrentIndex(id.isEmpty() ? 0 : combo->count() - 1);
}

} // namespace

void installGameProfilesWorkspace(OBSBasic *mainWindow)
{
	auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"));
	auto *dock = studioDock(mainWindow);
	if (!gateway || !dock) return;
	auto *scroll = dock->findChild<QScrollArea *>(QStringLiteral("xinkxBroadcastEditor"));
	if (!scroll || !scroll->widget()) return;
	auto *panel = scroll->widget();
	auto *editorLayout = qobject_cast<QVBoxLayout *>(panel->layout());
	if (!editorLayout) return;
	auto *title = panel->findChild<QLineEdit *>(QStringLiteral("xinkxBroadcastTitle"));
	auto *description = panel->findChild<QPlainTextEdit *>(QStringLiteral("xinkxBroadcastDescription"));
	auto *twitch = panel->findChild<QComboBox *>(QStringLiteral("xinkxBroadcastTwitchGame"));
	auto *kick = panel->findChild<QComboBox *>(QStringLiteral("xinkxBroadcastKickGame"));
	auto *youtube = panel->findChild<QComboBox *>(QStringLiteral("xinkxBroadcastYouTubeGame"));
	auto *youtubeBroadcast = panel->findChild<QComboBox *>(QStringLiteral("xinkxBroadcastYouTubeEvent"));
	if (!title || !description || !twitch || !kick || !youtube || !youtubeBroadcast) return;

	auto root = std::make_shared<QJsonObject>(QJsonObject{{QStringLiteral("version"), 1},
		{QStringLiteral("automatic"), false}, {QStringLiteral("profiles"), QJsonArray{}}});
	QFile file(filePath());
	if (file.open(QIODevice::ReadOnly) && file.size() < 65536) {
		const auto json = QJsonDocument::fromJson(file.readAll());
		if (json.isObject() && validProfiles(json.object())) *root = json.object();
	}

	auto *group = new QGroupBox(QStringLiteral("Профили эфира"), panel);
	auto *layout = new QVBoxLayout(group);
	layout->setSpacing(5);
	auto *form = new QFormLayout();
	auto *profiles = new QComboBox(group);
	form->addRow(QStringLiteral("Заготовка"), profiles);
	auto *name = new QLineEdit(group);
	name->setMaxLength(120);
	name->setPlaceholderText(QStringLiteral("Название заготовки"));
	form->addRow(QStringLiteral("Имя профиля"), name);
	auto *scene = new QComboBox(group);
	form->addRow(QStringLiteral("Сцена OBS"), scene);
	layout->addLayout(form);
	auto *buttons = new QGridLayout();
	auto *newProfile = new QPushButton(QStringLiteral("Новый"), group);
	auto *saveProfile = new QPushButton(QStringLiteral("Сохранить"), group);
	auto *removeProfile = new QPushButton(QStringLiteral("Удалить"), group);
	auto *activate = new QPushButton(QStringLiteral("Применить"), group);
	buttons->addWidget(newProfile, 0, 0);
	buttons->addWidget(saveProfile, 0, 1);
	buttons->addWidget(removeProfile, 1, 0);
	buttons->addWidget(activate, 1, 1);
	layout->addLayout(buttons);
	auto *automatic = new QCheckBox(QStringLiteral("Обновлять площадки при переключении на привязанную сцену"), group);
	automatic->setChecked(root->value(QStringLiteral("automatic")).toBool());
	layout->addWidget(automatic);
	auto *note = new QLabel(QStringLiteral("Один редактор для текущего эфира и профиля. "
		"«Сохранить» запоминает поля ниже; «Применить» переключает сцену и обновляет площадки."), group);
	note->setWordWrap(true);
	layout->addWidget(note);
	editorLayout->insertWidget(0, group);

	const auto reload = [=] {
		const int selected = profiles->currentIndex();
		const QSignalBlocker blocked(profiles);
		profiles->clear();
		profiles->addItem(QStringLiteral("Новый профиль"), -1);
		const auto list = root->value(QStringLiteral("profiles")).toArray();
		for (int i = 0; i < list.size(); ++i)
			profiles->addItem(list.at(i).toObject().value(QStringLiteral("name")).toString(), i);
		profiles->setCurrentIndex(qBound(0, selected, profiles->count() - 1));
	};
	const auto load = [=] {
		const int index = profiles->currentData().toInt();
		const auto list = root->value(QStringLiteral("profiles")).toArray();
		if (index < 0 || index >= list.size()) {
			name->clear();
			refreshScenes(scene);
			if (const int current = scene->findData(currentScene()); current >= 0) scene->setCurrentIndex(current);
			return;
		}
		const auto profile = list.at(index).toObject();
		name->setText(profile.value(QStringLiteral("name")).toString());
		refreshScenes(scene);
		if (const int s = scene->findData(profile.value(QStringLiteral("scene")).toString()); s >= 0)
			scene->setCurrentIndex(s);
		title->setText(profile.value(QStringLiteral("title")).toString());
		description->setPlainText(profile.value(QStringLiteral("description")).toString());
		selectSaved(twitch, profile.value(QStringLiteral("twitchCategory")).toString(),
			profile.value(QStringLiteral("twitchCategoryName")).toString());
		selectSaved(kick, profile.value(QStringLiteral("kickCategory")).toString(),
			profile.value(QStringLiteral("kickCategoryName")).toString());
		selectSaved(youtube, profile.value(QStringLiteral("youtubeCategory")).toString(),
			profile.value(QStringLiteral("youtubeCategoryName")).toString());
		selectSaved(youtubeBroadcast, profile.value(QStringLiteral("youtubeBroadcast")).toString(),
			profile.value(QStringLiteral("youtubeBroadcastName")).toString());
	};
	QObject::connect(profiles, &QComboBox::currentIndexChanged, group, load);
	reload();
	load();
	QObject::connect(newProfile, &QPushButton::clicked, group, [=] {
		profiles->setCurrentIndex(0);
		name->clear();
		refreshScenes(scene);
		if (const int current = scene->findData(currentScene()); current >= 0) scene->setCurrentIndex(current);
	});
	QObject::connect(saveProfile, &QPushButton::clicked, group, [=] {
		if (name->text().trimmed().isEmpty()) { note->setText(QStringLiteral("Укажи название профиля")); return; }
		QJsonArray list = root->value(QStringLiteral("profiles")).toArray();
		const QJsonObject profile{{QStringLiteral("name"), name->text().trimmed()},
			{QStringLiteral("scene"), scene->currentData().toString()},
			{QStringLiteral("title"), title->text()},
			{QStringLiteral("description"), description->toPlainText().left(5000)},
			{QStringLiteral("twitchCategory"), twitch->currentData().toString()},
			{QStringLiteral("twitchCategoryName"), twitch->currentText()},
			{QStringLiteral("kickCategory"), kick->currentData().toString()},
			{QStringLiteral("kickCategoryName"), kick->currentText()},
			{QStringLiteral("youtubeCategory"), youtube->currentData().toString()},
			{QStringLiteral("youtubeCategoryName"), youtube->currentText()},
			{QStringLiteral("youtubeBroadcast"), youtubeBroadcast->currentData().toString()},
			{QStringLiteral("youtubeBroadcastName"), youtubeBroadcast->currentText()}};
		const int index = profiles->currentData().toInt();
		if (index >= 0 && index < list.size()) list.replace(index, profile);
		else list.append(profile);
		root->insert(QStringLiteral("profiles"), list);
		if (!storeProfiles(*root)) { note->setText(QStringLiteral("Ошибка сохранения game-profiles.json")); return; }
		reload();
		profiles->setCurrentIndex(index >= 0 && index < list.size() ? index + 1 : list.size());
		note->setText(QStringLiteral("Профиль сохранён"));
	});
	QObject::connect(removeProfile, &QPushButton::clicked, group, [=] {
		const int index = profiles->currentData().toInt();
		QJsonArray list = root->value(QStringLiteral("profiles")).toArray();
		if (index < 0 || index >= list.size()) return;
		list.removeAt(index);
		root->insert(QStringLiteral("profiles"), list);
		if (!storeProfiles(*root)) { note->setText(QStringLiteral("Ошибка удаления")); return; }
		reload();
		profiles->setCurrentIndex(0);
	});
	const auto apply = [=](const QJsonObject &profile) {
		const QString target = profile.value(QStringLiteral("scene")).toString();
		if (!target.isEmpty() && !useScene(target)) {
			note->setText(QStringLiteral("Не найдена сцена: ") + target);
			return;
		}
		for (const auto &account : gateway->accounts()) {
			const QString field = account.platform == QStringLiteral("twitch") ? QStringLiteral("twitchCategory") :
				account.platform == QStringLiteral("kick") ? QStringLiteral("kickCategory") :
				QStringLiteral("youtubeCategory");
			const QString category = profile.value(field).toString();
			const QString heading = profile.value(QStringLiteral("title")).toString();
			const QString description = profile.value(QStringLiteral("description")).toString();
			if (category.isEmpty() && heading.isEmpty() && description.isEmpty()) continue;
			gateway->updateBroadcast(account.id, profile.value(QStringLiteral("youtubeBroadcast")).toString(),
				heading, account.platform == QStringLiteral("kick") ? QString{} : description, category);
		}
	};
	QObject::connect(activate, &QPushButton::clicked, group, [=] {
		const int index = profiles->currentData().toInt();
		const QJsonArray list = root->value(QStringLiteral("profiles")).toArray();
		if (index >= 0 && index < list.size()) apply(list.at(index).toObject());
	});
	QObject::connect(automatic, &QCheckBox::toggled, group, [=](bool checked) {
		root->insert(QStringLiteral("automatic"), checked);
		if (!storeProfiles(*root)) note->setText(QStringLiteral("Не удалось сохранить автоматизацию"));
	});
	auto sceneWatch = std::make_shared<QString>(currentScene());
	auto *timer = new QTimer(group);
	timer->setInterval(850);
	QObject::connect(timer, &QTimer::timeout, group, [=] {
		const QString current = currentScene();
		if (current == *sceneWatch) return;
		*sceneWatch = current;
		if (!root->value(QStringLiteral("automatic")).toBool() || current.isEmpty()) return;
		for (const auto &entry : root->value(QStringLiteral("profiles")).toArray()) {
			const QJsonObject profile = entry.toObject();
			if (profile.value(QStringLiteral("scene")).toString() == current) { apply(profile); break; }
		}
	});
	timer->start();
}

} // namespace xinkx
