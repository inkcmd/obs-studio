#include "ChatGateway.hpp"
#include "StreamAccounts.hpp"

#include <widgets/OBSBasic.hpp>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
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
	auto *dock = new QDockWidget(QStringLiteral("Профили игр и сцен"), mainWindow);
	dock->setObjectName(QStringLiteral("xinkxGameProfilesDock"));
	auto *panel = new QWidget(dock);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(7);
	auto *profiles = new QComboBox(panel);
	layout->addWidget(profiles);
	auto *form = new QFormLayout();
	auto *name = new QLineEdit(panel);
	form->addRow(QStringLiteral("Название профиля"), name);
	auto *scene = new QLineEdit(panel);
	form->addRow(QStringLiteral("Сцена OBS"), scene);
	auto *capture = new QPushButton(QStringLiteral("Текущая сцена"), panel);
	form->addRow(QString{}, capture);
	auto *title = new QLineEdit(panel);
	title->setMaxLength(100);
	form->addRow(QStringLiteral("Заголовок эфира"), title);
	auto *description = new QPlainTextEdit(panel);
	description->setMaximumHeight(85);
	form->addRow(QStringLiteral("Описание YouTube"), description);
	auto *twitchGame = new QLineEdit(panel);
	form->addRow(QStringLiteral("Игра Twitch · ID"), twitchGame);
	auto *kickGame = new QLineEdit(panel);
	form->addRow(QStringLiteral("Категория Kick · ID"), kickGame);
	auto *youtubeGame = new QLineEdit(panel);
	form->addRow(QStringLiteral("Категория YouTube · ID"), youtubeGame);
	auto *youtubeBroadcast = new QLineEdit(panel);
	form->addRow(QStringLiteral("Эфир YouTube · ID"), youtubeBroadcast);
	layout->addLayout(form);
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
	auto *note = new QLabel(QStringLiteral("Профили ссылаются на существующие сцены OBS. При сохранении не создаётся второй граф сцены. Автоматизация выключена по умолчанию."), panel);
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
		scene->setText(entry.value(QStringLiteral("scene")).toString());
		title->setText(entry.value(QStringLiteral("title")).toString());
		description->setPlainText(entry.value(QStringLiteral("description")).toString());
		twitchGame->setText(entry.value(QStringLiteral("twitchCategory")).toString());
		kickGame->setText(entry.value(QStringLiteral("kickCategory")).toString());
		youtubeGame->setText(entry.value(QStringLiteral("youtubeCategory")).toString());
		youtubeBroadcast->setText(entry.value(QStringLiteral("youtubeBroadcast")).toString());
	};
	QObject::connect(profiles, &QComboBox::currentIndexChanged, dock, readProfile);
	refresh();
	readProfile();
	QObject::connect(capture, &QPushButton::clicked, dock, [=] { scene->setText(currentScene()); });
	QObject::connect(add, &QPushButton::clicked, dock, [=] {
		profiles->setCurrentIndex(-1);
		readProfile();
		scene->setText(currentScene());
	});
	QObject::connect(autoSwitch, &QCheckBox::toggled, dock, [=](bool value) {
		root->insert(QStringLiteral("automatic"), value);
		if (!storeProfiles(*root)) note->setText(QStringLiteral("Ошибка сохранения автоматизации"));
	});
	QObject::connect(save, &QPushButton::clicked, dock, [=] {
		if (name->text().trimmed().isEmpty()) { note->setText(QStringLiteral("Укажи имя профиля")); return; }
		QJsonArray list = root->value(QStringLiteral("profiles")).toArray();
		const QJsonObject entry{{QStringLiteral("name"), name->text().trimmed().left(120)},
			{QStringLiteral("scene"), scene->text().trimmed()},
			{QStringLiteral("title"), title->text()},
			{QStringLiteral("description"), description->toPlainText().left(5000)},
			{QStringLiteral("twitchCategory"), twitchGame->text()},
			{QStringLiteral("kickCategory"), kickGame->text()},
			{QStringLiteral("youtubeCategory"), youtubeGame->text()},
			{QStringLiteral("youtubeBroadcast"), youtubeBroadcast->text()}};
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
	dock->setWidget(scroll);
	mainWindow->AddDockWidget(dock, Qt::RightDockWidgetArea);
	if (auto *presentation = mainWindow->findChild<QDockWidget *>(QStringLiteral("xinkxPresentationDock")))
		mainWindow->tabifyDockWidget(presentation, dock);
	dock->show();
}

} // namespace xinkx
