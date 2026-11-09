#include "ChatGateway.hpp"
#include "StudioWorkspace.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QSizePolicy>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {

void installBroadcastWorkspace(OBSBasic *mainWindow)
{
	auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"));
	if (!gateway) return;
	auto *dock = studioDock(mainWindow);
	if (!dock) return;
	auto *panel = new QWidget(dock);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(7);
	auto *form = new QFormLayout();
	auto *account = new QComboBox(panel);
	form->addRow(QStringLiteral("Площадка"), account);
	auto *title = new QLineEdit(panel);
	title->setMaxLength(100);
	form->addRow(QStringLiteral("Название"), title);
	auto *description = new QPlainTextEdit(panel);
	description->setMaximumHeight(95);
	description->setPlaceholderText(QStringLiteral("Описание Twitch / YouTube; Kick: только просмотр"));
	form->addRow(QStringLiteral("Описание"), description);
	auto *category = new QLineEdit(panel);
	category->hide();
	auto *categoryName = new QLabel(QStringLiteral("Не выбрана"), panel);
	categoryName->setWordWrap(true);
	categoryName->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	form->addRow(QStringLiteral("Игра / категория"), categoryName);
	auto *searchRow = new QHBoxLayout();
	auto *query = new QLineEdit(panel);
	query->setPlaceholderText(QStringLiteral("Найти игру или категорию"));
	auto *search = new QPushButton(QStringLiteral("Найти"), panel);
	searchRow->addWidget(query, 1);
	searchRow->addWidget(search);
	layout->addLayout(form);
	layout->addLayout(searchRow);
	auto *categoryFields = new QFormLayout();
	auto *twitchCategory = new QComboBox(panel);
	auto *kickCategory = new QComboBox(panel);
	auto *youtubeCategory = new QComboBox(panel);
	for (auto *choice : {twitchCategory, kickCategory, youtubeCategory})
		choice->addItem(QStringLiteral("Не изменять"), QString{});
	categoryFields->addRow(QStringLiteral("Twitch"), twitchCategory);
	categoryFields->addRow(QStringLiteral("Kick"), kickCategory);
	categoryFields->addRow(QStringLiteral("YouTube · категория"), youtubeCategory);
	layout->addLayout(categoryFields);
	auto *youtubeLink = new QPushButton(QStringLiteral("Настроить игру YouTube в Studio ↗"), panel);
	layout->addWidget(youtubeLink);
	QObject::connect(youtubeLink, &QPushButton::clicked, panel, [] {
		QDesktopServices::openUrl(QUrl(QStringLiteral("https://studio.youtube.com/")));
	});
	auto *ytRow = new QWidget(panel);
	auto *youtubeForm = new QFormLayout(ytRow);
	youtubeForm->setContentsMargins(0, 0, 0, 0);
	auto *broadcast = new QComboBox(ytRow);
	youtubeForm->addRow(QStringLiteral("Трансляция YouTube"), broadcast);
	auto *stream = new QComboBox(ytRow);
	youtubeForm->addRow(QStringLiteral("Видеопоток"), stream);
	layout->addWidget(ytRow);
	auto *ytActions = new QHBoxLayout();
	auto *create = new QPushButton(QStringLiteral("Создать эфир и поток YouTube"), panel);
	create->setToolTip(QStringLiteral("Создаст эфир с доступом по ссылке, новый RTMP-поток и привяжет его"));
	auto *bind = new QPushButton(QStringLiteral("Привязать выбранный поток"), panel);
	ytActions->addWidget(create);
	ytActions->addWidget(bind);
	layout->addLayout(ytActions);
	auto *all = new QCheckBox(QStringLiteral("Обновить название и найденные категории на всех площадках"), panel);
	all->setChecked(true);
	layout->addWidget(all);
	auto *actions = new QHBoxLayout();
	auto *read = new QPushButton(QStringLiteral("Загрузить текущие данные"), panel);
	auto *apply = new QPushButton(QStringLiteral("Сохранить на площадке"), panel);
	actions->addWidget(read);
	actions->addWidget(apply);
	layout->addLayout(actions);
	auto *status = new QLabel(QStringLiteral("Данные изменяются только по нажатию «Сохранить»"), panel);
	status->setWordWrap(true);
	layout->addWidget(status);
	layout->addStretch();
	auto youtubeIds = std::make_shared<QHash<QString, QString>>();
	auto descriptionEdited = std::make_shared<bool>(false);
	QObject::connect(description, &QPlainTextEdit::textChanged, panel, [descriptionEdited] {
		*descriptionEdited = true;
	});
	auto choices = std::make_shared<QHash<QString, QComboBox *>>();
	for (const auto &entry : gateway->accounts()) {
		if (entry.platform == QStringLiteral("twitch")) (*choices)[entry.id] = twitchCategory;
		if (entry.platform == QStringLiteral("kick")) (*choices)[entry.id] = kickCategory;
		if (entry.platform == QStringLiteral("youtube")) (*choices)[entry.id] = youtubeCategory;
	}
	auto selected = [account] { return account->currentData().toString(); };
	auto reload = [=] {
		const auto previous = selected();
		account->clear();
		for (const auto &entry : gateway->accounts())
			account->addItem(platformTitle(entry.platform) + QStringLiteral(" · ") + entry.displayName(), entry.id);
		const int index = account->findData(previous);
		if (index >= 0) account->setCurrentIndex(index);
	};
	reload();
	auto changePlatform = [=] {
		const auto selectedId = selected();
		QString platform;
		for (const auto &entry : gateway->accounts()) if (entry.id == selectedId) platform = entry.platform;
		const bool youtube = platform == QStringLiteral("youtube");
		ytRow->setVisible(youtube);
		create->setVisible(youtube);
		bind->setVisible(youtube);
		const bool twitch = platform == QStringLiteral("twitch");
		description->setReadOnly(!youtube && !twitch);
		description->setPlaceholderText(twitch ? QStringLiteral("Описание канала Twitch · OAuth user:edit") :
			youtube ? QStringLiteral("Описание выбранного эфира YouTube") :
			QStringLiteral("Kick: изменение описания не опубликовано в API"));
		broadcast->clear();
		stream->clear();
		category->clear();
		categoryName->setText(QStringLiteral("Не выбрана"));
		title->clear();
		{
			const QSignalBlocker blocked(description);
			description->clear();
		}
		*descriptionEdited = false;
		if (!selectedId.isEmpty()) gateway->inspectBroadcast(selectedId);
	};
	QObject::connect(account, &QComboBox::currentIndexChanged, dock, changePlatform);
	changePlatform();
	QObject::connect(read, &QPushButton::clicked, dock, [=] {
		gateway->reloadAccounts();
		gateway->inspectBroadcast(selected());
		status->setText(QStringLiteral("Запрашиваем данные платформы…"));
	});
	QObject::connect(search, &QPushButton::clicked, dock, [=] {
		if (query->text().trimmed().size() < 2) return;
		for (const auto &entry : gateway->accounts()) gateway->searchCategories(entry.id, query->text());
		status->setText(QStringLiteral("Ищем игру «%1» на всех площадках…").arg(query->text()));
	});
	QObject::connect(query, &QLineEdit::returnPressed, search, &QPushButton::click);
	for (auto *choice : {twitchCategory, kickCategory, youtubeCategory})
		QObject::connect(choice, &QComboBox::currentIndexChanged, dock, [=] {
			if (choices->value(selected()) == choice) {
				category->setText(choice->currentData().toString());
				categoryName->setText(choice->currentText());
			}
		});
	QObject::connect(broadcast, &QComboBox::activated, dock, [=](int index) {
		if (index >= 0) {
			(*youtubeIds)[selected()] = broadcast->itemData(index).toString();
			gateway->inspectBroadcast(selected(), broadcast->itemData(index).toString());
		}
	});
	QObject::connect(gateway, &ChatGateway::categoryResults, dock, [=](const QString &id, const QJsonArray &entries) {
		auto *results = choices->value(id);
		if (!results) return;
		const QSignalBlocker blocker(results);
		results->clear();
		results->addItem(QStringLiteral("Не изменять"), QString{});
		int best = 0;
		for (const auto &entry : entries) {
			const auto item = entry.toObject();
			const QString label = item.value(QStringLiteral("name")).toString();
			results->addItem(label, item.value(QStringLiteral("id")).toVariant().toString());
			if (label.compare(query->text().trimmed(), Qt::CaseInsensitive) == 0) best = results->count() - 1;
		}
		if (!best && entries.size() == 1) best = 1;
		if (!best && results == youtubeCategory) {
			for (int i = 1; i < results->count(); ++i)
				if (results->itemText(i).contains(QStringLiteral("Gaming"), Qt::CaseInsensitive) ||
				    results->itemText(i).contains(QStringLiteral("Видеоигр"), Qt::CaseInsensitive)) { best = i; break; }
		}
		results->setCurrentIndex(best);
		if (id == selected()) {
			category->setText(results->currentData().toString());
			categoryName->setText(results->currentText());
		}
		status->setText(QStringLiteral("%1: найдено %2 категорий. Проверь совпадение перед сохранением.")
			.arg(results == twitchCategory ? QStringLiteral("Twitch") : results == kickCategory ? QStringLiteral("Kick") : QStringLiteral("YouTube"))
			.arg(entries.size()));
	});
	QObject::connect(gateway, &ChatGateway::broadcastInfo, dock, [=](const QString &id, const QJsonObject &info) {
		if (id != selected()) return;
		title->setText(info.value(QStringLiteral("title")).toString());
		{
			const QSignalBlocker blocked(description);
			description->setPlainText(info.value(QStringLiteral("description")).toString());
		}
		*descriptionEdited = false;
		category->setText(info.value(QStringLiteral("categoryId")).toVariant().toString());
		categoryName->setText(info.value(QStringLiteral("categoryName")).toString(QStringLiteral("Не выбрана")));
		if (auto *choice = choices->value(id); choice && choice->count() <= 1 && !category->text().isEmpty()) {
			choice->addItem(categoryName->text(), category->text());
			choice->setCurrentIndex(choice->count() - 1);
		}
		const auto broadcasts = info.value(QStringLiteral("broadcasts")).toArray();
		if (!info.value(QStringLiteral("id")).toString().isEmpty())
			(*youtubeIds)[id] = info.value(QStringLiteral("id")).toString();
		if (!broadcasts.isEmpty()) {
			const QSignalBlocker blocker(broadcast);
			const QString before = broadcast->currentData().toString();
			broadcast->clear();
			for (const auto &row : broadcasts) {
				const auto item = row.toObject();
				broadcast->addItem(item.value(QStringLiteral("title")).toString(), item.value(QStringLiteral("id")).toString());
			}
			const QString chosen = info.value(QStringLiteral("id")).toString(before);
			if (const int index = broadcast->findData(chosen); index >= 0) broadcast->setCurrentIndex(index);
		}
		const auto streams = info.value(QStringLiteral("streams")).toArray();
		if (!streams.isEmpty()) {
			const QSignalBlocker blocker(stream);
			const QString before = stream->currentData().toString();
			stream->clear();
			for (const auto &row : streams) {
				const auto item = row.toObject();
				stream->addItem(item.value(QStringLiteral("title")).toString(), item.value(QStringLiteral("id")).toString());
			}
			if (const int index = stream->findData(before); index >= 0) stream->setCurrentIndex(index);
		}
		status->setText(QStringLiteral("Получены данные %1 · категория %2")
			.arg(info.value(QStringLiteral("platform")).toString(), info.value(QStringLiteral("categoryName")).toString()));
	});
	QObject::connect(gateway, &ChatGateway::broadcastResult, dock, [=](const QString &id, const QString &message, bool success) {
		if (id != selected() && !all->isChecked()) return;
		status->setText(message);
		status->setStyleSheet(success ? QStringLiteral("color: #70d9b2") : QStringLiteral("color: #ecb16d"));
	});
	QObject::connect(apply, &QPushButton::clicked, dock, [=] {
		QStringList targets{selected()};
		if (all->isChecked()) {
			targets.clear();
			for (const auto &entry : gateway->accounts()) targets.append(entry.id);
		}
		for (const auto &id : targets) {
			QString platform;
			for (const auto &entry : gateway->accounts()) if (entry.id == id) platform = entry.platform;
			const QString broadcastId = id == selected() ? broadcast->currentData().toString() : youtubeIds->value(id);
			if (platform == QStringLiteral("youtube") && broadcastId.isEmpty()) continue;
			const auto *choice = choices->value(id);
			const QString chosenCategory = choice ? choice->currentData().toString() : QString{};
			gateway->updateBroadcast(id, broadcastId, title->text(),
				platform == QStringLiteral("kick") || !*descriptionEdited ? QString{} : description->toPlainText(),
				chosenCategory);
		}
		status->setText(QStringLiteral("Отправляем изменения. Для YouTube необходимо предварительно выбрать эфир на его аккаунте."));
	});
	QObject::connect(create, &QPushButton::clicked, dock, [=] {
		gateway->createYouTubeBroadcast(selected(), title->text(), description->toPlainText(), category->text());
		status->setText(QStringLiteral("Создаём трансляцию YouTube и RTMP-поток…"));
	});
	QObject::connect(bind, &QPushButton::clicked, dock, [=] {
		gateway->bindYouTubeStream(selected(), broadcast->currentData().toString(), stream->currentData().toString());
		status->setText(QStringLiteral("Привязываем видеопоток YouTube…"));
	});
	auto *scroll = new QScrollArea(dock);
	scroll->setWidgetResizable(true);
	scroll->setWidget(panel);
	auto *tabs = new QTabWidget(dock);
	tabs->setObjectName(QStringLiteral("xinkxBroadcastTabs"));
	tabs->addTab(scroll, QStringLiteral("Текущий эфир"));
	studioTabs(mainWindow)->addTab(tabs, QStringLiteral("Эфир"));
}

} // namespace xinkx
