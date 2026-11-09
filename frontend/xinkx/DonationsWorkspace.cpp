#include "DonationGateway.hpp"
#include "StudioWorkspace.hpp"

#include "OverlayRuntime.hpp"

#include <widgets/OBSBasic.hpp>

#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace xinkx {

void installDonationWorkspace(OBSBasic *mainWindow)
{
	auto *donations = new DonationGateway(mainWindow);
	donations->setObjectName(QStringLiteral("xinkxDonationGateway"));
	auto *runtime = mainWindow->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime"));
	if (runtime) {
		QObject::connect(donations, &DonationGateway::donationReceived, runtime,
			[runtime](const QString &name, const QString &amount, const QString &message) {
				runtime->publishAlert(QStringLiteral("donation"), QStringLiteral("DonationAlerts"),
					name, amount, message);
			});
	}
	auto *dock = studioDock(mainWindow);
	if (!dock) return;
	auto *panel = new QWidget(dock);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 10, 10, 10);
	auto *form = new QFormLayout();
	auto *id = new QLineEdit(donations->clientId(), panel);
	id->setMaxLength(100);
	form->addRow(QStringLiteral("DonationAlerts Client ID"), id);
	auto *secret = new QLineEdit(panel);
	secret->setEchoMode(QLineEdit::Password);
	secret->setPlaceholderText(QStringLiteral("Client Secret (для нового OAuth)") );
	form->addRow(QStringLiteral("Client Secret"), secret);
	auto *callback = new QLineEdit(donations->callbackUrl(), panel);
	callback->setReadOnly(true);
	form->addRow(QStringLiteral("Redirect URI"), callback);
	layout->addLayout(form);
	auto *hint = new QLabel(QStringLiteral("Укажи этот Redirect URI в настройках OAuth приложения DonationAlerts. Права: oauth-user-show, oauth-donation-index, oauth-donation-subscribe. После входа программа сама получает новые донаты локальным опросом API каждые 15 секунд; публичный webhook не нужен."), panel);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *buttons = new QHBoxLayout();
	auto *connect = new QPushButton(QStringLiteral("Подключить OAuth"), panel);
	auto *refresh = new QPushButton(QStringLiteral("Обновить"), panel);
	auto *disconnect = new QPushButton(QStringLiteral("Отключить"), panel);
	buttons->addWidget(connect);
	buttons->addWidget(refresh);
	buttons->addWidget(disconnect);
	layout->addLayout(buttons);
	auto *status = new QLabel(donations->connected()
		? QStringLiteral("DonationAlerts: OAuth сохранён (DPAPI)")
		: QStringLiteral("DonationAlerts: не подключён"), panel);
	status->setWordWrap(true);
	layout->addWidget(status);
	layout->addStretch();
	QObject::connect(connect, &QPushButton::clicked, dock, [=] {
		donations->authorize(id->text(), secret->text());
		secret->clear();
	});
	QObject::connect(refresh, &QPushButton::clicked, dock, [=] { donations->refreshNow(); });
	QObject::connect(disconnect, &QPushButton::clicked, dock, [=] { donations->disconnectAccount(); });
	QObject::connect(donations, &DonationGateway::statusChanged, status, &QLabel::setText);
	QObject::connect(donations, &DonationGateway::donationReceived, dock,
		[status](const QString &name, const QString &amount, const QString &) {
			status->setText(QStringLiteral("Донат: %1 · %2").arg(name, amount));
		});
	studioTabs(mainWindow)->addTab(panel, QStringLiteral("Донаты"));
}

} // namespace xinkx
