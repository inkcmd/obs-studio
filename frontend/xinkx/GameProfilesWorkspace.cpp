#include "ChatGateway.hpp"
#include "StudioWorkspace.hpp"
#include "StreamAccounts.hpp"

#include <widgets/OBSBasic.hpp>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {

namespace {

QString currentScene()
{
	obs_source_t *source = obs_frontend_get_current_scene();
	if (!source) return {};
	const QString result = QString::fromUtf8(obs_source_get_name(source));
	obs_source_release(source);
	return result;
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
	const auto current = select->currentData().toString();
	const QSignalBlocker blocked(select);
	select->clear();
	select->addItem(QStringLiteral("Выбери сцену OBS"), QString{});
	obs_frontend_source_list list{};
	obs_frontend_get_scenes(&list);
	for (size_t index = 0; index < list.sources.num; ++index) {
		const auto *source = list.sources.array[index];
		const auto name = QString::fromUtf8(obs_source_get_name(source));
		select->addItem(name, name);
	}
	obs_frontend_source_list_free(&list);
	if (const int index = select->findData(current); index >= 0) select->setCurrentIndex(index);
}

void chooseId(QComboBox *select, const QString &id, const QString &label)
{
	const QSignalBlocker blocked(select);
	select->clear();
	select->addItem(id.isEmpty() ? QStringLiteral("Не выбрано") :
		(label.isEmpty() ? QStringLiteral("Сохранённая категория · %1").arg(id) : label), id);
	select->setCurrentIndex(0);
}

QString filePath()
{
	return QFileInfo(studioSettingsPath()).absoluteDir().filePath(QStringLiteral("game-profiles.json"));
}

bool validProfiles(const QJsonObject &root)
{
	if (root.value(QStringLiteral("version")).toInt() != 1 || !root.value(QStringLiteral("automatic")).isBool()) return false;
	const auto profiles = root.value(QStringLiteral("profiles")).toArray();
	if (profiles.size() > 60) return false;
	for (const auto &entry : profiles) {
		const auto object = entry.toObject();
		if (!entry.isObject() || object.value(QStringLiteral("name")).toString().trimmed().isEmpty()) return false;
		for (const auto &key : {QStringLiteral("name"), QStringLiteral("scene"), QStringLiteral("title"),
			QStringLiteral("description"), QStringLiteral("twitchCategory"), QStringLiteral("kickCategory"),
			QStringLiteral("youtubeCategory"), QStringLiteral("youtubeBroadcast")}) {
			if (!object.value(key).isString() || object.value(key).toString().size() > 5000) return false;
		}
		for (const auto &key : {QStringLiteral("twitchCategoryName"), QStringLiteral("kickCategoryName"),
			QStringLiteral("youtubeCategoryName"), QStringLiteral("youtubeBroadcastName")}) {
			if (!object.value(key).isUndefined() &&
			    (!object.value(key).isString() || object.value(key).toString().size() > 200)) return false;
		}
	}
	return true;
}

bool storeProfiles(const QJsonObject &root)
{
	if (!validProfiles(root)) return false;
	QSaveFile file(filePath());
	if (!file.open(QIODevice::WriteOnly)) return false;
	const auto content = QJsonDocument(root).toJson(QJsonDocument::Indented);
	return content.size() < 65536 && file.write(content) == content.size() && file.commit();
}

} // namespace

void installGameProfilesWorkspace(OBSBasic *mainWindow)
{
	auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"));
	if (!gateway) return;
	auto root = std::make_shared<QJsonObject>(QJsonObject{{QStringLiteral("version"), 1},
		{QStringLiteral("automatic"), false}, {QStringLiteral("profiles"), QJsonArray{}}});
	QFile file(filePath());
	if (file.open(QIODevice::ReadOnly) && file.size() < 65536) {
		const auto parsed = QJsonDocument::fromJson(file.readAll());
		if (parsed.isObject() && validProfiles(parsed.object())) *root = parsed.object();
	}
	auto *dock = studioDock(mainWindow);
	if (!dock) return;
	auto *tabs = dock->findChild<QTabWidget *>(QStringLiteral("xinkxBroadcastTabs"));
	if (!tabs) return;
	auto *panel = new QWidget(dock);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(7);
	auto *profiles = new QComboBox(panel);
	layout->addWidget(profiles);
	auto *form = new QFormLayout();
	auto *name = new QLineEdit(panel);
	form->addRow(QStringLiteral("Название профиля"), name);
	auto *scene = new QComboBox(panel);
	form->addRow(QStringLiteral("Сцена OBS"), scene);
	auto *capture = new QPushButton(QStringLiteral("Текущая сцена"), panel);
	form->addRow(QString{}, capture);
	auto *title = new QLineEdit(panel);
	title->setMaxLength(100);
	form->addRow(QStringLiteral("Заголовок эфира"), title);
	auto *description = new QPlainTextEdit(panel);
	description->setMaximumHeight(85);
	form->addRow(QStringLiteral("Описание YouTube"), description);
	auto addCategorySearch = [=](const QString &platform, const QString &label) {
		auto *wrapper = new QWidget(panel);
		auto *column = new QVBoxLayout(wrapper);
		column->setContentsMargins(0, 0, 0, 0);
		column->setSpacing(3);
		auto *row = new QHBoxLayout();
		auto *searchInput = new QLineEdit(wrapper);
		searchInput->setPlaceholderText(QStringLiteral("Поиск игры или категории…"));
		auto *search = new QPushButton(QStringLiteral("Найти"), wrapper);
		row->addWidget(searchInput, 1);
		row->addWidget(search);
		column->addLayout(row);
		auto *results = new QComboBox(wrapper);
		results->addItem(QStringLiteral("Не выбрано"), QString{});
		column->addWidget(results);
		form->addRow(label, wrapper);
		auto fetch = [=] {
			for (const auto &account : gateway->accounts()) {
				if (account.platform != platform) continue;
				gateway->searchCategories(account.id, searchInput->text());
				return;
			}
		};
		QObject::connect(search, &QPushButton::clicked, dock, fetch);
		QObject::connect(searchInput, &QLineEdit::returnPressed, dock, fetch);
		QObject::connect(gateway, &ChatGateway::categoryResults, dock,
			[=](const QString &accountId, const QJsonArray &entries) {
				bool matching = false;
				for (const auto &account : gateway->accounts())
					if (account.id == accountId && account.platform == platform) matching = true;
				if (!matching) return;
				const auto previous = results->currentData().toString();
				const QSignalBlocker blocked(results);
				results->clear();
				results->addItem(QStringLiteral("Не выбрано"), QString{});
				for (const auto &entry : entries) {
					const auto item = entry.toObject();
					results->addItem(item.value(QStringLiteral("name")).toString(), item.value(QStringLiteral("id")).toString());
				}
				const int found = results->findData(previous);
				results->setCurrentIndex(found >= 0 ? found : 0);
			});
		return results;
	};
	auto *twitchGame = addCategorySearch(QStringLiteral("twitch"), QStringLiteral("Игра Twitch"));
	auto *kickGame = addCategorySearch(QStringLiteral("kick"), QStringLiteral("Категория Kick"));
	auto *youtubeGame = addCategorySearch(QStringLiteral("youtube"), QStringLiteral("Категория YouTube"));
	auto *youtubeBroadcast = new QComboBox(panel);
	youtubeBroadcast->addItem(QStringLiteral("Выбери эфир YouTube"), QString{});
	form->addRow(QStringLiteral("Эфир YouTube"), youtubeBroadcast);
	layout->addLayout(form);
	auto *loadPlatform = new QPushButton(QStringLiteral("Синхронизировать с Twitch / YouTube / Kick"), panel);
	layout->addWidget(loadPlatform);
	auto *autoSwitch = new QCheckBox(QStringLiteral("Автоматически обновлять данные площадок при переключении на сцену профиля"), panel);
	autoSwitch->setChecked(root->value(QStringLiteral("automatic")).toBool());
	layout->addWidget(autoSwitch);
	auto *buttons = new QHBoxLayout();
	auto *add = new QPushButton(QStringLiteral("Новый"), panel);
	auto *save = new QPushButton(QStringLiteral("Сохранить"), panel);
	auto *remove = new QPushButton(QStringLiteral("Удалить"), panel);
	buttons->addWidget(add);
	buttons->addWidget(save);
	buttons->addWidget(remove);
	layout->addLayout(buttons);
	auto *activate = new QPushButton(QStringLiteral("Активировать · сцена OBS и параметры площадок"), panel);
	layout->addWidget(activate);
	auto *json = new QPushButton(QStringLiteral("Редактировать профили JSON…"), panel);
	layout->addWidget(json);
	auto *note = new QLabel(QStringLiteral("Выбери сцену OBS, найди игры на площадках и сохрани профиль. ID получаются автоматически. Автоматизация выключена по умолчанию."), panel);
	note->setWordWrap(true);
	layout->addWidget(note);
	layout->addStretch();
	auto refresh = [=] {
		const QSignalBlocker block(profiles);
		const int index = profiles->currentIndex();
		profiles->clear();
		const auto data = root->value(QStringLiteral("profiles")).toArray();
		for (const auto &item : data) profiles->addItem(item.toObject().value(QStringLiteral("name")).toString());
		profiles->setCurrentIndex(qBound(-1, index, profiles->count() - 1));
	};
	auto readProfile = [=] {
		const int index = profiles->currentIndex();
		const auto list = root->value(QStringLiteral("profiles")).toArray();
		const auto entry = index >= 0 && index < list.size() ? list.at(index).toObject() : QJsonObject{};
		name->setText(entry.value(QStringLiteral("name")).toString());
		refreshScenes(scene);
		const int sceneIndex = scene->findData(entry.value(QStringLiteral("scene")).toString());
		scene->setCurrentIndex(sceneIndex < 0 ? 0 : sceneIndex);
		title->setText(entry.value(QStringLiteral("title")).toString());
		description->setPlainText(entry.value(QStringLiteral("description")).toString());
		chooseId(twitchGame, entry.value(QStringLiteral("twitchCategory")).toString(),
			entry.value(QStringLiteral("twitchCategoryName")).toString());
		chooseId(kickGame, entry.value(QStringLiteral("kickCategory")).toString(),
			entry.value(QStringLiteral("kickCategoryName")).toString());
		chooseId(youtubeGame, entry.value(QStringLiteral("youtubeCategory")).toString(),
			entry.value(QStringLiteral("youtubeCategoryName")).toString());
		chooseId(youtubeBroadcast, entry.value(QStringLiteral("youtubeBroadcast")).toString(),
			entry.value(QStringLiteral("youtubeBroadcastName")).toString());
	};
	QObject::connect(profiles, &QComboBox::currentIndexChanged, dock, readProfile);
	refresh();
	readProfile();
	QObject::connect(capture, &QPushButton::clicked, dock, [=] {
		refreshScenes(scene);
		const int index = scene->findData(currentScene());
		if (index >= 0) scene->setCurrentIndex(index);
	});
	QObject::connect(add, &QPushButton::clicked, dock, [=] {
		profiles->setCurrentIndex(-1);
		readProfile();
		capture->click();
	});
	auto pendingLoads = std::make_shared<QSet<QString>>();
	QObject::connect(loadPlatform, &QPushButton::clicked, dock, [=] {
		pendingLoads->clear();
		for (const auto &account : gateway->accounts()) {
			pendingLoads->insert(account.id);
			gateway->inspectBroadcast(account.id,
				account.platform == QStringLiteral("youtube") ? youtubeBroadcast->currentData().toString() : QString{});
		}
		note->setText(QStringLiteral("Загружаем действующие названия, категории и доступные эфиры…"));
	});
	QObject::connect(gateway, &ChatGateway::broadcastInfo, dock,
		[=](const QString &accountId, const QJsonObject &info) {
			const auto platform = info.value(QStringLiteral("platform")).toString();
			if (platform == QStringLiteral("youtube")) {
				const QString savedId = youtubeBroadcast->currentData().toString();
				const auto broadcasts = info.value(QStringLiteral("broadcasts")).toArray();
				const QSignalBlocker blocked(youtubeBroadcast);
				youtubeBroadcast->clear();
				youtubeBroadcast->addItem(QStringLiteral("Выбери эфир YouTube"), QString{});
				for (const auto &entry : broadcasts) {
					const auto item = entry.toObject();
					youtubeBroadcast->addItem(item.value(QStringLiteral("title")).toString(),
						item.value(QStringLiteral("id")).toString());
				}
				const QString selected = savedId.isEmpty() ? info.value(QStringLiteral("id")).toString() : savedId;
				const int index = youtubeBroadcast->findData(selected);
				if (index >= 0) youtubeBroadcast->setCurrentIndex(index);
			}
			if (!pendingLoads->remove(accountId)) return;
			const auto heading = info.value(QStringLiteral("title")).toString();
			if (!heading.isEmpty()) title->setText(heading);
			if (platform == QStringLiteral("youtube"))
				description->setPlainText(info.value(QStringLiteral("description")).toString());
			QComboBox *target = platform == QStringLiteral("twitch") ? twitchGame :
				platform == QStringLiteral("kick") ? kickGame : youtubeGame;
			chooseId(target, info.value(QStringLiteral("categoryId")).toVariant().toString(),
				info.value(QStringLiteral("categoryName")).toString());
			if (platform == QStringLiteral("youtube"))
				gateway->searchCategories(accountId, QString{});
			if (pendingLoads->isEmpty()) note->setText(QStringLiteral("Данные площадок загружены. Выбери профиль и сохрани."));
		});
	QObject::connect(gateway, &ChatGateway::broadcastResult, dock,
		[=](const QString &id, const QString &message, bool success) {
			if (!pendingLoads->remove(id)) return;
			note->setText(success ? message : QStringLiteral("Не удалось получить данные: ") + message);
		});
	QObject::connect(autoSwitch, &QCheckBox::toggled, dock, [=](bool value) {
		root->insert(QStringLiteral("automatic"), value);
		if (!storeProfiles(*root)) note->setText(QStringLiteral("Ошибка сохранения автоматизации"));
	});
	QObject::connect(save, &QPushButton::clicked, dock, [=] {
		if (name->text().trimmed().isEmpty()) { note->setText(QStringLiteral("Укажи имя профиля")); return; }
		QJsonArray list = root->value(QStringLiteral("profiles")).toArray();
		const QJsonObject entry{{QStringLiteral("name"), name->text().trimmed().left(120)},
			{QStringLiteral("scene"), scene->currentData().toString()},
			{QStringLiteral("title"), title->text()},
			{QStringLiteral("description"), description->toPlainText().left(5000)},
			{QStringLiteral("twitchCategory"), twitchGame->currentData().toString()},
			{QStringLiteral("twitchCategoryName"), twitchGame->currentText()},
			{QStringLiteral("kickCategory"), kickGame->currentData().toString()},
			{QStringLiteral("kickCategoryName"), kickGame->currentText()},
			{QStringLiteral("youtubeCategory"), youtubeGame->currentData().toString()},
			{QStringLiteral("youtubeCategoryName"), youtubeGame->currentText()},
			{QStringLiteral("youtubeBroadcast"), youtubeBroadcast->currentData().toString()},
			{QStringLiteral("youtubeBroadcastName"), youtubeBroadcast->currentText()}};
		const int index = profiles->currentIndex();
		if (index >= 0 && index < list.size()) list.replace(index, entry);
		else list.append(entry);
		root->insert(QStringLiteral("profiles"), list);
		if (!storeProfiles(*root)) { note->setText(QStringLiteral("Не удалось сохранить game-profiles.json")); return; }
		refresh();
		profiles->setCurrentIndex(index >= 0 ? index : list.size() - 1);
		note->setText(QStringLiteral("Профиль сохранён"));
	});
	QObject::connect(remove, &QPushButton::clicked, dock, [=] {
		const int index = profiles->currentIndex();
		QJsonArray list = root->value(QStringLiteral("profiles")).toArray();
		if (index < 0 || index >= list.size()) return;
		list.removeAt(index);
		root->insert(QStringLiteral("profiles"), list);
		if (!storeProfiles(*root)) { note->setText(QStringLiteral("Ошибка удаления профиля")); return; }
		refresh(); readProfile();
	});
	auto applyProfile = [=](const QJsonObject &entry) {
		const auto linkedScene = entry.value(QStringLiteral("scene")).toString();
		if (!linkedScene.isEmpty() && !useScene(linkedScene)) {
			note->setText(QStringLiteral("Сцена OBS не найдена: ") + linkedScene);
			return;
		}
		for (const auto &account : gateway->accounts()) {
			const auto category = entry.value(account.platform == QStringLiteral("twitch") ? QStringLiteral("twitchCategory")
				: account.platform == QStringLiteral("kick") ? QStringLiteral("kickCategory")
				: QStringLiteral("youtubeCategory")).toString();
			if (category.trimmed().isEmpty() && entry.value(QStringLiteral("title")).toString().trimmed().isEmpty() &&
			    (account.platform != QStringLiteral("youtube") ||
			     entry.value(QStringLiteral("description")).toString().trimmed().isEmpty())) continue;
			gateway->updateBroadcast(account.id, entry.value(QStringLiteral("youtubeBroadcast")).toString(),
				entry.value(QStringLiteral("title")).toString(),
				account.platform == QStringLiteral("youtube") ? entry.value(QStringLiteral("description")).toString() : QString{},
				category);
		}
	};
	QObject::connect(activate, &QPushButton::clicked, dock, [=] {
		const int index = profiles->currentIndex();
		const auto list = root->value(QStringLiteral("profiles")).toArray();
		if (index >= 0 && index < list.size()) applyProfile(list.at(index).toObject());
	});
	QObject::connect(json, &QPushButton::clicked, dock, [=] {
		QDialog dialog(dock);
		dialog.setWindowTitle(QStringLiteral("Профили игр и сцен · JSON"));
		dialog.resize(800, 620);
		auto *box = new QVBoxLayout(&dialog);
		auto *edit = new QPlainTextEdit(&dialog);
		edit->setPlainText(QString::fromUtf8(QJsonDocument(*root).toJson(QJsonDocument::Indented)));
		box->addWidget(edit);
		auto *commit = new QPushButton(QStringLiteral("Проверить и сохранить"), &dialog);
		box->addWidget(commit);
		QObject::connect(commit, &QPushButton::clicked, &dialog, [=, &dialog] {
			const auto parsed = QJsonDocument::fromJson(edit->toPlainText().toUtf8());
			if (!parsed.isObject() || !storeProfiles(parsed.object())) {
				QMessageBox::warning(&dialog, QStringLiteral("JSON"), QStringLiteral("Схема некорректна или ошибка записи"));
				return;
			}
			*root = parsed.object();
			autoSwitch->setChecked(root->value(QStringLiteral("automatic")).toBool());
			refresh(); readProfile();
			dialog.accept();
		});
		dialog.exec();
	});
	auto previousScene = std::make_shared<QString>(currentScene());
	auto *watch = new QTimer(dock);
	watch->setInterval(800);
	QObject::connect(watch, &QTimer::timeout, dock, [=] {
		const auto now = currentScene();
		if (now == *previousScene) return;
		*previousScene = now;
		if (!root->value(QStringLiteral("automatic")).toBool()) return;
		for (const auto &value : root->value(QStringLiteral("profiles")).toArray()) {
			const auto entry = value.toObject();
			if (entry.value(QStringLiteral("scene")).toString() == now && !now.isEmpty()) {
				applyProfile(entry);
				break;
			}
		}
	});
	watch->start();
	auto *scroll = new QScrollArea(dock);
	scroll->setWidgetResizable(true);
	scroll->setWidget(panel);
	tabs->addTab(scroll, QStringLiteral("Профили игр и сцен"));
	auto opened = std::make_shared<bool>(false);
	QObject::connect(tabs, &QTabWidget::currentChanged, dock, [=](int index) {
		if (tabs->widget(index) != scroll) return;
		if (scene->count() < 2) {
			refreshScenes(scene);
			const auto data = root->value(QStringLiteral("profiles")).toArray();
			const int selected = profiles->currentIndex();
			if (selected >= 0 && selected < data.size()) {
				const int found = scene->findData(data.at(selected).toObject().value(QStringLiteral("scene")).toString());
				if (found >= 0) scene->setCurrentIndex(found);
			}
		}
		if (*opened) return;
		*opened = true;
		if (profiles->count() == 0) loadPlatform->click();
		else for (const auto &account : gateway->accounts())
			if (account.platform == QStringLiteral("youtube"))
				gateway->inspectBroadcast(account.id, youtubeBroadcast->currentData().toString());
	});
}

} // namespace xinkx
