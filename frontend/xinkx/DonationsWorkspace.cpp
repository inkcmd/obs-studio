#include "DonationGateway.hpp"
#include "OverlayRuntime.hpp"
#include "StreamAccounts.hpp"

#include <widgets/OBSBasic.hpp>

#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVariant>
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
				runtime->publishAlert(QStringLiteral("donation"), QStringLiteral("DonationAlerts"), name, amount, message);
			});
	}
	auto *dialog = new QDialog(mainWindow);
	dialog->setObjectName(QStringLiteral("xinkxDonationSettingsDialog"));
	dialog->setWindowTitle(QStringLiteral("XINKX · Донаты и события"));
	dialog->resize(720, 630);
	auto *outer = new QVBoxLayout(dialog);
	auto *scroll = new QScrollArea(dialog);
	scroll->setWidgetResizable(true);
	auto *panel = new QWidget(scroll);
	auto *layout = new QVBoxLayout(panel);
	layout->setSpacing(10);
	auto *hint = new QLabel(QStringLiteral("Все источники событий подключаются к общему движку XINKX. Сейчас OAuth реализован для DonationAlerts; свои шаблоны можно проверять вручную."), panel);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *provider = new QGroupBox(QStringLiteral("DonationAlerts · OAuth"), panel);
	auto *providerLayout = new QVBoxLayout(provider);
	auto *form = new QFormLayout();
	auto *id = new QLineEdit(donations->clientId(), provider);
	id->setMaxLength(100);
	form->addRow(QStringLiteral("Client ID"), id);
	auto *secret = new QLineEdit(provider);
	secret->setEchoMode(QLineEdit::Password);
	secret->setPlaceholderText(QStringLiteral("Client Secret (только для OAuth)"));
	form->addRow(QStringLiteral("Client Secret"), secret);
	auto *callback = new QLineEdit(donations->callbackUrl(), provider);
	callback->setReadOnly(true);
	form->addRow(QStringLiteral("Redirect URI"), callback);
	providerLayout->addLayout(form);
	auto *help = new QLabel(QStringLiteral("Права: oauth-user-show, oauth-donation-index, oauth-donation-subscribe. Опрос каждые 15 секунд; публичный webhook не нужен."), provider);
	help->setWordWrap(true);
	providerLayout->addWidget(help);
	auto *buttons = new QHBoxLayout();
	auto *connect = new QPushButton(QStringLiteral("Подключить OAuth"), provider);
	auto *refresh = new QPushButton(QStringLiteral("Обновить"), provider);
	auto *disconnect = new QPushButton(QStringLiteral("Отключить"), provider);
	for (auto *button : {connect, refresh, disconnect}) buttons->addWidget(button);
	providerLayout->addLayout(buttons);
	auto *status = new QLabel(donations->connected() ? QStringLiteral("OAuth сохранён (DPAPI)") :
		QStringLiteral("DonationAlerts не подключён"), provider);
	status->setWordWrap(true);
	providerLayout->addWidget(status);
	layout->addWidget(provider);

	auto *presets = new QGroupBox(QStringLiteral("Кастомные шаблоны донатов"), panel);
	auto *presetsLayout = new QVBoxLayout(presets);
	auto *list = new QListWidget(presets);
	list->setMaximumHeight(180);
	presetsLayout->addWidget(list);
	auto *presetActions = new QHBoxLayout();
	auto *add = new QPushButton(QStringLiteral("Добавить"), presets);
	auto *edit = new QPushButton(QStringLiteral("Изменить"), presets);
	auto *remove = new QPushButton(QStringLiteral("Удалить"), presets);
	auto *test = new QPushButton(QStringLiteral("Тест"), presets);
	for (auto *button : {add, edit, remove, test}) presetActions->addWidget(button);
	presetsLayout->addLayout(presetActions);
	auto *presetHelp = new QLabel(QStringLiteral("Шаблон не принимает реальные платежи. Он создаёт тестовое событие в том же движке, куда приходят OAuth-донаты."), presets);
	presetHelp->setWordWrap(true);
	presetsLayout->addWidget(presetHelp);
	layout->addWidget(presets);
	layout->addStretch();
	scroll->setWidget(panel);
	outer->addWidget(scroll);

	auto *storage = new QSettings(studioSettingsPath(), QSettings::IniFormat, dialog);
	const auto persist = [=] {
		QStringList items;
		for (int i = 0; i < list->count(); ++i)
			items.append(QString::fromUtf8(QJsonDocument(list->item(i)->data(Qt::UserRole).toJsonObject())
				.toJson(QJsonDocument::Compact)));
		storage->setValue(QStringLiteral("donations/customPresets"), items);
		storage->sync();
	};
	for (const auto &encoded : storage->value(QStringLiteral("donations/customPresets")).toStringList()) {
		const auto object = QJsonDocument::fromJson(encoded.toUtf8()).object();
		if (object.value(QStringLiteral("name")).toString().isEmpty()) continue;
			auto *row = new QListWidgetItem(object.value(QStringLiteral("name")).toString(), list);
			row->setData(Qt::UserRole, QVariant::fromValue(object));
	}
	if (!storage->contains(QStringLiteral("donations/customPresets"))) {
		for (const auto &amount : {QStringLiteral("100 ₽"), QStringLiteral("500 ₽"), QStringLiteral("1000 ₽")}) {
			const QJsonObject object{{QStringLiteral("name"), QStringLiteral("Донат · ") + amount},
				{QStringLiteral("sender"), QStringLiteral("Тестовый зритель")},
				{QStringLiteral("amount"), amount}, {QStringLiteral("message"), QStringLiteral("Проверка оформления доната")}};
			auto *row = new QListWidgetItem(object.value(QStringLiteral("name")).toString(), list);
			row->setData(Qt::UserRole, QVariant::fromValue(object));
		}
		persist();
	}
	const auto editPreset = [=](bool fresh) {
		if (fresh && list->count() >= 40) return;
		auto *current = fresh ? nullptr : list->currentItem();
		if (!fresh && !current) return;
		const auto old = current ? current->data(Qt::UserRole).toJsonObject() : QJsonObject{};
		QDialog editor(dialog);
		editor.setWindowTitle(fresh ? QStringLiteral("Новый шаблон") : QStringLiteral("Изменить шаблон"));
		auto *fields = new QFormLayout(&editor);
		auto *name = new QLineEdit(old.value(QStringLiteral("name")).toString(), &editor);
		name->setMaxLength(60);
		fields->addRow(QStringLiteral("Название"), name);
		auto *sender = new QLineEdit(old.value(QStringLiteral("sender")).toString(QStringLiteral("Тестовый зритель")), &editor);
		sender->setMaxLength(80);
		fields->addRow(QStringLiteral("Зритель"), sender);
		auto *amount = new QLineEdit(old.value(QStringLiteral("amount")).toString(QStringLiteral("100 ₽")), &editor);
		amount->setMaxLength(60);
		fields->addRow(QStringLiteral("Сумма"), amount);
		auto *message = new QLineEdit(old.value(QStringLiteral("message")).toString(), &editor);
		message->setMaxLength(240);
		fields->addRow(QStringLiteral("Сообщение"), message);
		auto *save = new QPushButton(QStringLiteral("Сохранить"), &editor);
		fields->addRow(save);
		QObject::connect(save, &QPushButton::clicked, &editor, [&] {
			if (name->text().trimmed().isEmpty()) return;
			const QJsonObject object{{QStringLiteral("name"), name->text().trimmed()},
				{QStringLiteral("sender"), sender->text()}, {QStringLiteral("amount"), amount->text()},
				{QStringLiteral("message"), message->text()}};
			if (!current) current = new QListWidgetItem(list);
			current->setText(name->text() + QStringLiteral(" · ") + amount->text());
			current->setData(Qt::UserRole, QVariant::fromValue(object));
			persist();
			editor.accept();
		});
		editor.exec();
	};
	QObject::connect(add, &QPushButton::clicked, dialog, [=] { editPreset(true); });
	QObject::connect(edit, &QPushButton::clicked, dialog, [=] { editPreset(false); });
	QObject::connect(list, &QListWidget::itemDoubleClicked, dialog, [=] { editPreset(false); });
	QObject::connect(remove, &QPushButton::clicked, dialog, [=] {
		delete list->takeItem(list->currentRow());
		persist();
	});
	QObject::connect(test, &QPushButton::clicked, dialog, [=] {
		if (!runtime || !list->currentItem()) return;
		const auto object = list->currentItem()->data(Qt::UserRole).toJsonObject();
		runtime->publishAlert(QStringLiteral("donation"), QStringLiteral("custom"),
			object.value(QStringLiteral("sender")).toString(), object.value(QStringLiteral("amount")).toString(),
			object.value(QStringLiteral("message")).toString());
	});
	QObject::connect(connect, &QPushButton::clicked, dialog, [=] {
		donations->authorize(id->text(), secret->text());
		secret->clear();
	});
	QObject::connect(refresh, &QPushButton::clicked, dialog, [=] { donations->refreshNow(); });
	QObject::connect(disconnect, &QPushButton::clicked, dialog, [=] { donations->disconnectAccount(); });
	QObject::connect(donations, &DonationGateway::statusChanged, status, &QLabel::setText);
	QObject::connect(donations, &DonationGateway::donationReceived, dialog,
		[status](const QString &name, const QString &amount, const QString &) {
			status->setText(QStringLiteral("Донат: %1 · %2").arg(name, amount));
		});
}

} // namespace xinkx
