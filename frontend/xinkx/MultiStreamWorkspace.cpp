#include "MultiStreamWorkspace.hpp"

#include "OAuthLogin.hpp"
#include "PlatformApi.hpp"
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
#include <QHash>
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
	for (const auto &api : platformApis()) {
		platform->addItem(api.title, api.id);
	}
	platform->addItem(QStringLiteral("RTMP(S)"), QStringLiteral("custom"));
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
	auto *clientId = new QLineEdit(account.oauth.clientId, &dialog);
	clientId->setPlaceholderText(QStringLiteral("Client ID из панели разработчика"));
	form->addRow(QStringLiteral("OAuth Client ID"), clientId);
	auto *clientSecret = new QLineEdit(account.oauth.clientSecret, &dialog);
	clientSecret->setEchoMode(QLineEdit::Password);
	clientSecret->setPlaceholderText(QStringLiteral("Twitch и Kick: обязателен; Google: если выдан"));
	form->addRow(QStringLiteral("Client Secret"), clientSecret);
	auto *callback = new QLabel(OAuthLogin::redirectUri(), &dialog);
	callback->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(QStringLiteral("OAuth callback"), callback);
	auto *youtubeId = new QLineEdit(account.youtubeStreamId, &dialog);
	form->addRow(QStringLiteral("ID видеопотока YouTube"), youtubeId);
	const auto updateOAuthFields = [=] {
		const auto provider = platform->currentData().toString();
		const bool oauth = platformApi(provider) != nullptr;
		for (auto *field : {static_cast<QWidget *>(clientId), static_cast<QWidget *>(clientSecret),
				    static_cast<QWidget *>(callback)}) {
			form->setRowVisible(field, oauth);
		}
		form->setRowVisible(youtubeId, provider == QStringLiteral("youtube"));
	};
	QObject::connect(platform, &QComboBox::currentIndexChanged, &dialog, updateOAuthFields);
	updateOAuthFields();
	auto *url = new QLineEdit(account.destination.ingestUrl, &dialog);
	url->setPlaceholderText(QStringLiteral("rtmps://... (заполнится через OAuth)"));
	form->addRow(QStringLiteral("RTMP(S) URL"), url);
	auto *key = new QLineEdit(account.destination.streamKey, &dialog);
	key->setEchoMode(QLineEdit::Password);
	key->setPlaceholderText(QStringLiteral("Ключ (не нужен после успешного получения через OAuth)"));
	form->addRow(QStringLiteral("Ключ"), key);
	auto *bitrate = spin(&dialog, 500, 51000, account.destination.bitrate);
	bitrate->setSingleStep(500);
	bitrate->setSuffix(QStringLiteral(" kbps"));
	form->addRow(QStringLiteral("Битрейт"), bitrate);
	auto *width = spin(&dialog, 320, 7680, account.destination.width);
	auto *height = spin(&dialog, 240, 4320, account.destination.height);
	form->addRow(QStringLiteral("Ширина"), width);
	form->addRow(QStringLiteral("Высота"), height);
	auto *encoder = new QComboBox(&dialog);
	encoder->addItem(QStringLiteral("Авто (NVENC, иначе x264)"), QStringLiteral("auto"));
	encoder->addItem(QStringLiteral("NVENC H.264"), QStringLiteral("nvenc"));
	encoder->addItem(QStringLiteral("x264 (CPU)"), QStringLiteral("x264"));
	const int encoderIndex = encoder->findData(account.destination.encoder);
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
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
		if ((!url->text().trimmed().isEmpty() && !validIngestUrl(url->text().trimmed())) ||
		    width->value() % 2 || height->value() % 2) {
			QMessageBox::warning(&dialog, dialog.windowTitle(),
					     QStringLiteral("Проверь RTMP(S)-адрес и чётность размеров кадра"));
			return;
		}
		dialog.accept();
	});
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
	const bool manualIngest = account.destination.ingestUrl != url->text().trimmed() ||
				  account.destination.streamKey != key->text().trimmed();
	const bool changedClient = account.oauth.clientId != clientId->text().trimmed() ||
				   account.oauth.clientSecret != clientSecret->text().trimmed();
	const bool changedStream = account.youtubeStreamId != youtubeId->text().trimmed();
	if (changedClient) {
		account.clearAuthorization();
	}
	if (changedStream && account.destination.fromOAuth) {
		account.destination.ingestUrl.clear();
		account.destination.streamKey.clear();
		account.destination.fromOAuth = false;
	}
	account.oauth.clientId = clientId->text().trimmed();
	account.oauth.clientSecret = clientSecret->text().trimmed();
	account.youtubeStreamId = youtubeId->text().trimmed();
	if (manualIngest) {
		account.destination.ingestUrl = url->text().trimmed();
		account.destination.streamKey = key->text().trimmed();
		account.destination.fromOAuth = false;
	}
	account.destination.bitrate = bitrate->value();
	account.destination.width = width->value();
	account.destination.height = height->value();
	account.destination.encoder = encoder->currentData().toString();
	if (creating) {
		account.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	return true;
}

} // namespace

std::function<bool()> installMultiStreamSettings(QWidget *streamPage, QWidget *nativeService, QWidget *nativeStack,
						      const std::function<void()> &changed,
						      const std::function<void(bool)> &busyChanged)
{
	using namespace Qt::StringLiterals;
	auto accounts = std::make_shared<QVector<StreamAccount>>(loadStreamAccounts());
	auto dirty = std::make_shared<bool>(false);
	auto notices = std::make_shared<QHash<QString, QString>>();
	auto *group = new QGroupBox(u"XINKX · Аккаунты трансляции"_s, streamPage);
	group->setObjectName(u"xinkxStreamAccounts"_s);
	auto *layout = new QVBoxLayout(group);
	auto *description = new QLabel(group);
	description->setWordWrap(true);
	layout->addWidget(description);
	auto *tree = new QTreeWidget(group);
	tree->setColumnCount(3);
	tree->setHeaderLabels({u"Запуск"_s, u"Аккаунт"_s, u"Состояние"_s});
	tree->setRootIsDecorated(false);
	tree->setMinimumHeight(150);
	tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	layout->addWidget(tree);

	auto *actions = new QHBoxLayout();
	auto *add = new QPushButton(u"Добавить"_s, group);
	auto *edit = new QPushButton(u"Изменить"_s, group);
	auto *remove = new QPushButton(u"Удалить"_s, group);
	auto *primary = new QPushButton(u"Основной выход"_s, group);
	for (auto *button : {add, edit, remove, primary}) {
		actions->addWidget(button);
	}
	layout->addLayout(actions);
	auto *oauthActions = new QHBoxLayout();
	auto *login = new QPushButton(u"Подключить OAuth"_s, group);
	auto *getKey = new QPushButton(u"Обновить данные"_s, group);
	auto *revoke = new QPushButton(u"Отключить OAuth"_s, group);
	for (auto *button : {login, getKey, revoke}) {
		oauthActions->addWidget(button);
	}
	layout->addLayout(oauthActions);
	auto *notice = new QLabel(u"OAuth callback: %1"_s.arg(OAuthLogin::redirectUri()), group);
	notice->setTextFormat(Qt::PlainText);
	notice->setWordWrap(true);
	layout->addWidget(notice);
	streamPage->layout()->addWidget(group);

	const auto markChanged = [dirty, changed] {
		*dirty = true;
		changed();
	};
	const auto rebuild = [=] {
		const auto selected = tree->currentItem() ? tree->currentItem()->data(1, Qt::UserRole).toString() : QString{};
		tree->blockSignals(true);
		tree->clear();
		bool hasPrimary = false;
		for (const auto &account : *accounts) {
			hasPrimary |= account.primary;
			auto *item = new QTreeWidgetItem(tree);
			item->setData(1, Qt::UserRole, account.id);
			item->setCheckState(0, account.enabled ? Qt::Checked : Qt::Unchecked);
			item->setText(1, u"%1 · %2%3"_s.arg(platformTitle(account.platform), account.displayName(),
							 account.primary ? u" ★ OBS"_s : QString{}));
			const auto &dest = account.destination;
			const bool connected = !account.oauth.requiresAuthorization() && !account.providerId.isEmpty();
			const bool ready = dest.ready() && (!dest.fromOAuth || !account.oauth.accessToken.isEmpty());
			const bool expired = dest.fromOAuth && account.oauth.requiresAuthorization();
			const bool issue = notices->contains(account.id);
			const auto state = !account.enabled ? u"Отключён"_s
					   : issue ? u"Ошибка / требуется действие"_s
					   : expired ? u"OAuth истёк · обнови сессию"_s
					   : ready ? (connected ? u"Подключён · готов"_s : u"Готов · RTMP"_s)
					   : connected ? u"Подключён · нужен ключ"_s
					   : platformApi(account.platform) ? u"Требуется авторизация"_s : u"Нужны параметры RTMP"_s;
			item->setText(2, state);
			item->setToolTip(2, notices->value(account.id));
			if (account.enabled && (issue || expired || connected || ready)) {
				item->setForeground(2, QBrush(QColor(issue || expired ? "#efa064" : "#58dea2")));
			}
			if (account.id == selected) {
				tree->setCurrentItem(item);
			}
		}
		tree->blockSignals(false);
		nativeService->setVisible(!hasPrimary);
		nativeStack->setVisible(!hasPrimary);
		description->setText(hasPrimary ? u"★ — основной выход OBS. Остальные выбранные аккаунты запускаются параллельно."_s
						: u"Основной выход использует настройки OBS. Назначь аккаунт, чтобы управлять им из этого списка."_s);
	};
	const auto current = [accounts, tree]() -> StreamAccount * {
		const auto *item = tree->currentItem();
		if (item) {
			const auto id = item->data(1, Qt::UserRole).toString();
			for (auto &account : *accounts) {
				if (account.id == id) {
					return &account;
				}
			}
		}
		return nullptr;
	};
	QObject::connect(tree, &QTreeWidget::itemChanged, group, [=](QTreeWidgetItem *item, int column) {
		if (column == 0) {
			for (auto &account : *accounts) {
				if (account.id == item->data(1, Qt::UserRole).toString()) {
					account.enabled = item->checkState(0) == Qt::Checked;
					markChanged();
					rebuild();
					break;
				}
			}
		}
	});
	QObject::connect(add, &QPushButton::clicked, group, [=] {
		StreamAccount account;
		account.platform = u"twitch"_s;
		if (editAccount(group, account, true)) {
			accounts->push_back(account);
			markChanged();
			rebuild();
		}
	});
	QObject::connect(edit, &QPushButton::clicked, group, [=] {
		if (auto *account = current(); account && editAccount(group, *account, false)) {
			notices->remove(account->id);
			markChanged();
			rebuild();
		}
	});
	QObject::connect(remove, &QPushButton::clicked, group, [=] {
		const auto *account = current();
		if (!account || QMessageBox::question(group, u"Удалить аккаунт"_s, u"Удалить локальные данные аккаунта?"_s) !=
					QMessageBox::Yes) {
			return;
		}
		const auto id = account->id;
		accounts->removeIf([&](const auto &entry) { return entry.id == id; });
		notices->remove(id);
		markChanged();
		rebuild();
	});
	QObject::connect(primary, &QPushButton::clicked, group, [=] {
		if (const auto *account = current()) {
			const auto id = account->primary ? QString{} : account->id;
			for (auto &entry : *accounts) {
				entry.primary = entry.id == id;
				entry.enabled |= entry.primary;
			}
			markChanged();
			rebuild();
		}
	});

	auto *auth = new OAuthLogin(group);
	const auto setBusy = [=](bool busy) {
		for (auto *button : {add, edit, remove, primary, login, getKey, revoke}) {
			button->setEnabled(!busy);
		}
		tree->setEnabled(!busy);
		busyChanged(busy);
	};
	const auto oauthAccount = [=]() -> StreamAccount * {
		if (*dirty) {
			notice->setText(u"Сначала примени изменения списка аккаунтов"_s);
			return nullptr;
		}
		auto *account = current();
		return account && platformApi(account->platform) ? account : nullptr;
	};
	QObject::connect(login, &QPushButton::clicked, group, [=] {
		if (const auto *account = oauthAccount()) {
			QString error;
			if (auth->begin(*account, &error)) {
				setBusy(true);
				notice->setText(u"Заверши авторизацию в системном браузере"_s);
			} else {
				notices->insert(account->id, error);
				notice->setText(error);
				rebuild();
			}
		}
	});
	QObject::connect(getKey, &QPushButton::clicked, group, [=] {
		if (const auto *account = oauthAccount()) {
			setBusy(true);
			notice->setText(u"Запрос данных аккаунта…"_s);
			auth->retrieveIngest(*account);
		}
	});
	QObject::connect(revoke, &QPushButton::clicked, group, [=] {
		if (const auto *account = oauthAccount()) {
			setBusy(true);
			notice->setText(u"Отзыв OAuth…"_s);
			auth->revoke(*account);
		}
	});
	QObject::connect(auth, &OAuthLogin::finished, group, [=](const StreamAccount &updated, const QString &message, bool success) {
		setBusy(false);
		for (auto &account : *accounts) {
			if (account.id == updated.id) {
				account = updated;
				if (success) {
					notices->remove(account.id);
				} else {
					notices->insert(account.id, message);
				}
				if (!saveStreamAccounts(*accounts)) {
					markChanged();
					notice->setText(u"Не удалось сохранить OAuth через DPAPI. Повтори применение настроек."_s);
				} else {
					notice->setText(message);
				}
				break;
			}
		}
		rebuild();
	});
	QObject::connect(auth, &OAuthLogin::failed, group, [=](const QString &id, const QString &message) {
		setBusy(false);
		notices->insert(id, message);
		notice->setText(message);
		rebuild();
	});
	rebuild();
	return [=] {
		if (*dirty && !saveStreamAccounts(*accounts)) {
			QMessageBox::warning(group, u"Аккаунты трансляции"_s, u"Не удалось сохранить настройки через DPAPI"_s);
			return false;
		}
		*dirty = false;
		return true;
	};
}

} // namespace xinkx
