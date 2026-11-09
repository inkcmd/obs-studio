#include "ChatGateway.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {

void installBroadcastWorkspace(OBSBasic *mainWindow)
{
	auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"));
	if (!gateway) return;
	auto *dock = new QDockWidget(QStringLiteral("Управление эфиром"), mainWindow);
	dock->setObjectName(QStringLiteral("xinkxBroadcastDock"));
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
	description->setPlaceholderText(QStringLiteral("YouTube: описание выбранного эфира; Twitch и Kick не поддерживают описание через этот API"));
	form->addRow(QStringLiteral("Описание"), description);
	auto *category = new QLineEdit(panel);
	category->setPlaceholderText(QStringLiteral("ID игры / категории"));
	form->addRow(QStringLiteral("ID категории"), category);
	auto *searchRow = new QHBoxLayout();
	auto *query = new QLineEdit(panel);
	query->setPlaceholderText(QStringLiteral("Найти игру или категорию"));
	auto *search = new QPushButton(QStringLiteral("Найти"), panel);
	searchRow->addWidget(query, 1);
	searchRow->addWidget(search);
	layout->addLayout(form);
	layout->addLayout(searchRow);
	auto *candidates = new QComboBox(panel);
	candidates->setPlaceholderText(QStringLiteral("Результаты поиска"));
	layout->addWidget(candidates);
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
	auto *all = new QCheckBox(QStringLiteral("Название — всем, категория — выбранной площадке (ID платформ разные)"), panel);
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
		description->setEnabled(youtube);
		broadcast->clear();
		stream->clear();
		candidates->clear();
		category->clear();
		title->clear();
		description->clear();
	};
	QObject::connect(account, &QComboBox::currentIndexChanged, dock, changePlatform);
	changePlatform();
	QObject::connect(read, &QPushButton::clicked, dock, [=] {
		gateway->reloadAccounts();
		gateway->inspectBroadcast(selected());
		status->setText(QStringLiteral("Запрашиваем данные платформы…"));
	});
	QObject::connect(search, &QPushButton::clicked, dock, [=] {
		if (!query->text().isEmpty()) gateway->searchCategories(selected(), query->text());
	});
	QObject::connect(query, &QLineEdit::returnPressed, search, &QPushButton::click);
	QObject::connect(candidates, &QComboBox::activated, dock, [=](int index) {
		if (index >= 0) category->setText(candidates->itemData(index).toString());
	});
	QObject::connect(broadcast, &QComboBox::activated, dock, [=](int index) {
		if (index >= 0) {
			(*youtubeIds)[selected()] = broadcast->itemData(index).toString();
			gateway->inspectBroadcast(selected(), broadcast->itemData(index).toString());
		}
	});
	QObject::connect(gateway, &ChatGateway::categoryResults, dock, [=](const QString &id, const QJsonArray &entries) {
		if (id != selected()) return;
		candidates->clear();
		for (const auto &entry : entries) {
			const auto item = entry.toObject();
			candidates->addItem(item.value(QStringLiteral("name")).toString(), item.value(QStringLiteral("id")).toString());
		}
		status->setText(QStringLiteral("Найдено категорий: %1 · выбери нужную").arg(entries.size()));
	});
	QObject::connect(gateway, &ChatGateway::broadcastInfo, dock, [=](const QString &id, const QJsonObject &info) {
		if (id != selected()) return;
		title->setText(info.value(QStringLiteral("title")).toString());
		description->setPlainText(info.value(QStringLiteral("description")).toString());
		category->setText(info.value(QStringLiteral("categoryId")).toVariant().toString());
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
			gateway->updateBroadcast(id, broadcastId, title->text(),
				all->isChecked() && id != selected() ? QString{} : description->toPlainText(),
				all->isChecked() && id != selected() ? QString{} : category->text());
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
	dock->setWidget(scroll);
	mainWindow->AddDockWidget(dock, Qt::RightDockWidgetArea);
	if (auto *chat = mainWindow->findChild<QDockWidget *>(QStringLiteral("xinkxChatDock")))
		mainWindow->tabifyDockWidget(chat, dock);
	dock->show();
}

} // namespace xinkx
