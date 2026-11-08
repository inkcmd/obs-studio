#include "MultiStreamWorkspace.hpp"

#include "OAuthLogin.hpp"
#include "StreamAccounts.hpp"

#include <QCheckBox>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {

namespace {

QSpinBox *spin(QWidget *parent, int minimum, int maximum, int value)
{
	auto *result = new QSpinBox(parent);
	result->setRange(minimum, maximum);
	result->setValue(value);
	result->setSingleStep(2);
	return result;
}

bool editAccount(QWidget *parent, StreamAccount &account, bool creating)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(creating ? QStringLiteral("Добавить площадку") : QStringLiteral("Настроить площадку"));
	dialog.resize(550, 520);
	auto *layout = new QVBoxLayout(&dialog);
	auto *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	auto *platform = new QComboBox(&dialog);
	platform->addItem(QStringLiteral("Twitch"), QStringLiteral("twitch"));
	platform->addItem(QStringLiteral("YouTube"), QStringLiteral("youtube"));
	platform->addItem(QStringLiteral("Kick"), QStringLiteral("kick"));
	const int platformIndex = platform->findData(account.platform);
	if (platformIndex >= 0) {
		platform->setCurrentIndex(platformIndex);
	}
	platform->setEnabled(creating);
	form->addRow(QStringLiteral("Платформа"), platform);
	auto *label = new QLineEdit(account.label, &dialog);
	label->setPlaceholderText(QStringLiteral("Название аккаунта"));
	form->addRow(QStringLiteral("Имя"), label);
	auto *enabled = new QCheckBox(QStringLiteral("Запускать вместе с OBS"), &dialog);
	enabled->setChecked(account.enabled);
	form->addRow(enabled);
	auto *clientId = new QLineEdit(account.clientId, &dialog);
	clientId->setPlaceholderText(QStringLiteral("Client ID из панели разработчика"));
	form->addRow(QStringLiteral("OAuth Client ID"), clientId);
	auto *clientSecret = new QLineEdit(account.clientSecret, &dialog);
	clientSecret->setEchoMode(QLineEdit::Password);
	clientSecret->setPlaceholderText(QStringLiteral("Twitch: обязателен; Google/Kick: если выдан"));
	form->addRow(QStringLiteral("Client Secret"), clientSecret);
	auto *callback = new QLabel(OAuthLogin::redirectUri(), &dialog);
	callback->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(QStringLiteral("OAuth callback"), callback);
	auto *url = new QLineEdit(account.ingestUrl, &dialog);
	url->setPlaceholderText(QStringLiteral("rtmps://... (заполнится через OAuth)"));
	form->addRow(QStringLiteral("RTMP(S) URL"), url);
	auto *key = new QLineEdit(account.streamKey, &dialog);
	key->setEchoMode(QLineEdit::Password);
	key->setPlaceholderText(QStringLiteral("Ключ (не нужен после успешного получения через OAuth)"));
	form->addRow(QStringLiteral("Ключ"), key);
	auto *bitrate = spin(&dialog, 500, 51000, account.bitrate);
	bitrate->setSingleStep(500);
	bitrate->setSuffix(QStringLiteral(" kbps"));
	form->addRow(QStringLiteral("Битрейт"), bitrate);
	auto *width = spin(&dialog, 320, 7680, account.width);
	auto *height = spin(&dialog, 240, 4320, account.height);
	form->addRow(QStringLiteral("Ширина"), width);
	form->addRow(QStringLiteral("Высота"), height);
	auto *encoder = new QComboBox(&dialog);
	encoder->addItem(QStringLiteral("Авто (NVENC, иначе x264)"), QStringLiteral("auto"));
	encoder->addItem(QStringLiteral("NVENC H.264"), QStringLiteral("nvenc"));
	encoder->addItem(QStringLiteral("x264 (CPU)"), QStringLiteral("x264"));
	const int encoderIndex = encoder->findData(account.encoder);
	if (encoderIndex >= 0) {
		encoder->setCurrentIndex(encoderIndex);
	}
	form->addRow(QStringLiteral("Энкодер"), encoder);
	if (creating) {
		QObject::connect(platform, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog,
				 [platform, width, height, bitrate](int) {
					 const QString provider = platform->currentData().toString();
					 width->setValue(provider == QStringLiteral("kick") ? 1280 :
							provider == QStringLiteral("youtube") ? 2560 : 1920);
					 height->setValue(provider == QStringLiteral("kick") ? 720 :
							 provider == QStringLiteral("youtube") ? 1440 : 1080);
					 bitrate->setValue(provider == QStringLiteral("youtube") ? 9000 :
							   provider == QStringLiteral("kick") ? 4500 : 6000);
				 });
	}
	layout->addLayout(form);
	auto *info = new QLabel(QStringLiteral("Основной поток настраивается стандартными средствами OBS выше. "
					     "Здесь только дополнительные выходы. OAuth-ключи и токены "
					     "защищены Windows DPAPI."), &dialog);
	info->setWordWrap(true);
	layout->addWidget(info);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	if (dialog.exec() != QDialog::Accepted) {
		return false;
	}
	account.platform = platform->currentData().toString();
	account.label = label->text().trimmed();
	if (account.label.isEmpty()) {
		account.label = platformTitle(account.platform);
	}
	account.enabled = enabled->isChecked();
	if (account.clientId != clientId->text().trimmed()) {
		account.accessToken.clear();
		account.refreshToken.clear();
		account.expiresAt = 0;
	}
	account.clientId = clientId->text().trimmed();
	account.clientSecret = clientSecret->text().trimmed();
	account.ingestUrl = url->text().trimmed();
	account.streamKey = key->text().trimmed();
	account.bitrate = bitrate->value();
	account.width = width->value();
	account.height = height->value();
	account.encoder = encoder->currentData().toString();
	if (creating) {
		account.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	return true;
}

} // namespace

void installMultiStreamSettings(QWidget *streamPage)
{
	if (!streamPage || !streamPage->layout()) {
		return;
	}
	auto accounts = std::make_shared<QVector<StreamAccount>>(loadStreamAccounts());
	auto *group = new QGroupBox(QStringLiteral("XINKX · Дополнительные трансляции"), streamPage);
	group->setObjectName(QStringLiteral("xinkxStreamAccounts"));
	auto *layout = new QVBoxLayout(group);
	auto *description = new QLabel(QStringLiteral("Основная трансляция использует службу, энкодер и индикаторы OBS. "
						"Когда нажимаешь «Начать трансляцию», отмеченные ниже аккаунты "
						"запускаются параллельно с собственными настройками."), group);
	description->setWordWrap(true);
	layout->addWidget(description);
	auto *tree = new QTreeWidget(group);
	tree->setColumnCount(3);
	tree->setHeaderLabels({QStringLiteral("Запуск"), QStringLiteral("Аккаунт"), QStringLiteral("Состояние")});
	tree->setRootIsDecorated(false);
	tree->setMinimumHeight(150);
	layout->addWidget(tree);
	auto *buttons = new QHBoxLayout();
	auto *add = new QPushButton(QStringLiteral("Добавить"), group);
	auto *edit = new QPushButton(QStringLiteral("Изменить"), group);
	auto *remove = new QPushButton(QStringLiteral("Удалить"), group);
	auto *login = new QPushButton(QStringLiteral("OAuth · Подключить"), group);
	auto *getKey = new QPushButton(QStringLiteral("Получить ключ"), group);
	for (auto *button : {add, edit, remove, login, getKey}) {
		buttons->addWidget(button);
	}
	layout->addLayout(buttons);
	auto *notice = new QLabel(QStringLiteral("OAuth redirect: %1").arg(OAuthLogin::redirectUri()), group);
	notice->setTextFormat(Qt::PlainText);
	notice->setWordWrap(true);
	notice->setTextInteractionFlags(Qt::TextSelectableByMouse);
	notice->setStyleSheet(QStringLiteral("color: #9da9bd;"));
	layout->addWidget(notice);
	streamPage->layout()->addWidget(group);

	auto rebuild = [accounts, tree] {
		tree->blockSignals(true);
		tree->clear();
		for (const auto &a : *accounts) {
			auto *item = new QTreeWidgetItem(tree);
			item->setData(1, Qt::UserRole, a.id);
			item->setCheckState(0, a.enabled ? Qt::Checked : Qt::Unchecked);
			item->setText(1, QStringLiteral("%1 · %2 · %3×%4 · %5 kbps")
					     .arg(platformTitle(a.platform), a.label)
					     .arg(a.width).arg(a.height).arg(a.bitrate));
			const bool ready = !a.ingestUrl.isEmpty() && !a.streamKey.isEmpty();
			const bool oauthConnected = !a.accessToken.isEmpty();
			item->setText(2, ready ? QStringLiteral("● Готов") :
					       oauthConnected ? QStringLiteral("OAuth · нужен ключ") :
								 QStringLiteral("Не настроен"));
			item->setForeground(2, QBrush(QColor(ready ? "#58dea2" : "#9da9bd")));
		}
		tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
		tree->blockSignals(false);
	};
	auto currentIndex = [accounts, tree]() -> int {
		const auto *selected = tree->currentItem();
		if (!selected) {
			return -1;
		}
		const QString id = selected->data(1, Qt::UserRole).toString();
		for (int i = 0; i < accounts->size(); ++i) {
			if ((*accounts)[i].id == id) {
				return i;
			}
		}
		return -1;
	};
	QObject::connect(tree, &QTreeWidget::itemChanged, group, [accounts](QTreeWidgetItem *item, int column) {
		if (column != 0) {
			return;
		}
		for (auto &account : *accounts) {
			if (account.id == item->data(1, Qt::UserRole).toString()) {
				account.enabled = item->checkState(0) == Qt::Checked;
				saveStreamAccounts(*accounts);
				break;
			}
		}
	});
	QObject::connect(add, &QPushButton::clicked, group, [accounts, rebuild, group] {
		StreamAccount account;
		account.platform = QStringLiteral("twitch");
		if (editAccount(group, account, true)) {
			accounts->push_back(account);
			saveStreamAccounts(*accounts);
			rebuild();
		}
	});
	QObject::connect(edit, &QPushButton::clicked, group, [accounts, rebuild, currentIndex, group] {
		const int index = currentIndex();
		if (index >= 0 && editAccount(group, (*accounts)[index], false)) {
			saveStreamAccounts(*accounts);
			rebuild();
		}
	});
	QObject::connect(remove, &QPushButton::clicked, group, [accounts, rebuild, currentIndex, group] {
		const int index = currentIndex();
		if (index < 0 || QMessageBox::question(group, QStringLiteral("Удаление аккаунта"),
						 QStringLiteral("Удалить эту площадку и её локальные токены?")) != QMessageBox::Yes) {
			return;
		}
		accounts->removeAt(index);
		saveStreamAccounts(*accounts);
		rebuild();
	});
	auto *auth = new OAuthLogin(group);
	QObject::connect(login, &QPushButton::clicked, group, [accounts, currentIndex, auth, notice] {
		const int index = currentIndex();
		if (index < 0) {
			return;
		}
		QString error;
		if (auth->begin((*accounts)[index], &error)) {
			notice->setText(QStringLiteral("Открой системный браузер и подтверди доступ к аккаунту"));
		} else {
			notice->setText(error);
		}
	});
	QObject::connect(getKey, &QPushButton::clicked, group, [accounts, currentIndex, auth, notice] {
		if (auth->isBusy()) {
			notice->setText(QStringLiteral("Дождись завершения текущей операции OAuth"));
			return;
		}
		const int index = currentIndex();
		if (index >= 0) {
			notice->setText(QStringLiteral("Получение RTMP-данных через API..."));
			auth->retrieveIngest((*accounts)[index]);
		}
	});
	QObject::connect(auth, &OAuthLogin::finished, group,
			 [accounts, rebuild, notice](const StreamAccount &updated, const QString &message) {
				for (auto &account : *accounts) {
					if (account.id == updated.id) {
						account = updated;
						saveStreamAccounts(*accounts);
						rebuild();
						break;
					}
				}
				notice->setText(message.isEmpty() ? QStringLiteral("OAuth подключён, RTMP-ключ получен") : message);
			});
	QObject::connect(auth, &OAuthLogin::failed, group, [notice](const QString &, const QString &error) {
		notice->setText(error);
	});
	rebuild();
}

} // namespace xinkx
