#include "ChatGateway.hpp"

#include <widgets/OBSBasic.hpp>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

namespace xinkx {

namespace {

class ChatViewState final : public QObject {
public:
	explicit ChatViewState(QObject *parent) : QObject(parent) {}
	QHash<QString, QCheckBox *> targets;
	QHash<QString, QLabel *> statuses;
	QHash<QString, QLabel *> viewers;
	QHash<QString, QString> statusCache;
};

} // namespace

void installChatWorkspace(OBSBasic *mainWindow)
{
	auto *gateway = new ChatGateway(mainWindow);
	gateway->setObjectName(QStringLiteral("xinkxChatGateway"));
	auto *dock = new QDockWidget(QStringLiteral("Мультичат"), mainWindow);
	dock->setObjectName(QStringLiteral("xinkxChatDock"));
	auto *panel = new QWidget(dock);
	auto *root = new QVBoxLayout(panel);
	root->setContentsMargins(9, 8, 9, 8);
	root->setSpacing(6);
	auto *toolbar = new QHBoxLayout();
	auto *heading = new QLabel(QStringLiteral("Чат Twitch / YouTube / Kick"), panel);
	heading->setStyleSheet(QStringLiteral("font-weight: 600"));
	toolbar->addWidget(heading, 1);
	auto *refresh = new QPushButton(QStringLiteral("Обновить"), panel);
	refresh->setToolTip(QStringLiteral("Перечитать подключённые аккаунты из настроек трансляции"));
	toolbar->addWidget(refresh);
	root->addLayout(toolbar);
	auto *selector = new QWidget(panel);
	auto *accountRow = new QHBoxLayout(selector);
	accountRow->setContentsMargins(0, 2, 0, 2);
	root->addWidget(selector);
	auto *view = new ChatViewState(dock);
	auto rebuild = [=] {
		QSet<QString> enabled;
		const bool hadChoices = !view->targets.isEmpty();
		for (auto it = view->targets.cbegin(); it != view->targets.cend(); ++it) {
			if (it.value()->isChecked()) enabled.insert(it.key());
		}
		while (auto *item = accountRow->takeAt(0)) {
			if (item->widget()) item->widget()->deleteLater();
			delete item;
		}
		view->targets.clear();
		view->statuses.clear();
		view->viewers.clear();
		const auto accounts = gateway->accounts();
		for (const auto &account : accounts) {
			auto *column = new QWidget(selector);
			auto *stack = new QVBoxLayout(column);
			stack->setContentsMargins(0, 0, 6, 0);
			stack->setSpacing(2);
			auto *box = new QCheckBox(platformTitle(account.platform) +
				QStringLiteral(" · ") + account.displayName(), column);
			box->setChecked(!hadChoices || enabled.contains(account.id));
			box->setToolTip(QStringLiteral("Отправлять сообщение в этот чат"));
			stack->addWidget(box);
			auto *status = new QLabel(view->statusCache.value(account.id, QStringLiteral("Ожидание соединения")), column);
			status->setWordWrap(true);
			status->setStyleSheet(QStringLiteral("color:#a1b5c9; font-size: 11px"));
			stack->addWidget(status);
			auto *viewer = new QLabel(QStringLiteral("Зрители: —"), column);
			viewer->setStyleSheet(QStringLiteral("font-size:11px;color:#82d8d3"));
			stack->addWidget(viewer);
			accountRow->addWidget(column, 1);
			view->targets.insert(account.id, box);
			view->statuses.insert(account.id, status);
			view->viewers.insert(account.id, viewer);
		}
		if (accounts.isEmpty()) {
			auto *hint = new QLabel(QStringLiteral("Подключи аккаунты в Настройки → Трансляция"), selector);
			hint->setWordWrap(true);
			accountRow->addWidget(hint);
		}
	};
	rebuild();
	QObject::connect(refresh, &QPushButton::clicked, dock, [=] {
		gateway->reloadAccounts();
		rebuild();
	});
	auto *kickRoomRow = new QHBoxLayout();
	auto *kickSelect = new QComboBox(panel);
	auto *kickRoom = new QLineEdit(panel);
	kickRoom->setPlaceholderText(QStringLiteral("Kick chatroom ID (пусто = автоматически)"));
	kickRoom->setMaxLength(15);
	auto *kickSave = new QPushButton(QStringLiteral("Комната Kick"), panel);
	kickRoomRow->addWidget(kickSelect);
	kickRoomRow->addWidget(kickRoom, 1);
	kickRoomRow->addWidget(kickSave);
	root->addLayout(kickRoomRow);
	auto rebuildKick = [=] {
		const auto selected = kickSelect->currentData().toString();
		kickSelect->clear();
		for (const auto &account : gateway->accounts()) {
			if (account.platform == QStringLiteral("kick")) kickSelect->addItem(account.displayName(), account.id);
		}
		const int index = kickSelect->findData(selected);
		if (index >= 0) kickSelect->setCurrentIndex(index);
		kickRoom->setVisible(kickSelect->count() > 0);
		kickSelect->setVisible(kickSelect->count() > 0);
		kickSave->setVisible(kickSelect->count() > 0);
		kickRoom->setText(gateway->kickRoomId(kickSelect->currentData().toString()));
	};
	QObject::connect(kickSelect, &QComboBox::currentIndexChanged, dock, [=] {
		kickRoom->setText(gateway->kickRoomId(kickSelect->currentData().toString()));
	});
	QObject::connect(kickSave, &QPushButton::clicked, dock, [=] {
		gateway->setKickRoomId(kickSelect->currentData().toString(), kickRoom->text());
	});
	QObject::connect(refresh, &QPushButton::clicked, dock, rebuildKick);
	rebuildKick();
	auto *feed = new QListWidget(panel);
	feed->setWordWrap(true);
	feed->setAlternatingRowColors(true);
	feed->setSelectionMode(QAbstractItemView::ExtendedSelection);
	feed->setContextMenuPolicy(Qt::DefaultContextMenu);
	feed->setToolTip(QStringLiteral("Общая хронология сообщений. Выделение текста для копирования доступно через Ctrl+C"));
	auto *copy = new QShortcut(QKeySequence::Copy, feed);
	QObject::connect(copy, &QShortcut::activated, dock, [feed] {
		QStringList rows;
		for (auto *item : feed->selectedItems()) rows.append(item->text());
		if (!rows.isEmpty()) QApplication::clipboard()->setText(rows.join(QLatin1Char('\n')));
	});
	root->addWidget(feed, 1);
	auto *composer = new QHBoxLayout();
	auto *entry = new QLineEdit(panel);
	entry->setPlaceholderText(QStringLiteral("Сообщение во все отмеченные чаты…"));
	entry->setMaxLength(200);
	composer->addWidget(entry, 1);
	auto *send = new QPushButton(QStringLiteral("Отправить"), panel);
	composer->addWidget(send);
	root->addLayout(composer);
	auto sendCurrent = [=] {
		QStringList selected;
		for (auto it = view->targets.cbegin(); it != view->targets.cend(); ++it) {
			if (it.value()->isChecked()) selected.append(it.key());
		}
		if (selected.isEmpty()) {
			QListWidgetItem *item = new QListWidgetItem(QStringLiteral("Выбери хотя бы одну площадку"), feed);
			item->setForeground(QColor(QStringLiteral("#efbd6d")));
			return;
		}
		gateway->sendMessage(entry->text(), selected);
	};
	QObject::connect(send, &QPushButton::clicked, dock, sendCurrent);
	QObject::connect(entry, &QLineEdit::returnPressed, dock, sendCurrent);
	QObject::connect(gateway, &ChatGateway::providerStatus, dock,
		[=](const QString &id, const QString &message) {
			view->statusCache[id] = message;
			if (view->statuses.contains(id)) view->statuses.value(id)->setText(message);
		});
	QObject::connect(gateway, &ChatGateway::providerWarning, dock,
		[=](const QString &id, const QString &message) {
			Q_UNUSED(id);
			auto *item = new QListWidgetItem(QStringLiteral("⚠ ") + message, feed);
			item->setForeground(QColor(QStringLiteral("#d8ac65")));
			while (feed->count() > 300) delete feed->takeItem(0);
		});
	QObject::connect(gateway, &ChatGateway::viewersReceived, dock, [view](const QString &id, int count) {
		if (auto *label = view->viewers.value(id)) label->setText(count < 0
			? QStringLiteral("Зрители: —") : QStringLiteral("Зрители: %1").arg(count));
	});
	QObject::connect(gateway, &ChatGateway::incoming, dock, [=](const ChatLine &message) {
		const auto timestamp = QDateTime::fromMSecsSinceEpoch(message.receivedAt).toString(QStringLiteral("HH:mm:ss"));
		QString formatted = QStringLiteral("%1  [%2] %3").arg(timestamp,
			platformTitle(message.platform), message.name);
		if (message.type == QStringLiteral("chat")) formatted += QStringLiteral(": ") + message.text;
		else formatted += QStringLiteral(" · %1 %2 %3").arg(message.type, message.amount, message.text);
		auto *item = new QListWidgetItem(formatted, feed);
		item->setForeground(QColor(message.platform == QStringLiteral("twitch") ? QStringLiteral("#ba91ff")
			: message.platform == QStringLiteral("youtube") ? QStringLiteral("#ff9292")
									   : QStringLiteral("#8be2b4")));
		while (feed->count() > 300) delete feed->takeItem(0);
		feed->scrollToBottom();
	});
	QObject::connect(gateway, &ChatGateway::sendStatus, dock,
		[=](const QString &id, const QString &result, bool success) {
			QString platform;
			for (const auto &account : gateway->accounts()) {
				if (account.id == id) { platform = platformTitle(account.platform); break; }
			}
			auto *item = new QListWidgetItem((success ? QStringLiteral("✓ ") : QStringLiteral("! ")) +
				platform + QStringLiteral(": ") + result, feed);
			item->setForeground(QColor(success ? QStringLiteral("#74d9b5") : QStringLiteral("#e9ae67")));
			if (success) entry->clear();
			while (feed->count() > 300) delete feed->takeItem(0);
			feed->scrollToBottom();
		});
	gateway->reloadAccounts();
	dock->setWidget(panel);
	mainWindow->AddDockWidget(dock, Qt::RightDockWidgetArea);
	bool visible = true;
	const auto path = studioSettingsPath();
	if (!path.isEmpty()) {
		auto *settings = new QSettings(path, QSettings::IniFormat, dock);
		visible = settings->value(QStringLiteral("workspace/chatVisible"), true).toBool();
		QObject::connect(dock, &QDockWidget::visibilityChanged, dock, [settings](bool shown) {
			settings->setValue(QStringLiteral("workspace/chatVisible"), shown);
		});
	}
	QTimer::singleShot(0, dock, [=] {
		if (auto *effects = mainWindow->findChild<QDockWidget *>(QStringLiteral("xinkxEffectsDock"))) {
			mainWindow->tabifyDockWidget(effects, dock);
		}
		dock->setVisible(visible);
		dock->raise();
	});
}

} // namespace xinkx
