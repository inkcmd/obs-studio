#include "ChatGateway.hpp"
#include "BroadcastTagsEditor.hpp"
#include "StudioWorkspace.hpp"
#include "StreamAccounts.hpp"

#include <widgets/OBSBasic.hpp>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {
namespace {

QString currentSceneId()
{
	obs_source_t *source = obs_frontend_get_current_scene();
	if (!source) return {};
	const QString id = QString::fromUtf8(obs_source_get_uuid(source));
	obs_source_release(source);
	return id;
}

bool useScene(const QString &uuid, const QString &legacyName)
{
	if (uuid.isEmpty() && legacyName.isEmpty()) return true;
	obs_source_t *source = uuid.isEmpty() ? nullptr : obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!source && !legacyName.isEmpty()) source = obs_get_source_by_name(legacyName.toUtf8().constData());
	if (!source) return false;
	const bool valid = obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE;
	if (valid) obs_frontend_set_current_scene(source);
	obs_source_release(source);
	return valid;
}

void refreshScenes(QComboBox *select)
{
	const QString previous = select->currentData().toString();
	obs_frontend_source_list scenes{};
	obs_frontend_get_scenes(&scenes);
	const QSignalBlocker blocked(select);
	select->clear();
	select->addItem(QStringLiteral("Сцена не привязана"), QString{});
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *source = scenes.sources.array[i];
		select->addItem(QString::fromUtf8(obs_source_get_name(source)),
			QString::fromUtf8(obs_source_get_uuid(source)));
	}
	obs_frontend_source_list_free(&scenes);
	if (const int index = select->findData(previous); index >= 0) select->setCurrentIndex(index);
}

// OBS populates scene collections after XINKX installs its workspace. Refresh
// at popup time as well as when the initial scene becomes available.
class SceneSelector final : public QComboBox {
public:
	using QComboBox::QComboBox;
	void showPopup() override
	{
		refreshScenes(this);
		QComboBox::showPopup();
	}
};

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
	for (const auto &entry : profiles) {
		if (!entry.isObject()) return false;
		const auto object = entry.toObject();
		if (object.value(QStringLiteral("name")).toString().trimmed().isEmpty()) return false;
		for (const auto &key : {"name", "scene", "title", "description", "twitchCategory", "kickCategory",
			"youtubeCategory", "youtubeBroadcast"})
			if (!object.value(QString::fromLatin1(key)).isString() ||
			    object.value(QString::fromLatin1(key)).toString().size() > 5000) return false;
		for (const auto &key : {"twitchCategoryName", "kickCategoryName", "youtubeCategoryName",
			"youtubeBroadcastName"}) {
			const auto item = object.value(QString::fromLatin1(key));
			if (!item.isUndefined() && (!item.isString() || item.toString().size() > 1024)) return false;
		}
		for (const auto &key : {"sceneUuid", "game"}) {
			const auto item = object.value(QString::fromLatin1(key));
			if (!item.isUndefined() && (!item.isString() || item.toString().size() > 160)) return false;
		}
		for (const auto &key : {"tags", "youtubeTags"}) {
			const QJsonValue value = object.value(QString::fromLatin1(key));
			if (value.isUndefined()) continue; // Profiles written before tag support.
			if (!value.isArray()) return false;
			const QJsonArray tags = value.toArray();
			if (tags.size() > 500 || (QString::fromLatin1(key) == QStringLiteral("tags") && tags.size() > 10))
				return false;
			for (const QJsonValue &tag : tags)
				if (!tag.isString() || tag.toString().size() > 500) return false;
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

void selectSaved(QComboBox *combo, const QString &id, const QString &name)
{
	const QSignalBlocker blocked(combo);
	int index = combo->findData(id);
	if (index < 0 && !id.isEmpty()) {
		combo->addItem(name.isEmpty() ? QStringLiteral("Сохранённая категория") : name, id);
		index = combo->count() - 1;
	}
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

} // namespace

void installGameProfilesWorkspace(OBSBasic *mainWindow)
{
	auto *dock = studioDock(mainWindow);
	if (!dock) return;
	auto *scroll = dock->findChild<QScrollArea *>(QStringLiteral("xinkxBroadcastEditor"));
	if (!scroll || !scroll->widget()) return;
	auto *panel = scroll->widget();
	auto *editor = qobject_cast<QVBoxLayout *>(panel->layout());
	if (!editor) return;
	auto *title = panel->findChild<QLineEdit *>(QStringLiteral("xinkxBroadcastTitle"));
	auto *description = panel->findChild<QPlainTextEdit *>(QStringLiteral("xinkxBroadcastDescription"));
	auto *tags = static_cast<BroadcastTagsEditor *>(panel->findChild<QWidget *>(QStringLiteral("xinkxBroadcastTags")));
	auto *youtubeTags = static_cast<BroadcastTagsEditor *>(
		panel->findChild<QWidget *>(QStringLiteral("xinkxBroadcastYouTubeTags")));
	auto *game = panel->findChild<QLineEdit *>(QStringLiteral("xinkxBroadcastGame"));
	auto *twitch = panel->findChild<QComboBox *>(QStringLiteral("xinkxBroadcastTwitchGame"));
	auto *kick = panel->findChild<QComboBox *>(QStringLiteral("xinkxBroadcastKickGame"));
	auto *youtube = panel->findChild<QComboBox *>(QStringLiteral("xinkxBroadcastYouTubeGame"));
	auto *youtubeEvent = panel->findChild<QComboBox *>(QStringLiteral("xinkxBroadcastYouTubeEvent"));
	auto *save = panel->findChild<QPushButton *>(QStringLiteral("xinkxBroadcastSave"));
	if (!title || !description || !tags || !youtubeTags || !game || !twitch || !kick || !youtube || !youtubeEvent || !save)
		return;

	auto root = std::make_shared<QJsonObject>(QJsonObject{{QStringLiteral("version"), 1},
		{QStringLiteral("automatic"), false}, {QStringLiteral("profiles"), QJsonArray{}}});
	QFile file(filePath());
	if (file.open(QIODevice::ReadOnly) && file.size() < 65536) {
		const auto json = QJsonDocument::fromJson(file.readAll());
		if (json.isObject() && validProfiles(json.object())) *root = json.object();
	}
	// Explicit Save is the only action that writes remotely. Migrate legacy
	// automatic scene-change profiles to manual mode without deleting them.
	root->insert(QStringLiteral("automatic"), false);

	auto *group = new QGroupBox(QStringLiteral("Профиль эфира"), panel);
	auto *layout = new QVBoxLayout(group);
	layout->setSpacing(6);
	auto *form = new QFormLayout();
	auto *profiles = new QComboBox(group);
	form->addRow(QStringLiteral("Профиль"), profiles);
	auto *name = new QLineEdit(group);
	name->setMaxLength(120);
	name->setPlaceholderText(QStringLiteral("Название профиля"));
	form->addRow(QStringLiteral("Имя"), name);
	auto *scene = new SceneSelector(group);
	form->addRow(QStringLiteral("Сцена OBS"), scene);
	layout->addLayout(form);
	auto *actions = new QHBoxLayout();
	auto *newProfile = new QPushButton(QStringLiteral("Новый профиль"), group);
	auto *removeProfile = new QPushButton(QStringLiteral("Удалить профиль"), group);
	actions->addWidget(newProfile);
	actions->addWidget(removeProfile);
	layout->addLayout(actions);
	auto *note = new QLabel(QStringLiteral("«Сохранить» внизу сохранит профиль, активирует его сцену и обновит площадки."), group);
	note->setWordWrap(true);
	layout->addWidget(note);
	editor->insertWidget(0, group);

	const auto reload = [=] {
		const int previous = profiles->count() ? profiles->currentData().toInt() : -1;
		const QString preferred = root->value(QStringLiteral("lastSelectedProfile")).toString();
		const QSignalBlocker blocked(profiles);
		profiles->clear();
		profiles->addItem(QStringLiteral("Новый профиль"), -1);
		const auto list = root->value(QStringLiteral("profiles")).toArray();
		for (int i = 0; i < list.size(); ++i)
			profiles->addItem(list.at(i).toObject().value(QStringLiteral("name")).toString(), i);
		int index = profiles->findData(previous);
		if (index < 0 || (previous < 0 && !preferred.isEmpty())) {
			for (int i = 1; i < profiles->count(); ++i)
				if (profiles->itemText(i) == preferred) { index = i; break; }
		}
		if (index == 0 && preferred.isEmpty() && profiles->count() > 1) index = 1;
		profiles->setCurrentIndex(index >= 0 ? index : 0);
	};
	const auto load = [=] {
		const auto list = root->value(QStringLiteral("profiles")).toArray();
		const int index = profiles->currentData().toInt();
		refreshScenes(scene);
		if (index < 0 || index >= list.size()) {
			name->clear();
			if (const int current = scene->findData(currentSceneId()); current >= 0) scene->setCurrentIndex(current);
			return;
		}
		const auto profile = list.at(index).toObject();
		name->setText(profile.value(QStringLiteral("name")).toString());
		const QString uuid = profile.value(QStringLiteral("sceneUuid")).toString();
		int sceneIndex = uuid.isEmpty() ? -1 : scene->findData(uuid);
		if (sceneIndex < 0) sceneIndex = scene->findText(profile.value(QStringLiteral("scene")).toString());
		if (sceneIndex >= 0) scene->setCurrentIndex(sceneIndex);
		title->setText(profile.value(QStringLiteral("title")).toString());
		description->setPlainText(profile.value(QStringLiteral("description")).toString());
		const auto readTags = [](const QJsonArray &items) {
			QStringList values;
			for (const auto &tag : items) values << tag.toString();
			return values;
		};
		if (profile.contains(QStringLiteral("tags")))
			tags->setTags(readTags(profile.value(QStringLiteral("tags")).toArray()));
		if (profile.contains(QStringLiteral("youtubeTags")))
			youtubeTags->setTags(readTags(profile.value(QStringLiteral("youtubeTags")).toArray()));
		game->setText(profile.value(QStringLiteral("game")).toString());
		selectSaved(twitch, profile.value(QStringLiteral("twitchCategory")).toString(),
			profile.value(QStringLiteral("twitchCategoryName")).toString());
		selectSaved(kick, profile.value(QStringLiteral("kickCategory")).toString(),
			profile.value(QStringLiteral("kickCategoryName")).toString());
		selectSaved(youtube, profile.value(QStringLiteral("youtubeCategory")).toString(),
			profile.value(QStringLiteral("youtubeCategoryName")).toString());
		selectSaved(youtubeEvent, profile.value(QStringLiteral("youtubeBroadcast")).toString(),
			profile.value(QStringLiteral("youtubeBroadcastName")).toString());
	};
	QObject::connect(profiles, &QComboBox::currentIndexChanged, group, load);
	QObject::connect(profiles, &QComboBox::currentIndexChanged, group, [=] {
		const int index = profiles->currentData().toInt();
		const QJsonArray available = root->value(QStringLiteral("profiles")).toArray();
		if (index < 0 || index >= available.size()) return;
		root->insert(QStringLiteral("lastSelectedProfile"), available.at(index).toObject()
			.value(QStringLiteral("name")).toString());
		storeProfiles(*root);
	});
	reload();
	load();
	// At startup obs_frontend_get_scenes() can return an empty list while
	// OBS is still loading the scene collection. Do not require "New profile".
	auto *awaitScenes = new QTimer(group);
	awaitScenes->setInterval(400);
	auto attempts = std::make_shared<int>(0);
	QObject::connect(awaitScenes, &QTimer::timeout, group, [=] {
		if (scene->count() > 1 || ++*attempts > 75) { awaitScenes->stop(); return; }
		load();
		if (scene->count() > 1) awaitScenes->stop();
	});
	awaitScenes->start();
	QObject::connect(newProfile, &QPushButton::clicked, group, [=] {
		profiles->setCurrentIndex(0);
		name->clear();
		refreshScenes(scene);
		if (const int current = scene->findData(currentSceneId()); current >= 0) scene->setCurrentIndex(current);
	});
	QObject::connect(removeProfile, &QPushButton::clicked, group, [=] {
		const int index = profiles->currentData().toInt();
		QJsonArray list = root->value(QStringLiteral("profiles")).toArray();
		if (index < 0 || index >= list.size()) return;
		list.removeAt(index);
		root->insert(QStringLiteral("profiles"), list);
		root->remove(QStringLiteral("lastSelectedProfile"));
		if (!storeProfiles(*root)) { note->setText(QStringLiteral("Ошибка удаления профиля")); return; }
		reload();
		profiles->setCurrentIndex(0);
	});
	QObject::connect(save, &QPushButton::clicked, group, [=] {
		save->setProperty("xinkxProfileReady", true);
		const QString uuid = scene->currentData().toString();
		if (!useScene(uuid, uuid.isEmpty() ? QString{} : scene->currentText())) {
			save->setProperty("xinkxProfileReady", false);
			note->setText(QStringLiteral("Сцена не найдена — профиль не сохранён"));
			return;
		}
		// A blank new profile is an ad-hoc broadcast. Do not create one silently.
		if (name->text().trimmed().isEmpty()) return;
		QJsonArray list = root->value(QStringLiteral("profiles")).toArray();
		const QJsonObject profile{
			{QStringLiteral("name"), name->text().trimmed()},
			{QStringLiteral("scene"), scene->currentData().toString().isEmpty() ? QString{} : scene->currentText()},
			{QStringLiteral("sceneUuid"), scene->currentData().toString()},
			{QStringLiteral("title"), title->text()},
			{QStringLiteral("description"), description->toPlainText().left(5000)},
			{QStringLiteral("tags"), QJsonArray::fromStringList(tags->tags())},
			{QStringLiteral("youtubeTags"), QJsonArray::fromStringList(youtubeTags->tags())},
			{QStringLiteral("game"), game->text().left(150)},
			{QStringLiteral("twitchCategory"), twitch->currentData().toString()},
			{QStringLiteral("twitchCategoryName"), twitch->currentText()},
			{QStringLiteral("kickCategory"), kick->currentData().toString()},
			{QStringLiteral("kickCategoryName"), kick->currentText()},
			{QStringLiteral("youtubeCategory"), youtube->currentData().toString()},
			{QStringLiteral("youtubeCategoryName"), youtube->currentText()},
			{QStringLiteral("youtubeBroadcast"), youtubeEvent->currentData().toString()},
			{QStringLiteral("youtubeBroadcastName"), youtubeEvent->currentText()}};
		const int index = profiles->currentData().toInt();
		if (index >= 0 && index < list.size()) list.replace(index, profile);
		else list.append(profile);
		root->insert(QStringLiteral("profiles"), list);
		root->insert(QStringLiteral("lastSelectedProfile"), name->text().trimmed());
		if (!storeProfiles(*root)) {
			save->setProperty("xinkxProfileReady", false);
			note->setText(QStringLiteral("Ошибка сохранения game-profiles.json"));
			return;
		}
		reload();
		profiles->setCurrentIndex(index >= 0 && index < list.size() ? index + 1 : list.size());
		note->setText(QStringLiteral("Профиль сохранён"));
	});
	if (auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"))) {
		QObject::connect(gateway, &ChatGateway::broadcastResult, group,
			[=](const QString &accountId, const QString &, bool success) {
				if (!success) return;
				bool youtubeAccount = false;
				for (const auto &account : gateway->accounts())
					if (account.id == accountId && account.platform == QStringLiteral("youtube"))
						youtubeAccount = true;
				if (!youtubeAccount) return;
				const int index = profiles->currentData().toInt();
				QJsonArray saved = root->value(QStringLiteral("profiles")).toArray();
				if (index < 0 || index >= saved.size()) return;
				QJsonObject profile = saved.at(index).toObject();
				if (youtubeEvent->currentData().toString().isEmpty() ||
				    profile.value(QStringLiteral("youtubeBroadcast")).toString() != youtubeEvent->currentData().toString())
					return;
				profile.insert(QStringLiteral("youtubeBroadcastName"), youtubeEvent->currentText());
				saved.replace(index, profile);
				QJsonObject updated = *root;
				updated.insert(QStringLiteral("profiles"), saved);
				if (storeProfiles(updated)) *root = updated;
			});
	}
}

} // namespace xinkx
