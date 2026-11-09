#include "ChatGateway.hpp"
#include "StudioWorkspace.hpp"
#include "StudioDiagnostics.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {
namespace {

QString editorKey(const QString &part)
{
	return QStringLiteral("broadcast/editor/") + part;
}

void selectCategory(QComboBox *choice, const QString &id, const QString &name)
{
	if (!choice) return;
	const QSignalBlocker blocked(choice);
	int index = choice->findData(id);
	if (index < 0 && !id.isEmpty()) {
		choice->addItem(name.isEmpty() ? QStringLiteral("Категория %1").arg(id) : name, id);
		index = choice->count() - 1;
	}
	choice->setCurrentIndex(index >= 0 ? index : 0);
}

QString brief(const QString &value)
{
	return value.simplified().left(90);
}

} // namespace

void installBroadcastWorkspace(OBSBasic *mainWindow)
{
	auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"));
	auto *dock = studioDock(mainWindow);
	if (!gateway || !dock) return;

	auto *panel = new QWidget(dock);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(7);
	auto *local = new QSettings(studioSettingsPath(), QSettings::IniFormat, panel);
	auto *localFlush = new QTimer(panel);
	localFlush->setSingleShot(true);
	localFlush->setInterval(500);
	QObject::connect(localFlush, &QTimer::timeout, panel, [local] { local->sync(); });

	auto *intro = new QLabel(QStringLiteral(
		"Одно название, описание и игра для Twitch / YouTube / Kick. Поля хранятся на ПК; "
		"обновление площадок их не сбрасывает."), panel);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	auto *form = new QFormLayout();
	form->setVerticalSpacing(6);
	auto *title = new QLineEdit(panel);
	title->setObjectName(QStringLiteral("xinkxBroadcastTitle"));
	title->setMaxLength(100);
	title->setPlaceholderText(QStringLiteral("Название для всех площадок"));
	title->setText(local->value(editorKey(QStringLiteral("title"))).toString());
	form->addRow(QStringLiteral("Название"), title);
	auto *description = new QPlainTextEdit(panel);
	description->setObjectName(QStringLiteral("xinkxBroadcastDescription"));
	description->setMaximumHeight(95);
	description->setPlaceholderText(QStringLiteral("Описание для YouTube; на Twitch — описание канала. Kick не поддерживает запись описания."));
	description->setPlainText(local->value(editorKey(QStringLiteral("description"))).toString());
	form->addRow(QStringLiteral("Описание"), description);
	auto *game = new QLineEdit(panel);
	game->setObjectName(QStringLiteral("xinkxBroadcastGame"));
	game->setPlaceholderText(QStringLiteral("Название игры для всех площадок"));
	game->setText(local->value(editorKey(QStringLiteral("game"))).toString());
	form->addRow(QStringLiteral("Игра"), game);
	layout->addLayout(form);

	auto *searchRow = new QHBoxLayout();
	auto *search = new QPushButton(QStringLiteral("Найти игру на Twitch / Kick"), panel);
	searchRow->addWidget(search);
	layout->addLayout(searchRow);

	auto *categories = new QFormLayout();
	auto *twitchCategory = new QComboBox(panel);
	twitchCategory->setObjectName(QStringLiteral("xinkxBroadcastTwitchGame"));
	auto *kickCategory = new QComboBox(panel);
	kickCategory->setObjectName(QStringLiteral("xinkxBroadcastKickGame"));
	auto *youtubeCategory = new QComboBox(panel);
	youtubeCategory->setObjectName(QStringLiteral("xinkxBroadcastYouTubeGame"));
	for (auto *choice : {twitchCategory, kickCategory, youtubeCategory})
		choice->addItem(QStringLiteral("Не выбрана"), QString{});
	const auto platformLabel = [panel](const QString &platform, const QString &caption) {
		auto *row = new QWidget(panel);
		auto *line = new QHBoxLayout(row);
		line->setContentsMargins(0, 0, 4, 0);
		line->setSpacing(5);
		auto *icon = new QLabel(row);
		const QPixmap image(QStringLiteral(":/xinkx/%1.svg").arg(platform));
		if (!image.isNull()) icon->setPixmap(image.scaled(17, 17, Qt::KeepAspectRatio, Qt::SmoothTransformation));
		line->addWidget(icon);
		line->addWidget(new QLabel(caption, row));
		return row;
	};
	categories->addRow(platformLabel(QStringLiteral("twitch"), QStringLiteral("Twitch")), twitchCategory);
	categories->addRow(platformLabel(QStringLiteral("kick"), QStringLiteral("Kick")), kickCategory);
	categories->addRow(platformLabel(QStringLiteral("youtube"), QStringLiteral("YouTube")), youtubeCategory);
	layout->addLayout(categories);

	// Platform IDs are never interchangeable. Cache each selected category locally
	// so asynchronous HTTP responses cannot reset a user's current choice.
	const auto categoryChoices = std::make_shared<QHash<QString, QComboBox *>>();
	categoryChoices->insert(QStringLiteral("twitch"), twitchCategory);
	categoryChoices->insert(QStringLiteral("kick"), kickCategory);
	categoryChoices->insert(QStringLiteral("youtube"), youtubeCategory);
	for (auto it = categoryChoices->cbegin(); it != categoryChoices->cend(); ++it) {
		const QString platform = it.key();
		auto *choice = it.value();
		const QString id = local->value(editorKey(platform + QStringLiteral("Category"))).toString();
		const QString name = local->value(editorKey(platform + QStringLiteral("CategoryName"))).toString();
		selectCategory(choice, id, name);
		QObject::connect(choice, &QComboBox::activated, panel, [=](int) {
			local->setValue(editorKey(platform + QStringLiteral("Category")), choice->currentData().toString());
			local->setValue(editorKey(platform + QStringLiteral("CategoryName")), choice->currentText());
			localFlush->start();
		});
	}

	auto *remoteGroup = new QGroupBox(QStringLiteral("Фактические данные площадок"), panel);
	auto *remoteLayout = new QVBoxLayout(remoteGroup);
	auto *remoteState = new QLabel(QStringLiteral("Данные ещё не запрошены"), remoteGroup);
	remoteState->setWordWrap(true);
	remoteState->setTextInteractionFlags(Qt::TextSelectableByMouse);
	remoteLayout->addWidget(remoteState);
	layout->addWidget(remoteGroup);

	const auto accounts = gateway->accounts();
	auto *youtubeGroup = new QGroupBox(QStringLiteral("YouTube · эфир и RTMP-поток"), panel);
	auto *youtubeLayout = new QVBoxLayout(youtubeGroup);
	auto *youtubeForm = new QFormLayout();
	auto *youtubeAccount = new QComboBox(youtubeGroup);
	for (const auto &entry : accounts)
		if (entry.platform == QStringLiteral("youtube"))
			youtubeAccount->addItem(entry.displayName(), entry.id);
	if (youtubeAccount->count() > 1) youtubeForm->addRow(QStringLiteral("Канал"), youtubeAccount);
	auto *broadcast = new QComboBox(youtubeGroup);
	broadcast->setObjectName(QStringLiteral("xinkxBroadcastYouTubeEvent"));
	youtubeForm->addRow(QStringLiteral("Трансляция"), broadcast);
	auto *stream = new QComboBox(youtubeGroup);
	youtubeForm->addRow(QStringLiteral("Видеопоток"), stream);
	youtubeLayout->addLayout(youtubeForm);
	auto *experiment = new QCheckBox(QStringLiteral(
		"Однократный тест gameTitle в YouTube API (запрос и полный ответ в diagnostics.log)"), youtubeGroup);
	experiment->setChecked(true);
	experiment->setToolTip(QStringLiteral(
		"Недокументированное поле. При ошибке программа повторит сохранение без gameTitle."));
	youtubeLayout->addWidget(experiment);
	auto *youtubeLink = new QPushButton(QStringLiteral("Открыть настройку игры в YouTube Studio ↗"), youtubeGroup);
	youtubeLayout->addWidget(youtubeLink);
	QObject::connect(youtubeLink, &QPushButton::clicked, panel, [] {
		QDesktopServices::openUrl(QUrl(QStringLiteral("https://studio.youtube.com/")));
	});
	auto *create = new QPushButton(QStringLiteral("Создать эфир и поток YouTube"), youtubeGroup);
	create->setToolTip(QStringLiteral("Создаст эфир с доступом по ссылке и привяжет новый RTMP-поток"));
	auto *bind = new QPushButton(QStringLiteral("Привязать выбранный поток"), youtubeGroup);
	youtubeLayout->addWidget(create);
	youtubeLayout->addWidget(bind);
	youtubeGroup->setVisible(youtubeAccount->count() > 0);
	layout->addWidget(youtubeGroup);

	auto *read = new QPushButton(QStringLiteral("Обновить данные всех площадок"), panel);
	auto *apply = new QPushButton(QStringLiteral("Сохранить на Twitch / YouTube / Kick"), panel);
	layout->addWidget(read);
	layout->addWidget(apply);
	auto *status = new QLabel(QStringLiteral("Поля сохранены локально. Изменения на площадках — по кнопке сохранения."), panel);
	status->setWordWrap(true);
	layout->addWidget(status);
	layout->addStretch();

	auto remote = std::make_shared<QHash<QString, QJsonObject>>();
	auto youtubeIds = std::make_shared<QHash<QString, QString>>();
	auto shownYoutube = std::make_shared<QString>();
	auto initialTitle = std::make_shared<bool>(local->contains(editorKey(QStringLiteral("title"))));
	auto initialDescription = std::make_shared<bool>(local->contains(editorKey(QStringLiteral("description"))));
	auto initialGame = std::make_shared<bool>(local->contains(editorKey(QStringLiteral("game"))));

	const auto renderRemote = [=] {
		QStringList lines;
		for (const auto &entry : gateway->accounts()) {
			const QJsonObject data = remote->value(entry.id);
			const QString prefix = platformTitle(entry.platform) + QStringLiteral(" · ") + entry.displayName();
			if (data.isEmpty()) { lines << prefix + QStringLiteral(": данные не загружены"); continue; }
			const QString actualTitle = data.value(QStringLiteral("title")).toString();
			QString message = actualTitle.isEmpty()
				? QStringLiteral("название не вернулось из API")
				: actualTitle == title->text().trimmed()
				? QStringLiteral("название совпадает")
				: QStringLiteral("другое название: «%1»").arg(brief(actualTitle));
			if (entry.platform != QStringLiteral("kick")) {
				const QString actualDescription = data.value(QStringLiteral("description")).toString();
				if (!actualDescription.isEmpty() && actualDescription != description->toPlainText())
					message += QStringLiteral("; описание отличается");
			}
			const auto *choice = categoryChoices->value(entry.platform);
			const QString actualCategory = data.value(QStringLiteral("categoryId")).toVariant().toString();
			if (choice && !actualCategory.isEmpty() && actualCategory != QStringLiteral("0") &&
			    actualCategory != choice->currentData().toString())
				message += QStringLiteral("; категория на площадке: %1")
					.arg(data.value(QStringLiteral("categoryName")).toString(actualCategory));
			lines << prefix + QStringLiteral(": ") + message;
		}
		remoteState->setText(lines.join(QLatin1Char('\n')));
	};
	QObject::connect(title, &QLineEdit::textChanged, panel, [=](const QString &value) {
		*initialTitle = true;
		local->setValue(editorKey(QStringLiteral("title")), value);
		localFlush->start();
		renderRemote();
	});
	QObject::connect(description, &QPlainTextEdit::textChanged, panel, [=] {
		*initialDescription = true;
		local->setValue(editorKey(QStringLiteral("description")), description->toPlainText());
		localFlush->start();
		renderRemote();
	});
	QObject::connect(game, &QLineEdit::textChanged, panel, [=](const QString &value) {
		*initialGame = true;
		local->setValue(editorKey(QStringLiteral("game")), value);
		localFlush->start();
	});
	QObject::connect(game, &QLineEdit::returnPressed, search, &QPushButton::click);
	QObject::connect(search, &QPushButton::clicked, panel, [=] {
		const QString name = game->text().trimmed();
		if (name.size() < 2) return;
		for (const auto &entry : gateway->accounts())
			if (entry.platform != QStringLiteral("youtube"))
				gateway->searchCategories(entry.id, name);
		status->setText(QStringLiteral("Ищем «%1» в каталогах Twitch и Kick…").arg(name));
	});

	const auto chosenYoutube = [youtubeAccount] { return youtubeAccount->currentData().toString(); };
	const auto showYoutube = [=] {
		const QString id = chosenYoutube();
		if (id.isEmpty()) return;
		const QJsonObject data = remote->value(id);
		const QSignalBlocker broadcastBlock(broadcast);
		const QSignalBlocker streamBlock(stream);
		QString oldBroadcast = *shownYoutube == id ? broadcast->currentData().toString() : QString{};
		if (oldBroadcast.isEmpty()) oldBroadcast = youtubeIds->value(id,
				local->value(editorKey(QStringLiteral("youtubeEvent-")) + id).toString());
		*shownYoutube = id;
		const QString oldStream = stream->currentData().toString();
		broadcast->clear();
		for (const auto &row : data.value(QStringLiteral("broadcasts")).toArray()) {
			const auto item = row.toObject();
			broadcast->addItem(item.value(QStringLiteral("title")).toString(),
				item.value(QStringLiteral("id")).toString());
		}
		QString target = oldBroadcast;
		if (broadcast->findData(target) < 0) target = data.value(QStringLiteral("id")).toString();
		if (const int index = broadcast->findData(target); index >= 0) broadcast->setCurrentIndex(index);
		if (!broadcast->currentData().toString().isEmpty())
			(*youtubeIds)[id] = broadcast->currentData().toString();
		stream->clear();
		for (const auto &row : data.value(QStringLiteral("streams")).toArray()) {
			const auto item = row.toObject();
			stream->addItem(item.value(QStringLiteral("title")).toString(),
				item.value(QStringLiteral("id")).toString());
		}
		if (const int index = stream->findData(oldStream); index >= 0) stream->setCurrentIndex(index);
	};
	QObject::connect(youtubeAccount, &QComboBox::currentIndexChanged, panel, showYoutube);
	QObject::connect(broadcast, &QComboBox::activated, panel, [=](int index) {
		if (index < 0 || chosenYoutube().isEmpty()) return;
		const QString id = chosenYoutube();
		const QString eventId = broadcast->itemData(index).toString();
		(*youtubeIds)[id] = eventId;
		local->setValue(editorKey(QStringLiteral("youtubeEvent-")) + id, eventId);
		localFlush->start();
		gateway->inspectBroadcast(id, eventId);
	});

	QObject::connect(gateway, &ChatGateway::categoryResults, panel, [=](const QString &id, const QJsonArray &entries) {
		QString platform;
		for (const auto &entry : gateway->accounts()) if (entry.id == id) platform = entry.platform;
		auto *choice = categoryChoices->value(platform);
		if (!choice) return;
		const QString before = choice->currentData().toString();
		const QString beforeName = choice->currentText();
		int exact = -1;
		{
			const QSignalBlocker blocked(choice);
			choice->clear();
			choice->addItem(QStringLiteral("Не выбрана"), QString{});
			for (const auto &entry : entries) {
				const auto item = entry.toObject();
				const QString itemId = item.value(QStringLiteral("id")).toVariant().toString();
				const QString label = item.value(QStringLiteral("name")).toString();
				if (itemId.isEmpty() || label.isEmpty()) continue;
				choice->addItem(label, itemId);
				if (platform != QStringLiteral("youtube") &&
				    label.compare(game->text().trimmed(), Qt::CaseInsensitive) == 0) exact = choice->count() - 1;
			}
			if (!before.isEmpty()) selectCategory(choice, before, beforeName);
			else if (exact >= 0) choice->setCurrentIndex(exact);
		}
		if (before.isEmpty() && exact >= 0) {
			local->setValue(editorKey(platform + QStringLiteral("Category")), choice->currentData().toString());
			local->setValue(editorKey(platform + QStringLiteral("CategoryName")), choice->currentText());
			localFlush->start();
		}
		renderRemote();
	});

	QObject::connect(gateway, &ChatGateway::broadcastInfo, panel, [=](const QString &id, const QJsonObject &info) {
		(*remote)[id] = info;
		const QString platform = info.value(QStringLiteral("platform")).toString();
		const QString actualTitle = info.value(QStringLiteral("title")).toString();
		const QString actualDescription = info.value(QStringLiteral("description")).toString();
		const QString actualGame = info.value(QStringLiteral("categoryName")).toString();
		if (!*initialTitle && !actualTitle.isEmpty()) title->setText(actualTitle);
		if (!*initialDescription && !actualDescription.isEmpty()) description->setPlainText(actualDescription);
		if (!*initialGame && platform != QStringLiteral("youtube") && !actualGame.isEmpty())
			game->setText(actualGame);
		auto *choice = categoryChoices->value(platform);
		const QString actualCategory = info.value(QStringLiteral("categoryId")).toVariant().toString();
		if (choice && !actualCategory.isEmpty() && actualCategory != QStringLiteral("0") &&
		    choice->currentData().toString().isEmpty()) {
			const QString label = actualGame.isEmpty() ? QStringLiteral("Категория %1").arg(actualCategory) : actualGame;
			selectCategory(choice, actualCategory, label);
			local->setValue(editorKey(platform + QStringLiteral("Category")), actualCategory);
			local->setValue(editorKey(platform + QStringLiteral("CategoryName")), label);
			localFlush->start();
		}
		if (platform == QStringLiteral("youtube")) {
			const QString eventId = info.value(QStringLiteral("id")).toString();
			if (!eventId.isEmpty() && !youtubeIds->contains(id)) (*youtubeIds)[id] = eventId;
			if (id == chosenYoutube()) showYoutube();
		}
		renderRemote();
	});

	QObject::connect(gateway, &ChatGateway::broadcastResult, panel, [=](const QString &id, const QString &message, bool success) {
		writeDiagnostic(QStringLiteral("broadcast:%1:%2").arg(id, success ? QStringLiteral("ok") : QStringLiteral("error")), message);
		status->setText(message);
		status->setStyleSheet(success ? QStringLiteral("color: #70d9b2") : QStringLiteral("color: #ecb16d"));
		if (success) gateway->inspectBroadcast(id, youtubeIds->value(id));
	});
	QObject::connect(read, &QPushButton::clicked, panel, [=] {
		gateway->reloadAccounts();
		for (const auto &entry : gateway->accounts()) {
			const QString eventId = entry.id == chosenYoutube() && !broadcast->currentData().toString().isEmpty()
				? broadcast->currentData().toString() : youtubeIds->value(entry.id);
			gateway->inspectBroadcast(entry.id, eventId);
			if (entry.platform == QStringLiteral("youtube")) gateway->searchCategories(entry.id, QString{});
		}
		status->setStyleSheet({});
		status->setText(QStringLiteral("Получаем фактические данные, локальные поля не изменяются…"));
	});
	QObject::connect(apply, &QPushButton::clicked, panel, [=] {
		local->sync();
		int sent = 0;
		bool testedGameTitle = false;
		for (const auto &entry : gateway->accounts()) {
			const QString eventId = entry.id == chosenYoutube() && !broadcast->currentData().toString().isEmpty()
				? broadcast->currentData().toString()
				: youtubeIds->value(entry.id, entry.platform == QStringLiteral("youtube")
				? local->value(editorKey(QStringLiteral("youtubeEvent-")) + entry.id).toString() : QString{});
			if (entry.platform == QStringLiteral("youtube") && eventId.isEmpty()) {
				status->setText(QStringLiteral("YouTube: нет выбранной трансляции — её данные не изменены"));
				continue;
			}
			auto *choice = categoryChoices->value(entry.platform);
			const bool testGame = entry.platform == QStringLiteral("youtube") && experiment->isChecked() &&
				!game->text().trimmed().isEmpty();
			if (entry.platform == QStringLiteral("youtube")) (*youtubeIds)[entry.id] = eventId;
			gateway->updateBroadcast(entry.id, eventId, title->text(), description->toPlainText(),
				choice ? choice->currentData().toString() : QString{},
				testGame ? game->text() : QString{});
			testedGameTitle = testedGameTitle || testGame;
			++sent;
		}
		if (testedGameTitle) experiment->setChecked(false); // One diagnostic experiment per session.
		if (sent) status->setText(QStringLiteral("Сохраняем данные на %1 площадках…").arg(sent));
	});
	QObject::connect(create, &QPushButton::clicked, panel, [=] {
		gateway->createYouTubeBroadcast(chosenYoutube(), title->text(), description->toPlainText(),
			youtubeCategory->currentData().toString());
		status->setText(QStringLiteral("Создаём эфир и RTMP-поток YouTube…"));
	});
	QObject::connect(bind, &QPushButton::clicked, panel, [=] {
		gateway->bindYouTubeStream(chosenYoutube(), broadcast->currentData().toString(),
			stream->currentData().toString());
		status->setText(QStringLiteral("Привязываем видеопоток YouTube…"));
	});
	renderRemote();
	// Retrieve YouTube's full regional category catalog at startup, without
	// relying on the text of a game search or a selected account.
	for (const auto &entry : accounts) {
		gateway->inspectBroadcast(entry.id);
		if (entry.platform == QStringLiteral("youtube")) gateway->searchCategories(entry.id, QString{});
	}

	auto *scroll = new QScrollArea(dock);
	scroll->setObjectName(QStringLiteral("xinkxBroadcastEditor"));
	scroll->setWidgetResizable(true);
	scroll->setWidget(panel);
	studioTabs(mainWindow)->addTab(scroll, QStringLiteral("Эфир"));
}

} // namespace xinkx
