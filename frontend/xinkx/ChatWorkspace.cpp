#include "ChatGateway.hpp"
#include "ModerationEngine.hpp"
#include "StudioDiagnostics.hpp"
#include "StudioWorkspace.hpp"

#include <widgets/OBSBasic.hpp>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QListWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QTabWidget>
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
	QHash<QString, QStringList> warnings;
};

} // namespace

void installChatWorkspace(OBSBasic *mainWindow)
{
	auto *gateway = new ChatGateway(mainWindow);
	gateway->setObjectName(QStringLiteral("xinkxChatGateway"));
	auto *dock = studioDock(mainWindow);
	if (!dock) return;
	auto *panel = new QWidget(dock);
	panel->setObjectName(QStringLiteral("xinkxChatPage"));
	auto *root = new QVBoxLayout(panel);
	root->setContentsMargins(9, 8, 9, 8);
	root->setSpacing(6);
	auto *toolbar = new QHBoxLayout();
	auto *heading = new QLabel(QStringLiteral("Чат Twitch / YouTube / Kick"), panel);
	heading->setStyleSheet(QStringLiteral("font-weight: 600"));
	toolbar->addWidget(heading, 1);
	auto *refresh = new QPushButton(QStringLiteral("Обновить"), panel);
	refresh->setToolTip(QStringLiteral("Перечитать аккаунты и повторно подключить недоступные чаты"));
	toolbar->addWidget(refresh);
	// A small editor in the existing Chat page; no extra dock or duplicated feed.
	if (auto *moderation = mainWindow->findChild<ModerationEngine *>(QStringLiteral("xinkxModerationEngine"))) {
		auto *editModeration = new QPushButton(panel);
		editModeration->setToolTip(QStringLiteral("Фильтр действует локально: не удаляет сообщения на площадках"));
		toolbar->addWidget(editModeration);
		const auto updateLabel = [moderation, editModeration] {
			const auto settings = moderation->configuration();
			const auto counters = moderation->counters();
			editModeration->setText(settings.enabled
				? QStringLiteral("Модерация: %1 скрыто").arg(counters.suppressed())
				: QStringLiteral("Модерация: выкл."));
		};
		updateLabel();
		QObject::connect(moderation, &ModerationEngine::configurationChanged, editModeration, updateLabel);
		QObject::connect(moderation, &ModerationEngine::countersChanged, editModeration, updateLabel);
		QObject::connect(editModeration, &QPushButton::clicked, panel, [panel, moderation] {
			QDialog dialog(panel);
			dialog.setWindowTitle(QStringLiteral("Локальная модерация чата"));
			dialog.resize(540, 440);
			auto *layout = new QVBoxLayout(&dialog);
			auto *notice = new QLabel(QStringLiteral(
				"Только входящий чат в XINKX Studio: окно чата, OBS-оверлей, "
				"бот и автоматические правила. На Twitch, YouTube и Kick "
				"сообщения не удаляются. Донаты и подписки не фильтруются."), &dialog);
			notice->setWordWrap(true);
			layout->addWidget(notice);
			const auto current = moderation->configuration();
			auto *enabled = new QCheckBox(QStringLiteral("Включить локальный фильтр"), &dialog);
			enabled->setChecked(current.enabled);
			layout->addWidget(enabled);
			auto *links = new QCheckBox(QStringLiteral("Скрывать сообщения со ссылками"), &dialog);
			links->setChecked(current.blockLinks);
			layout->addWidget(links);
			auto *repeats = new QCheckBox(QStringLiteral("Скрывать повтор одного сообщения за 15 секунд"), &dialog);
			repeats->setChecked(current.blockRepeats);
			layout->addWidget(repeats);
			auto *flood = new QCheckBox(QStringLiteral("Ограничивать частоту сообщений одного зрителя"), &dialog);
			flood->setChecked(current.blockFlood);
			layout->addWidget(flood);
			auto *form = new QFormLayout();
			auto *maxMessages = new QSpinBox(&dialog);
			maxMessages->setRange(2, 30);
			maxMessages->setValue(current.maxMessages);
			form->addRow(QStringLiteral("Сообщений за интервал"), maxMessages);
			auto *window = new QSpinBox(&dialog);
			window->setRange(3, 120);
			window->setSuffix(QStringLiteral(" сек"));
			window->setValue(current.windowSeconds);
			form->addRow(QStringLiteral("Интервал"), window);
			layout->addLayout(form);
			QObject::connect(flood, &QCheckBox::toggled, maxMessages, &QSpinBox::setEnabled);
			QObject::connect(flood, &QCheckBox::toggled, window, &QSpinBox::setEnabled);
			maxMessages->setEnabled(current.blockFlood);
			window->setEnabled(current.blockFlood);
			layout->addWidget(new QLabel(QStringLiteral("Стоп-фразы: одна на строку, не более 100"), &dialog));
			auto *phrases = new QPlainTextEdit(current.blockedPhrases.join(QLatin1Char('\n')), &dialog);
			phrases->setPlaceholderText(QStringLiteral("Слова или фразы для локального скрытия сообщений"));
			layout->addWidget(phrases, 1);
			auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
			layout->addWidget(buttons);
			QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
			QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
				ModerationEngine::Configuration changed;
				changed.enabled = enabled->isChecked();
				changed.blockLinks = links->isChecked();
				changed.blockRepeats = repeats->isChecked();
				changed.blockFlood = flood->isChecked();
				changed.maxMessages = maxMessages->value();
				changed.windowSeconds = window->value();
				changed.blockedPhrases = phrases->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
				QString error;
				if (moderation->updateConfiguration(changed, &error)) dialog.accept();
				else QMessageBox::warning(&dialog, QStringLiteral("Модерация"), error);
			});
			dialog.exec();
		});
	}
	root->addLayout(toolbar);
	auto *selector = new QWidget(panel);
	auto *accountRow = new QHBoxLayout(selector);
	accountRow->setContentsMargins(0, 2, 0, 2);
	root->addWidget(selector);
	auto *connectionNotice = new QLabel(panel);
	connectionNotice->setWordWrap(true);
	connectionNotice->setTextInteractionFlags(Qt::TextSelectableByMouse);
	connectionNotice->setStyleSheet(QStringLiteral("color:#efbd6d;font-size:11px"));
	connectionNotice->hide();
	root->addWidget(connectionNotice);
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
		const auto accounts = gateway->chatAccounts();
		for (const auto &account : accounts) {
			auto *column = new QWidget(selector);
			auto *stack = new QHBoxLayout(column);
			stack->setContentsMargins(0, 0, 4, 0);
			stack->setSpacing(3);
			auto *icon = new QLabel(column);
			const QPixmap image(QStringLiteral(":/xinkx/%1.svg").arg(account.platform));
			if (!image.isNull()) icon->setPixmap(image.scaled(16, 16, Qt::KeepAspectRatio,
				Qt::SmoothTransformation));
			icon->setFixedSize(16, 16);
			stack->addWidget(icon);
			auto *box = new QCheckBox(platformTitle(account.platform), column);
			box->setChecked(!hadChoices || enabled.contains(account.id));
			box->setToolTip(QStringLiteral("%1 · %2\nОтправлять сообщения в этот чат")
				.arg(platformTitle(account.platform), account.displayName()));
			stack->addWidget(box);
			auto *status = new QLabel(view->statusCache.value(account.id, QStringLiteral("Ожидание соединения")), column);
			status->hide();
			box->setToolTip(box->toolTip() + QStringLiteral("\n") + status->text());
			auto *viewer = new QLabel(QStringLiteral("—"), column);
			viewer->setStyleSheet(QStringLiteral("font-size:11px;color:#82d8d3"));
			stack->addWidget(viewer);
			accountRow->addWidget(column);
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
	const auto showProblems = [=] {
		QStringList messages;
		for (const auto &account : gateway->chatAccounts()) {
			const QString state = view->statusCache.value(account.id);
			if (!state.isEmpty() && !state.contains(QStringLiteral("подключён"), Qt::CaseInsensitive) &&
			    !state.contains(QStringLiteral("перенесён"), Qt::CaseInsensitive))
				messages << state;
			messages << view->warnings.value(account.id);
		}
		connectionNotice->setText(messages.join(QLatin1Char('\n')));
		connectionNotice->setVisible(!messages.isEmpty());
	};
	QObject::connect(refresh, &QPushButton::clicked, dock, [=] {
		view->warnings.clear();
		gateway->retryDisconnectedChats();
		rebuild();
		showProblems();
	});
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
		if (!entry->text().trimmed().isEmpty()) entry->clear();
	};
	QObject::connect(send, &QPushButton::clicked, dock, sendCurrent);
	QObject::connect(entry, &QLineEdit::returnPressed, dock, sendCurrent);
	QObject::connect(gateway, &ChatGateway::providerStatus, dock,
		[=](const QString &id, const QString &message) {
			if (view->statusCache.value(id) != message)
				writeDiagnostic(QStringLiteral("provider:%1").arg(id), message);
			view->statusCache[id] = message;
			showProblems();
			if (view->statuses.contains(id)) {
				view->statuses.value(id)->setText(message);
				if (auto *box = view->targets.value(id))
					box->setToolTip(box->text() + QStringLiteral("\n") + message);
			}
		});
	QObject::connect(gateway, &ChatGateway::providerWarning, dock,
		[=](const QString &id, const QString &message) {
			writeDiagnostic(QStringLiteral("provider:%1").arg(id), message);
			if (!view->warnings[id].contains(message)) view->warnings[id].append(message);
			showProblems();
		});
	QObject::connect(gateway, &ChatGateway::viewersReceived, dock, [view](const QString &id, int count) {
		if (auto *label = view->viewers.value(id)) label->setText(count < 0
			? QStringLiteral("—") : QString::number(count));
	});
	QObject::connect(gateway, &ChatGateway::incoming, dock, [=](const ChatLine &message) {
		const auto timestamp = QDateTime::fromMSecsSinceEpoch(message.receivedAt).toString(QStringLiteral("HH:mm:ss"));
		QString formatted = QStringLiteral("%1  [%2] %3").arg(timestamp,
			platformTitle(message.platform), message.name);
		if (message.type == QStringLiteral("chat")) formatted += QStringLiteral(": ") + message.text;
		else formatted += QStringLiteral(" · %1 %2 %3").arg(message.type, message.amount, message.text);
		auto *item = new QListWidgetItem(formatted, feed);
		item->setIcon(QIcon(QStringLiteral(":/xinkx/%1.svg").arg(message.platform)));
		item->setForeground(QColor(message.platform == QStringLiteral("twitch") ? QStringLiteral("#ba91ff")
			: message.platform == QStringLiteral("youtube") ? QStringLiteral("#ff9292")
									   : QStringLiteral("#8be2b4")));
		while (feed->count() > 300) delete feed->takeItem(0);
		feed->scrollToBottom();
	});
	QObject::connect(gateway, &ChatGateway::sendStatus, dock,
		[=](const QString &id, const QString &result, bool success) {
			writeDiagnostic(QStringLiteral("chat:%1:%2").arg(id, success ? QStringLiteral("ok") : QStringLiteral("warning")),
				result);
		});
	gateway->reloadAccounts();
	studioTabs(mainWindow)->addTab(panel, QStringLiteral("Чат"));
}

} // namespace xinkx
