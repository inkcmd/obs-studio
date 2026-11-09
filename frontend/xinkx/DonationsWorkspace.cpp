#include "DonationGateway.hpp"
#include "OverlayRuntime.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace xinkx {

void installDonationWorkspace(OBSBasic *mainWindow)
{
	auto *donations = new DonationGateway(mainWindow);
	donations->setObjectName(QStringLiteral("xinkxDonationGateway"));
	auto *runtime = mainWindow->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime"));
	if (runtime) {
		QObject::connect(donations, &DonationGateway::donationEvent, runtime,
			[runtime](const QString &provider, const QString &name, const QString &amount, const QString &message) {
				runtime->publishAlert(QStringLiteral("donation"), provider, name, amount, message);
			});
	}
}

QWidget *createDonationsSettingsPage(OBSBasic *mainWindow, QWidget *parent)
{
	auto *donations = mainWindow->findChild<DonationGateway *>(QStringLiteral("xinkxDonationGateway"));
	auto *page = new QWidget(parent);
	auto *layout = new QVBoxLayout(page);
	layout->setSpacing(12);
	auto *intro = new QLabel(QStringLiteral(
		"Все реальные события попадают в один XINKX-движок. Подключения и оформление уведомлений "
		"независимы: здесь указываются источники платежных событий, а внешний вид задаётся у источника XINKX в OBS."), page);
	intro->setWordWrap(true);
	layout->addWidget(intro);
	if (!donations) return page;

	auto *provider = new QGroupBox(QStringLiteral("DonationAlerts · OAuth"), page);
	auto *providerForm = new QFormLayout(provider);
	auto *clientId = new QLineEdit(donations->clientId(), provider);
	clientId->setMaxLength(100);
	auto *secret = new QLineEdit(provider);
	secret->setEchoMode(QLineEdit::Password);
	secret->setPlaceholderText(QStringLiteral("Client Secret (сохранённый вводить повторно не нужно)"));
	providerForm->addRow(QStringLiteral("Client ID"), clientId);
	providerForm->addRow(QStringLiteral("Client Secret"), secret);
	providerForm->addRow(QStringLiteral("Callback"), new QLabel(donations->callbackUrl(), provider));
	auto *authButtons = new QHBoxLayout();
	auto *connect = new QPushButton(QStringLiteral("Подключить"), provider);
	auto *refresh = new QPushButton(QStringLiteral("Проверить события"), provider);
	auto *disconnect = new QPushButton(QStringLiteral("Отключить"), provider);
	for (auto *button : {connect, refresh, disconnect}) authButtons->addWidget(button);
	providerForm->addRow(authButtons);
	auto *status = new QLabel(donations->connected() ? QStringLiteral("OAuth подключён") :
		QStringLiteral("OAuth не подключён"), provider);
	status->setWordWrap(true);
	providerForm->addRow(status);
	layout->addWidget(provider);
	QObject::connect(connect, &QPushButton::clicked, page, [=] {
		donations->authorize(clientId->text(), secret->text());
		secret->clear();
	});
	QObject::connect(refresh, &QPushButton::clicked, page, [=] { donations->refreshNow(); });
	QObject::connect(disconnect, &QPushButton::clicked, page, [=] { donations->disconnectAccount(); });
	QObject::connect(donations, &DonationGateway::statusChanged, status, &QLabel::setText);

	auto *feedsBox = new QGroupBox(QStringLiteral("Другие сайты и платёжные шлюзы · JSON API"), page);
	auto *feedsLayout = new QVBoxLayout(feedsBox);
	auto *help = new QLabel(QStringLiteral(
		"HTTPS API с Bearer-токеном (токен хранится через Windows DPAPI). "
		"Ответ: {\"data\":[{\"id\":\"123\",\"name\":\"Зритель\",\"amount\":100,\"currency\":\"RUB\",\"message\":\"...\"}]}. "
		"ID каждого события должен быть стабильным; существующие платежи при подключении пропускаются. "
		"Банковские подписи, webhook и отдельные OAuth-процедуры требуют адаптера конкретного провайдера."), feedsBox);
	help->setWordWrap(true);
	feedsLayout->addWidget(help);
	auto *list = new QListWidget(feedsBox);
	list->setMinimumHeight(130);
	feedsLayout->addWidget(list);
	auto *feedActions = new QHBoxLayout();
	auto *add = new QPushButton(QStringLiteral("Добавить API"), feedsBox);
	auto *edit = new QPushButton(QStringLiteral("Изменить"), feedsBox);
	auto *remove = new QPushButton(QStringLiteral("Удалить"), feedsBox);
	for (auto *button : {add, edit, remove}) feedActions->addWidget(button);
	feedsLayout->addLayout(feedActions);
	layout->addWidget(feedsBox);
	layout->addStretch();

	const auto populate = [=] {
		const QString previous = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString{};
		list->clear();
		for (const auto &feed : donations->feeds()) {
			auto *row = new QListWidgetItem(feed.name + (feed.enabled ? QStringLiteral(" · активен") :
				QStringLiteral(" · выключен")), list);
			row->setData(Qt::UserRole, feed.id);
			row->setToolTip(feed.url);
			if (feed.id == previous) list->setCurrentItem(row);
		}
	};
	const auto openEditor = [=](bool fresh) {
		DonationGateway::Feed feed;
		if (!fresh) {
			if (!list->currentItem()) return;
			for (const auto &saved : donations->feeds())
				if (saved.id == list->currentItem()->data(Qt::UserRole).toString()) feed = saved;
		}
		QDialog editor(page);
		editor.setWindowTitle(fresh ? QStringLiteral("Новый источник событий") :
			QStringLiteral("Изменить источник событий"));
		auto *fields = new QFormLayout(&editor);
		auto *name = new QLineEdit(feed.name, &editor);
		auto *url = new QLineEdit(feed.url, &editor);
		url->setPlaceholderText(QStringLiteral("https://provider.example/api/donations"));
		auto *token = new QLineEdit(&editor);
		token->setEchoMode(QLineEdit::Password);
		token->setPlaceholderText(feed.hasToken ? QStringLiteral("Оставь пустым, чтобы сохранить токен") :
			QStringLiteral("Bearer token, если необходим"));
		auto *enabled = new QCheckBox(QStringLiteral("Опрос включён"), &editor);
		enabled->setChecked(fresh || feed.enabled);
		fields->addRow(QStringLiteral("Название"), name);
		fields->addRow(QStringLiteral("URL JSON API"), url);
		fields->addRow(QStringLiteral("Bearer token"), token);
		fields->addRow(enabled);
		auto *save = new QPushButton(QStringLiteral("Сохранить"), &editor);
		fields->addRow(save);
		QObject::connect(save, &QPushButton::clicked, &editor, [&] {
			feed.name = name->text();
			feed.url = url->text();
			feed.enabled = enabled->isChecked();
			QString error;
			if (!donations->saveFeed(feed, token->text(), &error)) {
				QMessageBox::warning(&editor, QStringLiteral("Источник событий"), error);
				return;
			}
			editor.accept();
		});
		if (editor.exec() == QDialog::Accepted) populate();
	};
	QObject::connect(add, &QPushButton::clicked, page, [=] { openEditor(true); });
	QObject::connect(edit, &QPushButton::clicked, page, [=] { openEditor(false); });
	QObject::connect(list, &QListWidget::itemDoubleClicked, page, [=] { openEditor(false); });
	QObject::connect(remove, &QPushButton::clicked, page, [=] {
		if (!list->currentItem()) return;
		donations->removeFeed(list->currentItem()->data(Qt::UserRole).toString());
		populate();
	});
	populate();
	return page;
}

} // namespace xinkx
