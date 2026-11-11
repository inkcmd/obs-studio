#include "MultiStreamWorkspace.hpp"

#include "ChatGateway.hpp"
#include "OAuthLogin.hpp"
#include "PlatformApi.hpp"
#include "PortraitCanvas.hpp"
#include "StreamAccounts.hpp"

#include <QCheckBox>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <algorithm>
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

// Reuse the exact catalog and property callbacks used by OBS Settings -> Stream.
// The extra output is an RTMP output, so WHIP, HLS and other protocols cannot be offered here.
struct RtmpServer {
	QString label;
	QString address;
};

struct RtmpCatalog {
	QStringList services;
	QHash<QString, QVector<RtmpServer>> servers;
};

RtmpCatalog loadRtmpCatalog()
{
	RtmpCatalog result;
	OBSProperties properties = obs_get_service_properties("rtmp_common");
	if (!properties) {
		return result;
	}
	OBSDataAutoRelease settings = obs_data_create();
	auto *showAll = obs_properties_get(properties, "show_all");
	auto *services = obs_properties_get(properties, "service");
	auto *servers = obs_properties_get(properties, "server");
	if (!showAll || !services || !servers) {
		return result;
	}
	obs_data_set_bool(settings, "show_all", true);
	obs_property_modified(showAll, settings);
	const size_t count = obs_property_list_item_count(services);
	for (size_t index = 0; index < count; ++index) {
		const char *name = obs_property_list_item_string(services, index);
		if (!name || !*name) {
			continue;
		}
		obs_data_set_string(settings, "service", name);
		obs_property_modified(services, settings);
		const QString protocol = QString::fromUtf8(obs_data_get_string(settings, "protocol"));
		if (protocol.compare(QStringLiteral("RTMP"), Qt::CaseInsensitive) != 0 &&
		    protocol.compare(QStringLiteral("RTMPS"), Qt::CaseInsensitive) != 0) {
			continue;
		}
		QVector<RtmpServer> choices;
		const size_t serverCount = obs_property_list_item_count(servers);
		for (size_t serverIndex = 0; serverIndex < serverCount; ++serverIndex) {
			const char *value = obs_property_list_item_string(servers, serverIndex);
			if (!value || !*value) {
				continue;
			}
			const QString address = QString::fromUtf8(value);
			const QUrl parsed(address, QUrl::StrictMode);
			const bool rtmpAddress = parsed.isValid() && !parsed.host().isEmpty() &&
						 (parsed.scheme() == QStringLiteral("rtmp") ||
						  parsed.scheme() == QStringLiteral("rtmps"));
			// OBS resolves these automatic ingests inside rtmp_common (Twitch, IVS, Nimo TV).
			if (!rtmpAddress && address != QStringLiteral("auto") &&
			    address != QStringLiteral("auto-rtmp") && address != QStringLiteral("auto-rtmps")) {
				continue;
			}
			const char *label = obs_property_list_item_name(servers, serverIndex);
			choices.push_back({QString::fromUtf8(label ? label : value), address});
		}
		if (choices.isEmpty()) {
			continue;
		}
		const QString serviceName = QString::fromUtf8(name);
		result.services.push_back(serviceName);
		result.servers.insert(serviceName, choices);
	}
	result.services.sort(Qt::CaseInsensitive);
	return result;
}

// One YouTube channel owns the OAuth session. Its independent horizontal and
// vertical encoders remain separate StreamAccount destinations internally, so
// existing DPAPI settings and MultiStreamManager outputs need no migration.
bool editYouTubeChannel(QWidget *parent, QVector<StreamAccount> &accounts, const QString &ownerId)
{
	const auto ownerIt = std::find_if(accounts.cbegin(), accounts.cend(), [&](const StreamAccount &item) {
		return item.id == ownerId && item.platform == QStringLiteral("youtube");
	});
	if (ownerIt == accounts.cend()) return false;
	StreamAccount horizontal = *ownerIt;
	if (!horizontal.authorizationSourceId.isEmpty()) return false;
	const auto secondIt = std::find_if(accounts.cbegin(), accounts.cend(), [&](const StreamAccount &item) {
		return item.platform == QStringLiteral("youtube") && item.authorizationSourceId == ownerId;
	});
	StreamAccount vertical;
	const bool hasVertical = secondIt != accounts.cend();
	if (hasVertical) vertical = *secondIt;
	else {
		vertical.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
		vertical.platform = QStringLiteral("youtube");
		vertical.label = QStringLiteral("Вертикальный · %1").arg(horizontal.displayName());
		vertical.authorizationSourceId = horizontal.id;
		vertical.providerId = horizontal.providerId;
		vertical.providerName = horizontal.providerName;
		vertical.oauth = horizontal.oauth;
		vertical.enabled = false;
		vertical.destination = horizontal.destination;
		vertical.destination.ingestUrl.clear();
		vertical.destination.streamKey.clear();
		vertical.destination.fromOAuth = false;
		vertical.destination.bitrate = 9000;
		vertical.destination.width = 1080;
		vertical.destination.height = 1920;
		vertical.destination.portrait = true;
	}
	QDialog dialog(parent);
	dialog.setWindowTitle(QStringLiteral("YouTube · %1").arg(horizontal.displayName()));
	dialog.resize(690, 675);
	auto *layout = new QVBoxLayout(&dialog);
	auto *identity = new QLabel(QStringLiteral("Канал: %1 · один OAuth для обоих видеопотоков")
		.arg(horizontal.providerName.isEmpty() ? horizontal.displayName() : horizontal.providerName), &dialog);
	identity->setWordWrap(true);
	layout->addWidget(identity);
	auto *mode = new QComboBox(&dialog);
	mode->setObjectName(QStringLiteral("xinkxYoutubeDualMode"));
	mode->addItem(QStringLiteral("Только основной · 16:9"), 0);
	mode->addItem(QStringLiteral("Только вертикальный · 9:16"), 1);
	mode->addItem(QStringLiteral("Оба потока · 16:9 + 9:16"), 2);
	mode->setCurrentIndex(vertical.enabled && horizontal.enabled ? 2 : vertical.enabled ? 1 : 0);
	auto *modeForm = new QFormLayout();
	modeForm->addRow(QStringLiteral("Режим трансляции"), mode);
	layout->addLayout(modeForm);
	struct Fields {
		QGroupBox *box;
		QComboBox *stream;
		QPushButton *create;
		QSpinBox *bitrate;
		QSpinBox *width;
		QSpinBox *height;
		QComboBox *encoder;
		QLabel *notice;
	};
	const auto makeFields = [&](const QString &title, const StreamAccount &account) {
		Fields fields{};
		fields.box = new QGroupBox(title, &dialog);
		auto *form = new QFormLayout(fields.box);
		fields.stream = new QComboBox(fields.box);
		fields.stream->setMinimumContentsLength(20);
		fields.stream->addItem(QStringLiteral("Выберите видеопоток YouTube"), QString{});
		if (!account.youtubeStreamId.isEmpty()) {
			fields.stream->addItem(QStringLiteral("Сохранённый · %1").arg(account.youtubeStreamId), account.youtubeStreamId);
			fields.stream->setCurrentIndex(1);
		}
		form->addRow(QStringLiteral("Ключ YouTube"), fields.stream);
		fields.create = new QPushButton(QStringLiteral("Создать видеопоток"), fields.box);
		form->addRow(fields.create);
		fields.bitrate = spin(fields.box, 500, 51000, account.destination.bitrate);
		fields.bitrate->setSuffix(QStringLiteral(" kbps"));
		form->addRow(QStringLiteral("Битрейт"), fields.bitrate);
		fields.width = spin(fields.box, 320, 7680, account.destination.width);
		fields.height = spin(fields.box, 240, 4320, account.destination.height);
		form->addRow(QStringLiteral("Ширина"), fields.width);
		form->addRow(QStringLiteral("Высота"), fields.height);
		fields.encoder = new QComboBox(fields.box);
		fields.encoder->addItem(QStringLiteral("Авто (NVENC / x264)"), QStringLiteral("auto"));
		fields.encoder->addItem(QStringLiteral("NVENC H.264"), QStringLiteral("nvenc"));
		fields.encoder->addItem(QStringLiteral("x264"), QStringLiteral("x264"));
		fields.encoder->setCurrentIndex(std::max(0, fields.encoder->findData(account.destination.encoder)));
		form->addRow(QStringLiteral("Кодировщик"), fields.encoder);
		fields.notice = new QLabel(account.destination.ready() ? QStringLiteral("RTMP(S)-ключ сохранён")
			: QStringLiteral("Ключ появится автоматически после загрузки списка"), fields.box);
		fields.notice->setWordWrap(true);
		form->addRow(fields.notice);
		layout->addWidget(fields.box);
		return fields;
	};
	const Fields main = makeFields(QStringLiteral("Основной · холст OBS 16:9"), horizontal);
	const Fields tall = makeFields(QStringLiteral("Вертикальный · холст XINKX 9:16"), vertical);
	const auto updateMode = [=] {
		main.box->setEnabled(mode->currentIndex() != 1);
		tall.box->setEnabled(mode->currentIndex() != 0);
	};
	QObject::connect(mode, &QComboBox::currentIndexChanged, &dialog, updateMode);
	updateMode();
	auto *status = new QLabel(QStringLiteral("Загрузка ключей через OAuth…"), &dialog);
	status->setWordWrap(true);
	status->setTextFormat(Qt::PlainText);
	layout->addWidget(status);
	auto *refresh = new QPushButton(QStringLiteral("Обновить видеопотоки и ключи"), &dialog);
	layout->addWidget(refresh);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(buttons);
	OAuthLogin auth(&dialog);
	QHash<QString, QJsonObject> available;
	const Fields *creatingFor = nullptr;
	const auto setDestination = [&](StreamAccount &account, const Fields &fields) {
		const QString id = fields.stream->currentData().toString();
		if (account.youtubeStreamId != id) {
			account.destination.streamKey.clear();
			account.destination.ingestUrl.clear();
			account.destination.fromOAuth = false;
		}
		account.youtubeStreamId = id;
		const auto ingest = available.value(id).value(QStringLiteral("cdn")).toObject()
			.value(QStringLiteral("ingestionInfo")).toObject();
		const QString key = ingest.value(QStringLiteral("streamName")).toString();
		const QString url = ingest.value(QStringLiteral("rtmpsIngestionAddress")).toString(
			ingest.value(QStringLiteral("ingestionAddress")).toString());
		if (!key.isEmpty() && validIngestUrl(url)) {
			account.destination.streamKey = key;
			account.destination.ingestUrl = url;
			account.destination.fromOAuth = true;
		}
		account.destination.width = fields.width->value();
		account.destination.height = fields.height->value();
		account.destination.bitrate = fields.bitrate->value();
		account.destination.encoder = fields.encoder->currentData().toString();
		account.destination.portrait = &fields == &tall;
	};
	const auto renderKeys = [&] {
		for (const auto &entry : {std::pair{&main, &horizontal}, std::pair{&tall, &vertical}}) {
			const QString selected = entry.first->stream->currentData().toString();
			const auto ingestion = available.value(selected).value(QStringLiteral("cdn")).toObject()
				.value(QStringLiteral("ingestionInfo")).toObject();
			const bool resolved = !ingestion.value(QStringLiteral("streamName")).toString().isEmpty();
			entry.first->notice->setText(resolved ? QStringLiteral("RTMP(S)-адрес и ключ получены автоматически")
				: entry.second->youtubeStreamId == selected && entry.second->destination.ready()
				? QStringLiteral("Сохранённый RTMP(S)-ключ") : QStringLiteral("Ключ пока недоступен"));
		}
	};
	QObject::connect(&auth, &OAuthLogin::youtubeStreamsLoaded, &dialog,
		[&](const StreamAccount &updated, const QJsonArray &streams) {
			horizontal.oauth = updated.oauth;
			vertical.oauth = updated.oauth;
			available.clear();
			for (const auto &item : streams) {
				const QJsonObject stream = item.toObject();
				const QString id = stream.value(QStringLiteral("id")).toString();
				if (!id.isEmpty()) available.insert(id, stream);
			}
			for (const auto &entry : {std::pair{&main, &horizontal}, std::pair{&tall, &vertical}}) {
				const QSignalBlocker blocked(entry.first->stream);
				const QString old = entry.first->stream->currentData().toString();
				entry.first->stream->clear();
				entry.first->stream->addItem(QStringLiteral("Выберите видеопоток"), QString{});
				for (auto it = available.cbegin(); it != available.cend(); ++it) {
					const QString title = it.value().value(QStringLiteral("snippet")).toObject()
						.value(QStringLiteral("title")).toString(it.key());
					entry.first->stream->addItem(title, it.key());
				}
				int index = entry.first->stream->findData(old);
				if (index < 0 && !old.isEmpty()) {
					entry.first->stream->addItem(QStringLiteral("Сохранённый · %1").arg(old), old);
					index = entry.first->stream->count() - 1;
				}
				entry.first->stream->setCurrentIndex(std::max(0, index));
			}
			status->setText(QStringLiteral("Доступных видеопотоков: %1").arg(streams.size()));
			refresh->setEnabled(true);
			renderKeys();
		});
	QObject::connect(&auth, &OAuthLogin::failed, &dialog, [=](const QString &, const QString &message) {
		status->setText(message);
		refresh->setEnabled(true);
	});
	QObject::connect(&auth, &OAuthLogin::youtubeStreamCreated, &dialog,
		[&](const StreamAccount &updated, const QJsonObject &stream) {
			horizontal.oauth = updated.oauth;
			vertical.oauth = updated.oauth;
			const QString id = stream.value(QStringLiteral("id")).toString();
			if (!id.isEmpty() && creatingFor) {
				available.insert(id, stream);
				const QString title = stream.value(QStringLiteral("snippet")).toObject()
					.value(QStringLiteral("title")).toString(id);
				creatingFor->stream->addItem(title, id);
				creatingFor->stream->setCurrentIndex(creatingFor->stream->findData(id));
				status->setText(QStringLiteral("Видеопоток создан. Проверь привязку вертикального ключа в YouTube Studio."));
			}
			creatingFor = nullptr;
			refresh->setEnabled(true);
			renderKeys();
		});
	const auto refreshKeys = [&] {
		if (auth.isBusy()) return;
		if (horizontal.oauth.accessToken.isEmpty() && horizontal.oauth.refreshToken.isEmpty()) {
			status->setText(QStringLiteral("Подключи OAuth YouTube в настройках трансляции."));
			return;
		}
		refresh->setEnabled(false);
		status->setText(QStringLiteral("Загрузка видеопотоков…"));
		auth.listYouTubeStreams(horizontal);
	};
	QObject::connect(refresh, &QPushButton::clicked, &dialog, refreshKeys);
	const auto createStream = [&](const Fields &fields, bool portrait) {
		if (auth.isBusy()) return;
		if (horizontal.oauth.accessToken.isEmpty() && horizontal.oauth.refreshToken.isEmpty()) {
			status->setText(QStringLiteral("Сначала подключи OAuth YouTube."));
			return;
		}
		bool accepted = false;
		const QString name = QInputDialog::getText(&dialog, QStringLiteral("Новый видеопоток YouTube"),
			QStringLiteral("Название"), QLineEdit::Normal,
			portrait ? QStringLiteral("XINKX Vertical") : QStringLiteral("XINKX Horizontal"), &accepted).trimmed();
		if (!accepted || name.isEmpty()) return;
		creatingFor = &fields;
		refresh->setEnabled(false);
		status->setText(QStringLiteral("Создание видеопотока…"));
		const int resolution = portrait ? fields.width->value() : fields.height->value();
		const QString format = resolution >= 2160 ? QStringLiteral("2160p") :
			resolution >= 1440 ? QStringLiteral("1440p") :
			resolution >= 1080 ? QStringLiteral("1080p") : QStringLiteral("720p");
		auth.createYouTubeStream(horizontal, name, format, QStringLiteral("60fps"));
	};
	QObject::connect(main.create, &QPushButton::clicked, &dialog, [&] { createStream(main, false); });
	QObject::connect(tall.create, &QPushButton::clicked, &dialog, [&] { createStream(tall, true); });
	for (auto *field : {main.stream, tall.stream})
		QObject::connect(field, &QComboBox::currentIndexChanged, &dialog, renderKeys);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
		setDestination(horizontal, main);
		setDestination(vertical, tall);
		horizontal.enabled = mode->currentIndex() != 1;
		vertical.enabled = mode->currentIndex() != 0;
		if ((horizontal.enabled && !horizontal.destination.ready()) ||
		    (vertical.enabled && !vertical.destination.ready()) ||
		    (horizontal.enabled && vertical.enabled &&
		     horizontal.destination.streamKey == vertical.destination.streamKey)) {
			QMessageBox::warning(&dialog, QStringLiteral("YouTube"),
				QStringLiteral("Выбери два разных видеопотока с доступными ключами и корректными параметрами."));
			return;
		}
		dialog.accept();
	});
	QTimer::singleShot(0, &dialog, refreshKeys);
	if (dialog.exec() != QDialog::Accepted) return false;
	for (auto &item : accounts) {
		if (item.id == horizontal.id) item = horizontal;
		if (hasVertical && item.id == vertical.id) item = vertical;
	}
	if (!hasVertical) accounts.push_back(vertical);
	return true;
}

bool editAccount(QWidget *parent, StreamAccount &account, bool creating, const QVector<StreamAccount> &available)
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
	platform->addItem(QStringLiteral("Каталог OBS · RTMP-сервисы"), QStringLiteral("obs_service"));
	platform->addItem(QStringLiteral("RTMP(S)"), QStringLiteral("custom"));
	const int platformIndex = platform->findData(account.platform);
	if (platformIndex >= 0) {
		platform->setCurrentIndex(platformIndex);
	}
	platform->setEnabled(creating);
	form->addRow(QStringLiteral("Платформа"), platform);
	const auto catalog = loadRtmpCatalog();
	auto *service = new QComboBox(&dialog);
	service->setObjectName(QStringLiteral("xinkxRtmpService"));
	for (const auto &name : catalog.services) {
		service->addItem(name, name);
	}
	service->setCurrentIndex(service->findData(account.destination.serviceName));
	form->addRow(QStringLiteral("Сервис OBS"), service);
	auto *server = new QComboBox(&dialog);
	server->setObjectName(QStringLiteral("xinkxRtmpServer"));
	form->addRow(QStringLiteral("Сервер"), server);
	const auto updateServers = [=] {
		const QString name = service->currentData().toString();
		const auto values = catalog.servers.value(name);
		const QString selected = server->currentData().toString();
		server->blockSignals(true);
		server->clear();
		for (const auto &item : values) {
			server->addItem(item.label, item.address);
		}
		const QString prefer = selected.isEmpty() ? account.destination.serviceServer : selected;
		const int existing = server->findData(prefer);
		if (existing >= 0) {
			server->setCurrentIndex(existing);
		} else if (name == account.destination.serviceName && !account.destination.serviceServer.isEmpty()) {
			// Do not silently switch an existing account to another ingest server.
			server->setCurrentIndex(-1);
		}
		server->blockSignals(false);
	};
	QObject::connect(service, &QComboBox::currentIndexChanged, &dialog, updateServers);
	updateServers();
	auto *label = new QLineEdit(account.label, &dialog);
	label->setPlaceholderText(QStringLiteral("Название аккаунта"));
	form->addRow(QStringLiteral("Имя"), label);
	auto *enabled = new QCheckBox(QStringLiteral("Участвует в общей трансляции"), &dialog);
	enabled->setChecked(account.enabled);
	form->addRow(enabled);
	auto *authorization = new QComboBox(&dialog);
	authorization->setObjectName(QStringLiteral("xinkxSharedAuthorization"));
	authorization->addItem(QStringLiteral("Отдельная авторизация"), QString{});
	for (const auto &existing : available) {
		if (existing.platform != QStringLiteral("youtube") || !existing.authorizationSourceId.isEmpty() ||
		    existing.id == account.id || existing.oauth.refreshToken.isEmpty()) continue;
		authorization->addItem(existing.displayName(), existing.id);
	}
	const int existingIndex = authorization->findData(account.authorizationSourceId);
	if (existingIndex >= 0) authorization->setCurrentIndex(existingIndex);
	form->addRow(QStringLiteral("Авторизация YouTube"), authorization);
	auto *clientId = new QLineEdit(account.oauth.clientId, &dialog);
	clientId->setPlaceholderText(QStringLiteral("Client ID из панели разработчика"));
	clientId->setToolTip(QStringLiteral("Client ID публичен. В общей сборке приложения можно распространять только публичный Client ID."));
	form->addRow(QStringLiteral("OAuth Client ID"), clientId);
	auto *clientSecret = new QLineEdit(account.oauth.clientSecret, &dialog);
	clientSecret->setEchoMode(QLineEdit::Password);
	clientSecret->setPlaceholderText(QStringLiteral("Twitch и Kick: обязателен; Google: если выдан"));
	clientSecret->setToolTip(QStringLiteral("Конфиденциальный секрет хранится локально через DPAPI. Не встраивай его в распространяемый EXE."));
	form->addRow(QStringLiteral("Client Secret"), clientSecret);
	auto *callback = new QLabel(OAuthLogin::redirectUri(account.platform), &dialog);
	callback->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(QStringLiteral("OAuth callback"), callback);
	auto *youtubeId = new QComboBox(&dialog);
	youtubeId->setObjectName(QStringLiteral("xinkxInlineYouTubeStreams"));
	youtubeId->setMinimumContentsLength(18);
	youtubeId->addItem(QStringLiteral("Выберите видеопоток"), QString{});
	if (!account.youtubeStreamId.isEmpty()) {
		youtubeId->addItem(QStringLiteral("Сохранённый · %1").arg(account.youtubeStreamId), account.youtubeStreamId);
		youtubeId->setCurrentIndex(1);
	}
	OAuthSession selectedOAuth = account.oauth;
	auto *youtubeStreamRow = new QWidget(&dialog);
	auto *youtubeStreamLayout = new QHBoxLayout(youtubeStreamRow);
	youtubeStreamLayout->setContentsMargins(0, 0, 0, 0);
	youtubeStreamLayout->addWidget(youtubeId);
	auto *selectYoutubeStream = new QPushButton(QStringLiteral("Обновить"), youtubeStreamRow);
	selectYoutubeStream->setObjectName(QStringLiteral("xinkxYouTubeStreamPicker"));
	youtubeStreamLayout->addWidget(selectYoutubeStream);
	auto *createYoutubeStream = new QPushButton(QStringLiteral("Создать"), youtubeStreamRow);
	createYoutubeStream->setObjectName(QStringLiteral("xinkxYouTubeStreamCreate"));
	youtubeStreamLayout->addWidget(createYoutubeStream);
	form->addRow(QStringLiteral("Видеопоток YouTube"), youtubeStreamRow);
	auto *youtubeStreamStatus = new QLabel(&dialog);
	youtubeStreamStatus->setWordWrap(true);
	form->addRow(youtubeStreamStatus);
	const auto updateOAuthFields = [=] {
		const auto provider = platform->currentData().toString();
		callback->setText(OAuthLogin::redirectUri(provider));
		const bool oauth = platformApi(provider) != nullptr;
		const bool shared = provider == QStringLiteral("youtube") && !authorization->currentData().toString().isEmpty();
		for (auto *field : {static_cast<QWidget *>(clientId), static_cast<QWidget *>(clientSecret),
				    static_cast<QWidget *>(callback)}) {
			form->setRowVisible(field, oauth && !shared);
		}
		form->setRowVisible(authorization, provider == QStringLiteral("youtube") && authorization->count() > 1);
		form->setRowVisible(youtubeStreamRow, provider == QStringLiteral("youtube"));
		form->setRowVisible(youtubeStreamStatus, provider == QStringLiteral("youtube"));
		const bool namedService = provider == QStringLiteral("obs_service");
		form->setRowVisible(service, namedService);
		form->setRowVisible(server, namedService);
	};
	QObject::connect(platform, &QComboBox::currentIndexChanged, &dialog, updateOAuthFields);
	QObject::connect(authorization, &QComboBox::currentIndexChanged, &dialog, updateOAuthFields);
	updateOAuthFields();
	auto *url = new QLineEdit(account.destination.ingestUrl, &dialog);
	url->setPlaceholderText(QStringLiteral("rtmps://... (заполнится через OAuth)"));
	form->addRow(QStringLiteral("RTMP(S) URL"), url);
	auto *key = new QLineEdit(account.destination.streamKey, &dialog);
	key->setEchoMode(QLineEdit::Password);
	key->setPlaceholderText(QStringLiteral("Ключ (не нужен после успешного получения через OAuth)"));
	form->addRow(QStringLiteral("Ключ"), key);
	const auto updateIngestFields = [=] {
		const bool namedService = platform->currentData().toString() == QStringLiteral("obs_service");
		form->setRowVisible(url, !namedService);
		key->setPlaceholderText(namedService ? QStringLiteral("Ключ трансляции выбранного сервиса OBS")
						: QStringLiteral("Ключ трансляции или получение через OAuth"));
	};
	QObject::connect(platform, &QComboBox::currentIndexChanged, &dialog, updateIngestFields);
	updateIngestFields();
	auto *bitrate = spin(&dialog, 500, 51000, account.destination.bitrate);
	bitrate->setSingleStep(500);
	bitrate->setSuffix(QStringLiteral(" kbps"));
	form->addRow(QStringLiteral("Битрейт"), bitrate);
	auto *width = spin(&dialog, 320, 7680, account.destination.width);
	auto *height = spin(&dialog, 240, 4320, account.destination.height);
	auto *composition = new QComboBox(&dialog);
	composition->addItem(QStringLiteral("Основной холст OBS"), false);
	composition->addItem(QStringLiteral("Вертикальная композиция 1080×1920"), true);
	composition->setCurrentIndex(account.destination.portrait ? 1 : 0);
	form->addRow(QStringLiteral("Видеокомпозиция"), composition);
	auto *editPortrait = new QPushButton(QStringLiteral("Редактировать вертикальную композицию"), &dialog);
	editPortrait->setToolTip(QStringLiteral("Отдельные позиции и размеры источников для вертикального потока."));
	form->addRow(editPortrait);
	QObject::connect(editPortrait, &QPushButton::clicked, &dialog, [&dialog] {
		PortraitCanvas::instance().edit(&dialog);
	});
	QObject::connect(composition, &QComboBox::currentIndexChanged, &dialog,
		[=] {
			const bool portrait = composition->currentData().toBool();
			editPortrait->setEnabled(portrait);
			if (portrait && width->value() > height->value()) {
				width->setValue(1080);
				height->setValue(1920);
			} else if (!portrait && width->value() < height->value()) {
				width->setValue(1920);
				height->setValue(1080);
			}
		});
	editPortrait->setEnabled(account.destination.portrait);
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
	// The selected stream is a single field in the account editor. Only
	// stream creation opens a separate dialog; choosing a stream never does.
	auto *streamApi = new OAuthLogin(&dialog);
	auto streamResolutions = std::make_shared<QHash<QString, QString>>();
	const auto pickerCredentials = [&](StreamAccount *picker) {
		*picker = account;
		const QString linked = authorization->currentData().toString();
		if (!linked.isEmpty()) {
			const auto origin = std::find_if(available.cbegin(), available.cend(), [&](const StreamAccount &item) {
				return item.id == linked && item.platform == QStringLiteral("youtube");
			});
			if (origin == available.cend()) return false;
			picker->oauth = origin->oauth;
			picker->providerId = origin->providerId;
			picker->platform = origin->platform;
		} else if (creating || account.platform != QStringLiteral("youtube") ||
			   clientId->text().trimmed() != account.oauth.clientId ||
			   clientSecret->text().trimmed() != account.oauth.clientSecret) {
			return false;
		}
		return !picker->oauth.accessToken.isEmpty() || !picker->oauth.refreshToken.isEmpty();
	};
	const auto applyStreamResolution = [=] {
		const QString selected = youtubeId->currentData().toString();
		if (selected.isEmpty() || selected == account.youtubeStreamId) return;
		const QString resolution = streamResolutions->value(selected);
		const int targetHeight = resolution == QStringLiteral("2160p") ? 2160 :
			resolution == QStringLiteral("1440p") ? 1440 :
			resolution == QStringLiteral("1080p") ? 1080 :
			resolution == QStringLiteral("720p") ? 720 : 0;
		if (targetHeight && !composition->currentData().toBool()) {
			width->setValue(targetHeight * 16 / 9);
			height->setValue(targetHeight);
			bitrate->setValue(targetHeight >= 2160 ? 35000 : targetHeight >= 1440 ? 18000 :
				targetHeight >= 1080 ? 9000 : 4500);
		}
	};
	QObject::connect(youtubeId, &QComboBox::currentIndexChanged, &dialog, applyStreamResolution);
	QObject::connect(streamApi, &OAuthLogin::youtubeStreamsLoaded, &dialog,
		[=, &selectedOAuth](const StreamAccount &updated, const QJsonArray &items) {
			selectedOAuth = updated.oauth;
			const QString previous = youtubeId->currentData().toString();
			const QSignalBlocker blocked(youtubeId);
			youtubeId->clear();
			youtubeId->addItem(QStringLiteral("Выберите видеопоток"), QString{});
			streamResolutions->clear();
			for (const auto &value : items) {
				const QJsonObject stream = value.toObject();
				const QString id = stream.value(QStringLiteral("id")).toString();
				if (id.isEmpty()) continue;
				const QString resolution = stream.value(QStringLiteral("cdn")).toObject()
					.value(QStringLiteral("resolution")).toString();
				streamResolutions->insert(id, resolution);
				const QString title = stream.value(QStringLiteral("snippet")).toObject()
					.value(QStringLiteral("title")).toString(id);
				youtubeId->addItem(resolution.isEmpty() ? title : title + QStringLiteral(" · ") + resolution, id);
			}
			const int existing = youtubeId->findData(previous);
			if (existing >= 0) {
				youtubeId->setCurrentIndex(existing);
			} else if (!previous.isEmpty()) {
				youtubeId->addItem(QStringLiteral("Сохранённый · %1").arg(previous), previous);
				youtubeId->setCurrentIndex(youtubeId->count() - 1);
			}
			selectYoutubeStream->setEnabled(true);
			createYoutubeStream->setEnabled(true);
			youtubeStreamStatus->setText(items.isEmpty() ? QStringLiteral("Видеопотоков нет. Создай первый.")
				: QStringLiteral("Видеопотоков: %1. Выбранный поток сохранится с аккаунтом.").arg(items.size()));
		});
	QObject::connect(streamApi, &OAuthLogin::failed, &dialog, [=](const QString &, const QString &message) {
		selectYoutubeStream->setEnabled(true);
		createYoutubeStream->setEnabled(true);
		youtubeStreamStatus->setText(message);
	});
	QObject::connect(streamApi, &OAuthLogin::youtubeStreamCreated, &dialog,
		[=, &selectedOAuth](const StreamAccount &updated, const QJsonObject &stream) {
			selectedOAuth = updated.oauth;
			const QString id = stream.value(QStringLiteral("id")).toString();
			const QString resolution = stream.value(QStringLiteral("cdn")).toObject()
				.value(QStringLiteral("resolution")).toString();
			streamResolutions->insert(id, resolution);
			const QString title = stream.value(QStringLiteral("snippet")).toObject()
				.value(QStringLiteral("title")).toString(id);
			if (youtubeId->findData(id) < 0) youtubeId->addItem(title, id);
			youtubeId->setCurrentIndex(youtubeId->findData(id));
			selectYoutubeStream->setEnabled(true);
			createYoutubeStream->setEnabled(true);
			youtubeStreamStatus->setText(QStringLiteral("Видеопоток создан. Привяжи его к эфиру в YouTube Studio."));
		});
	const auto refreshStreams = [=, &pickerCredentials] {
		if (streamApi->isBusy()) return;
		StreamAccount picker;
		if (!pickerCredentials(&picker)) {
			youtubeStreamStatus->setText(QStringLiteral("Сначала подключи OAuth или выбери существующий YouTube-аккаунт."));
			return;
		}
		selectYoutubeStream->setEnabled(false);
		createYoutubeStream->setEnabled(false);
		youtubeStreamStatus->setText(QStringLiteral("Загрузка видеопотоков…"));
		streamApi->listYouTubeStreams(picker);
	};
	QObject::connect(selectYoutubeStream, &QPushButton::clicked, &dialog, refreshStreams);
	QObject::connect(createYoutubeStream, &QPushButton::clicked, &dialog,
		[=, &pickerCredentials, &dialog] {
			StreamAccount picker;
			if (!pickerCredentials(&picker)) {
				youtubeStreamStatus->setText(QStringLiteral("Сначала подключи OAuth YouTube."));
				return;
			}
			QDialog create(&dialog);
			create.setWindowTitle(QStringLiteral("Новый видеопоток YouTube"));
			auto *createForm = new QFormLayout(&create);
			auto *title = new QLineEdit(QStringLiteral("XINKX Studio"), &create);
			title->setMaxLength(128);
			createForm->addRow(QStringLiteral("Название"), title);
			auto *resolution = new QComboBox(&create);
			for (const auto &choice : {"720p", "1080p", "1440p", "2160p"})
				resolution->addItem(QString::fromLatin1(choice), QString::fromLatin1(choice));
			const int preferred = resolution->findData(QString::number(height->value()) + QLatin1Char('p'));
			resolution->setCurrentIndex(preferred < 0 ? 1 : preferred);
			createForm->addRow(QStringLiteral("Разрешение"), resolution);
			auto *fps = new QComboBox(&create);
			fps->addItem(QStringLiteral("60 FPS"), QStringLiteral("60fps"));
			fps->addItem(QStringLiteral("30 FPS"), QStringLiteral("30fps"));
			createForm->addRow(QStringLiteral("Частота кадров"), fps);
			auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &create);
			createForm->addRow(buttons);
			QObject::connect(buttons, &QDialogButtonBox::accepted, &create, &QDialog::accept);
			QObject::connect(buttons, &QDialogButtonBox::rejected, &create, &QDialog::reject);
			if (create.exec() != QDialog::Accepted) return;
			if (title->text().trimmed().isEmpty()) {
				youtubeStreamStatus->setText(QStringLiteral("Укажи название нового видеопотока."));
				return;
			}
			selectYoutubeStream->setEnabled(false);
			createYoutubeStream->setEnabled(false);
			youtubeStreamStatus->setText(QStringLiteral("Создание видеопотока…"));
			streamApi->createYouTubeStream(picker, title->text().trimmed(), resolution->currentData().toString(),
				fps->currentData().toString());
		});
	QObject::connect(authorization, &QComboBox::currentIndexChanged, &dialog, [=] {
		if (platform->currentData().toString() == QStringLiteral("youtube")) refreshStreams();
	});
	QTimer::singleShot(0, &dialog, [=] {
		if (platform->currentData().toString() == QStringLiteral("youtube")) refreshStreams();
	});
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
		const bool namedService = platform->currentData().toString() == QStringLiteral("obs_service");
		if ((namedService && (service->currentData().toString().isEmpty() || server->currentData().toString().isEmpty())) ||
		    (!namedService && !url->text().trimmed().isEmpty() && !validIngestUrl(url->text().trimmed())) ||
		    width->value() % 2 || height->value() % 2 ||
		    (composition->currentData().toBool() && width->value() * 16 != height->value() * 9)) {
			QMessageBox::warning(&dialog, dialog.windowTitle(),
					     QStringLiteral("Проверь RTMP, размеры кадра и пропорции 9:16 для вертикального выхода"));
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
		account.label = account.platform == QStringLiteral("obs_service") ? service->currentText()
									   : platformTitle(account.platform);
	}
	account.enabled = enabled->isChecked();
	const QString linked = platform->currentData().toString() == QStringLiteral("youtube")
		? authorization->currentData().toString() : QString{};
	account.authorizationSourceId = linked;
	const bool manualIngest = account.destination.ingestUrl != url->text().trimmed() ||
				  account.destination.streamKey != key->text().trimmed();
	const bool changedClient = account.oauth.clientId != clientId->text().trimmed() ||
				   account.oauth.clientSecret != clientSecret->text().trimmed();
	const bool changedStream = account.youtubeStreamId != youtubeId->currentData().toString();
	if (changedClient && linked.isEmpty()) {
		account.clearAuthorization();
	} else if (linked.isEmpty()) {
		account.oauth = selectedOAuth;
	}
	if (changedStream && account.destination.fromOAuth) {
		account.destination.ingestUrl.clear();
		account.destination.streamKey.clear();
		account.destination.fromOAuth = false;
	}
	if (!linked.isEmpty()) {
		const auto origin = std::find_if(available.cbegin(), available.cend(), [&](const StreamAccount &item) {
			return item.id == linked && item.platform == QStringLiteral("youtube");
		});
		if (origin != available.cend()) {
			account.oauth = origin->oauth;
			account.providerId = origin->providerId;
			account.providerName = origin->providerName;
		}
	} else {
		account.oauth.clientId = clientId->text().trimmed();
		account.oauth.clientSecret = clientSecret->text().trimmed();
	}
	account.youtubeStreamId = youtubeId->currentData().toString();
	if (manualIngest) {
		account.destination.ingestUrl = url->text().trimmed();
		account.destination.streamKey = key->text().trimmed();
		account.destination.fromOAuth = false;
	}
	account.destination.serviceName = account.platform == QStringLiteral("obs_service") ? service->currentData().toString()
											     : QString{};
	account.destination.serviceServer = account.platform == QStringLiteral("obs_service") ? server->currentData().toString()
												 : QString{};
	account.destination.bitrate = bitrate->value();
	account.destination.width = width->value();
	account.destination.height = height->value();
	account.destination.portrait = composition->currentData().toBool();
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
	// Remember the chosen YouTube output mode while the group checkbox is off.
	auto youtubeMode = std::make_shared<QHash<QString, QPair<bool, bool>>>();
	auto *group = new QGroupBox(u"XINKX · Выходы трансляции"_s, streamPage);
	group->setObjectName(u"xinkxStreamAccounts"_s);
	auto *layout = new QVBoxLayout(group);
	auto *description = new QLabel(group);
	description->setWordWrap(true);
	layout->addWidget(description);
	auto *showNative = new QCheckBox(u"Показать штатные настройки OBS (режим без аккаунтов XINKX)"_s, group);
	showNative->setToolTip(u"Штатная служба OBS сохраняется в профиле, но не запускается вместе с выходами XINKX."_s);
	layout->addWidget(showNative);
	auto *tree = new QTreeWidget(group);
	tree->setColumnCount(3);
	tree->setHeaderLabels({u"Эфир"_s, u"Выход"_s, u"Состояние"_s});
	tree->setRootIsDecorated(false);
	tree->setMinimumHeight(150);
	tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	layout->addWidget(tree);

	auto *actions = new QHBoxLayout();
	auto *add = new QPushButton(u"Добавить"_s, group);
	auto *edit = new QPushButton(u"Изменить"_s, group);
	auto *remove = new QPushButton(u"Удалить"_s, group);
	for (auto *button : {add, edit, remove}) {
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
	auto *pendingActions = new QHBoxLayout();
	auto *copyLink = new QPushButton(u"Скопировать ссылку OAuth"_s, group);
	auto *cancelAuth = new QPushButton(u"Отменить ожидание"_s, group);
	copyLink->hide();
	cancelAuth->hide();
	pendingActions->addWidget(copyLink);
	pendingActions->addWidget(cancelAuth);
	layout->addLayout(pendingActions);
	auto *notice = new QLabel(u"OAuth callback: http://localhost:8765/api/oauth/{platform}/callback"_s, group);
	notice->setTextFormat(Qt::PlainText);
	notice->setWordWrap(true);
	layout->addWidget(notice);
	streamPage->layout()->addWidget(group);

	const auto updateActions = [=] {
		const auto *selected = tree->currentItem();
		const auto id = selected ? selected->data(1, Qt::UserRole).toString() : QString{};
		const auto it = std::find_if(accounts->cbegin(), accounts->cend(),
					     [&](const StreamAccount &candidate) { return candidate.id == id; });
		const bool hasSelection = it != accounts->cend();
		const bool oauth = hasSelection && platformApi(it->platform);
		edit->setEnabled(hasSelection);
		remove->setEnabled(hasSelection);
		login->setEnabled(oauth && it->authorizationSourceId.isEmpty());
		getKey->setEnabled(oauth);
		revoke->setEnabled(oauth && it->authorizationSourceId.isEmpty());
	};
	const auto markChanged = [dirty, changed] {
		*dirty = true;
		changed();
	};
	const auto rebuild = [=] {
		const auto selected = tree->currentItem() ? tree->currentItem()->data(1, Qt::UserRole).toString() : QString{};
		tree->blockSignals(true);
		tree->clear();
		for (const auto &account : *accounts) {
			// A secondary YouTube output is a destination of the same channel,
			// not a second login or a second row in the account list.
			if (account.platform == u"youtube"_s && !account.authorizationSourceId.isEmpty()) continue;
			auto *item = new QTreeWidgetItem(tree);
			item->setData(1, Qt::UserRole, account.id);
			const auto sibling = std::find_if(accounts->cbegin(), accounts->cend(), [&](const StreamAccount &other) {
				return other.platform == u"youtube"_s && other.authorizationSourceId == account.id;
			});
			const bool hasSecondary = sibling != accounts->cend();
			const bool active = account.enabled || (hasSecondary && sibling->enabled);
			item->setCheckState(0, active ? Qt::Checked : Qt::Unchecked);
			const QString provider = account.platform == u"obs_service"_s
						 ? account.destination.serviceName : platformTitle(account.platform);
			item->setText(1, u"%1 · %2"_s.arg(provider, account.displayName()));
			item->setIcon(1, QIcon(QStringLiteral(":/xinkx/%1.svg").arg(account.platform)));
			const auto &dest = account.destination;
			const bool connected = !account.oauth.requiresAuthorization() && !account.providerId.isEmpty();
			const bool ready = dest.ready() && (!dest.fromOAuth || !account.oauth.accessToken.isEmpty());
			const bool secondaryReady = hasSecondary && sibling->destination.ready();
			const bool expired = dest.fromOAuth && account.oauth.requiresAuthorization();
			const bool issue = notices->contains(account.id);
			const auto state = !active ? u"Отключён"_s
					   : issue ? u"Ошибка / требуется действие"_s
					   : expired ? u"OAuth истёк · обнови сессию"_s
					   : hasSecondary ? u"%1 · основной %2 / вертикальный %3"_s
						.arg(account.enabled && sibling->enabled ? u"Оба"_s : account.enabled ? u"16:9"_s : u"9:16"_s,
							ready ? u"готов"_s : u"нет ключа"_s,
							secondaryReady ? u"готов"_s : u"нет ключа"_s)
					   : ready ? (connected ? u"Подключён · готов"_s : u"Готов · RTMP"_s)
					   : connected ? u"Подключён · нужен ключ"_s
					   : platformApi(account.platform) ? u"Требуется авторизация"_s : u"Нужны сервер и ключ"_s;
			item->setText(2, state);
			item->setToolTip(2, notices->value(account.id));
			if (active && (issue || expired || connected || ready || secondaryReady)) {
				item->setForeground(2, QBrush(QColor(issue || expired ? "#efa064" : "#58dea2")));
			}
			if (account.id == selected) {
				tree->setCurrentItem(item);
			}
		}
		if (!tree->currentItem() && tree->topLevelItemCount() > 0) {
			tree->setCurrentItem(tree->topLevelItem(0));
		}
		tree->blockSignals(false);
		updateActions();
		const bool hasAccounts = !accounts->isEmpty();
		showNative->setVisible(hasAccounts);
		const bool stockVisible = !hasAccounts || showNative->isChecked();
		nativeService->setVisible(stockVisible);
		nativeStack->setVisible(stockVisible);
		description->setText(hasAccounts
					 ? u"Все отмеченные выходы равноправны: каждый кодирует и передаёт видео независимо. Общая кнопка запускает и останавливает их вместе; штатная служба OBS не используется."_s
					 : u"Пока выходы XINKX не добавлены, работает штатная служба OBS из настроек выше."_s);
	};
	QObject::connect(tree, &QTreeWidget::currentItemChanged, group, [=] { updateActions(); });
	QObject::connect(showNative, &QCheckBox::toggled, group, [=] { rebuild(); });
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
					const bool on = item->checkState(0) == Qt::Checked;
					auto linked = std::find_if(accounts->begin(), accounts->end(), [&](const StreamAccount &entry) {
						return entry.platform == u"youtube"_s && entry.authorizationSourceId == account.id;
					});
					if (linked == accounts->end()) account.enabled = on;
					else if (!on) {
						if (account.enabled || linked->enabled)
							youtubeMode->insert(account.id, {account.enabled, linked->enabled});
						account.enabled = linked->enabled = false;
					} else {
						const auto previous = youtubeMode->value(account.id, {true, false});
						account.enabled = previous.first;
						linked->enabled = previous.second;
					}
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
		if (editAccount(group, account, true, *accounts)) {
			accounts->push_back(account);
			markChanged();
			rebuild();
		}
	});
	QObject::connect(edit, &QPushButton::clicked, group, [=] {
		const auto *account = current();
		if (!account) return;
		const QString id = account->id;
		const bool updated = account->platform == u"youtube"_s && account->authorizationSourceId.isEmpty()
			? editYouTubeChannel(group, *accounts, id)
			: editAccount(group, *current(), false, *accounts);
		if (updated) {
			notices->remove(id);
			markChanged();
			rebuild();
		}
	});
	QObject::connect(tree, &QTreeWidget::itemDoubleClicked, group, [edit](QTreeWidgetItem *, int column) {
		if (column != 0 && edit->isEnabled()) edit->click();
	});
	QObject::connect(remove, &QPushButton::clicked, group, [=] {
		const auto *account = current();
		if (!account) return;
		const auto id = account->id;
		const bool linked = std::any_of(accounts->cbegin(), accounts->cend(), [&](const StreamAccount &item) {
			return item.authorizationSourceId == id;
		});
		if (QMessageBox::question(group, u"Удалить аккаунт"_s,
			linked ? u"Удалить канал и оба связанных выхода вместе с локальными настройками?"_s
			       : u"Удалить локальные данные аккаунта?"_s) !=
					QMessageBox::Yes) {
			return;
		}
		accounts->removeIf([&](const auto &entry) {
			return entry.id == id || entry.authorizationSourceId == id;
		});
		notices->remove(id);
		markChanged();
		rebuild();
	});
	auto *auth = new OAuthLogin(group);
	const auto setBusy = [=](bool busy) {
		copyLink->setVisible(busy && !auth->authorizationUrl().isEmpty());
		cancelAuth->setVisible(busy);
		if (busy) {
			for (auto *button : {add, edit, remove, login, getKey, revoke}) {
				button->setEnabled(false);
			}
		} else {
			add->setEnabled(true);
			updateActions();
		}
		tree->setEnabled(!busy);
		busyChanged(busy);
	};
	QObject::connect(copyLink, &QPushButton::clicked, group, [=] {
		QGuiApplication::clipboard()->setText(auth->authorizationUrl());
		notice->setText(u"Ссылка скопирована. Открой её в браузере, где выполнен вход на площадку."_s);
	});
	QObject::connect(cancelAuth, &QPushButton::clicked, auth, &OAuthLogin::cancel);
	QObject::connect(auth, &OAuthLogin::cancelled, group, [=](const QString &) {
		setBusy(false);
		notice->setText(u"Авторизация отменена. Можно закрыть настройки или начать заново."_s);
	});
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
				notice->setText(u"Заверши авторизацию в браузере. Если открылся не тот профиль, скопируй ссылку."_s);
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
				if (success && !account.authorizationSourceId.isEmpty()) {
					for (auto &owner : *accounts)
						if (owner.id == account.authorizationSourceId && owner.platform == account.platform) {
							owner.oauth = account.oauth;
							break;
						}
				}
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
					if (auto *host = group->window()->parentWidget())
						if (auto *gateway = host->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway")))
							gateway->reloadAccounts();
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
		if (auto *window = static_cast<QWidget *>(obs_frontend_get_main_window()))
			QTimer::singleShot(0, window, [] {
				PortraitCanvas::instance().setViewMode(2);
			});
		return true;
	};
}

} // namespace xinkx
