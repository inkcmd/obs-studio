#include "OverlayRuntime.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QHostAddress>
#include <QTcpSocket>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

namespace xinkx {

namespace {

constexpr quint16 kPort = 8766;
const QByteArray kSourceName = QStringLiteral("XINKX · Эффекты и события").toUtf8();

QByteArray frame(const QByteArray &name, const QJsonObject &payload)
{
	return "event: " + name + "\ndata: " + QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n\n";
}

void writeChunk(QTcpSocket *socket, const QByteArray &body)
{
	socket->write(QByteArray::number(body.size(), 16) + "\r\n" + body + "\r\n");
}

QString shortText(const QString &text, int maximum = 240)
{
	QString result = text.left(maximum);
	result.remove(QLatin1Char('\0'));
	return result;
}

} // namespace

OverlayRuntime::OverlayRuntime(QObject *parent) : QObject(parent)
{
	connect(&listener, &QTcpServer::newConnection, this, &OverlayRuntime::acceptConnections);
	listener.listen(QHostAddress::LocalHost, kPort);
	tick.setInterval(250);
	connect(&tick, &QTimer::timeout, this, [this] {
		if (deadlineMs <= 0) {
			return;
		}
		const int left = secondsRemaining();
		emit timerChanged(left, true);
		if (!left) {
			deadlineMs = 0;
			remaining = 0;
			timerVisible = false;
			stateChanged();
			emit timerChanged(0, false);
		}
	});
	tick.start();
	auto *heartbeat = new QTimer(this);
	heartbeat->setInterval(20000);
	connect(heartbeat, &QTimer::timeout, this, [this] {
		const auto snapshot = clients;
		for (auto *socket : snapshot) {
			if (socket->bytesToWrite() > 262144) {
				socket->disconnectFromHost();
			} else {
				writeChunk(socket, ": keepalive\n\n");
			}
		}
	});
	heartbeat->start();
}

QString OverlayRuntime::error() const
{
	return available() ? QString{} : QStringLiteral("Порт 8766 занят: ") + listener.errorString();
}

QString OverlayRuntime::sourceUrl() const
{
	return QStringLiteral("http://127.0.0.1:8766/overlay");
}

void OverlayRuntime::acceptConnections()
{
	while (auto *socket = listener.nextPendingConnection()) {
		auto *buffer = new QByteArray();
		connect(socket, &QTcpSocket::disconnected, socket, [this, socket, buffer] {
			clients.remove(socket);
			delete buffer;
			socket->deleteLater();
		});
		connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
			if (socket->property("xinkxHandled").toBool()) {
				// This HTTP/1.1 connection only transmits server-sent events.
				socket->readAll();
				return;
			}
			buffer->append(socket->readAll());
			if (buffer->size() > 8192) {
				socket->disconnectFromHost();
				return;
			}
			const int end = buffer->indexOf("\r\n\r\n");
			if (end < 0) {
				return;
			}
			socket->setProperty("xinkxHandled", true);
			handleRequest(socket, buffer->left(end));
		});
		QTimer::singleShot(5000, socket, [socket] {
			if (!socket->property("xinkxHandled").toBool()) {
				socket->disconnectFromHost();
			}
		});
	}
}

void OverlayRuntime::handleRequest(QTcpSocket *socket, const QByteArray &header)
{
	const auto requestLine = header.left(header.indexOf("\r\n") < 0 ? header.size() : header.indexOf("\r\n"));
	if (requestLine == "GET /events HTTP/1.1") {
		if (clients.size() >= 32) {
			socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
			socket->disconnectFromHost();
			return;
		}
		socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
			      "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
			      "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n");
		clients.insert(socket);
		sendEvent(socket, "state", {{QStringLiteral("position"), position},
						    {QStringLiteral("paused"), effectsPaused},
						    {QStringLiteral("visible"), timerVisible},
						    {QStringLiteral("running"), deadlineMs > 0},
						    {QStringLiteral("remaining"), secondsRemaining()},
						    {QStringLiteral("deadline"), deadlineMs}});
		return;
	}
	if (requestLine == "GET /overlay HTTP/1.1") {
		QFile html(QStringLiteral(":/xinkx/overlay.html"));
		if (html.open(QIODevice::ReadOnly)) {
			const auto bytes = html.readAll();
			socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
					      "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
					      "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; "
					      "script-src 'unsafe-inline'; connect-src 'self'\r\n"
					      "Content-Length: " + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
			socket->disconnectFromHost();
			return;
		}
	}
	socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	socket->disconnectFromHost();
}

void OverlayRuntime::sendEvent(QTcpSocket *socket, const QByteArray &name, const QJsonObject &payload)
{
	if (socket->state() == QAbstractSocket::ConnectedState && socket->bytesToWrite() < 262144) {
		writeChunk(socket, frame(name, payload));
	}
}

void OverlayRuntime::broadcast(const QByteArray &name, const QJsonObject &payload)
{
	const auto snapshot = clients;
	for (auto *socket : snapshot) {
		if (socket->bytesToWrite() >= 262144) {
			socket->disconnectFromHost();
		} else {
			sendEvent(socket, name, payload);
		}
	}
}

int OverlayRuntime::secondsRemaining() const
{
	return deadlineMs > 0
		       ? static_cast<int>(std::max<qint64>(0, (deadlineMs - QDateTime::currentMSecsSinceEpoch() + 999) / 1000))
		       : remaining;
}

void OverlayRuntime::stateChanged()
{
	broadcast("state", {{QStringLiteral("position"), position},
			    {QStringLiteral("paused"), effectsPaused},
			    {QStringLiteral("visible"), timerVisible},
			    {QStringLiteral("running"), deadlineMs > 0},
			    {QStringLiteral("remaining"), secondsRemaining()},
			    {QStringLiteral("deadline"), deadlineMs}});
}

void OverlayRuntime::dispatch(QJsonObject event)
{
	if (!available() || effectsPaused) {
		return;
	}
	event.insert(QStringLiteral("at"), QDateTime::currentMSecsSinceEpoch());
	broadcast("effect", event);
	emit eventDispatched(QStringLiteral("%1 · %2")
				     .arg(event.value(QStringLiteral("kind")).toString(),
					  event.value(QStringLiteral("type")).toString()));
}

void OverlayRuntime::reaction(const QString &type)
{
	if (type != QStringLiteral("gg") && type != QStringLiteral("boom") &&
	    type != QStringLiteral("low-hp") && type != QStringLiteral("confetti")) {
		return;
	}
	dispatch({{QStringLiteral("kind"), QStringLiteral("reaction")}, {QStringLiteral("type"), type}});
}

void OverlayRuntime::testAlert(const QString &type)
{
	publishAlert(type, QStringLiteral("test"), QStringLiteral("Тестовый зритель"),
		     type == QStringLiteral("donation") ? QStringLiteral("100 ₽") : QString{},
		     QStringLiteral("Проверка XINKX Studio"));
}

void OverlayRuntime::publishAlert(const QString &type, const QString &platform, const QString &name,
				  const QString &amount, const QString &message)
{
	if (type != QStringLiteral("follow") && type != QStringLiteral("sub") &&
	    type != QStringLiteral("donation") && type != QStringLiteral("raid")) return;
	dispatch({{QStringLiteral("kind"), QStringLiteral("alert")}, {QStringLiteral("type"), type},
		  {QStringLiteral("platform"), shortText(platform, 24)}, {QStringLiteral("name"), shortText(name, 80)},
		  {QStringLiteral("amount"), shortText(amount, 60)}, {QStringLiteral("message"), shortText(message)}});
}

void OverlayRuntime::publishChat(const QString &platform, const QString &name, const QString &message)
{
	const QString text = shortText(message.trimmed());
	if (text.isEmpty()) {
		return;
	}
	// Same sanitized event model will accept native provider chat adapters.
	broadcast("chat", {{QStringLiteral("platform"), shortText(platform, 24)},
			    {QStringLiteral("name"), shortText(name, 80)},
			    {QStringLiteral("message"), text}});
	emit eventDispatched(QStringLiteral("chat · %1").arg(shortText(platform, 24)));
	const QString normalized = text.toCaseFolded().trimmed();
	QString reactionType;
	int cooldownMs = 0;
	if (normalized == QStringLiteral("!boom")) {
		reactionType = QStringLiteral("boom");
		cooldownMs = 12000;
	} else if (normalized.contains(QStringLiteral("low hp"))) {
		reactionType = QStringLiteral("low-hp");
		cooldownMs = 30000;
	} else if (normalized == QStringLiteral("!hype")) {
		reactionType = QStringLiteral("confetti");
		cooldownMs = 30000;
	} else {
		const QStringList words = normalized.split(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}_]+")),
								       Qt::SkipEmptyParts);
		if (words.contains(QStringLiteral("gg"))) {
			reactionType = QStringLiteral("gg");
			cooldownMs = 20000;
		}
	}
	if (reactionType.isEmpty()) {
		return;
	}
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (now - cooldowns.value(reactionType) < cooldownMs || now - lastReactionAt < 500) {
		return;
	}
	cooldowns[reactionType] = now;
	lastReactionAt = now;
	reaction(reactionType);
}

void OverlayRuntime::startTimer(int seconds)
{
	remaining = std::clamp(seconds, 1, 3600);
	deadlineMs = QDateTime::currentMSecsSinceEpoch() + remaining * 1000LL;
	timerVisible = true;
	stateChanged();
	emit timerChanged(remaining, true);
}

void OverlayRuntime::pauseTimer()
{
	remaining = secondsRemaining();
	deadlineMs = 0;
	stateChanged();
	emit timerChanged(remaining, false);
}

void OverlayRuntime::resetTimer()
{
	remaining = 0;
	deadlineMs = 0;
	timerVisible = false;
	stateChanged();
	emit timerChanged(0, false);
}

void OverlayRuntime::setPosition(const QString &value)
{
	if (value == QStringLiteral("top-right") || value == QStringLiteral("top-left") ||
	    value == QStringLiteral("center")) {
		position = value;
		stateChanged();
	}
}

void OverlayRuntime::setPaused(bool paused)
{
	effectsPaused = paused;
	stateChanged();
}

bool OverlayRuntime::addToScene(OBSBasic *mainWindow, QString *message)
{
	if (!available()) {
		*message = error();
		return false;
	}
	OBSScene scene = mainWindow->GetCurrentScene();
	if (!scene) {
		*message = QStringLiteral("Выбери сцену OBS");
		return false;
	}
	if (obs_scene_find_source(scene, kSourceName.constData())) {
		*message = QStringLiteral("Слой XINKX уже есть в выбранной сцене");
		return true;
	}
	obs_source_t *source = obs_get_source_by_name(kSourceName.constData());
	if (source && std::strcmp(obs_source_get_id(source), "browser_source") != 0) {
		obs_source_release(source);
		*message = QStringLiteral("Имя слоя XINKX занято другим OBS-источником");
		return false;
	}
	if (!source) {
		obs_video_info video{};
		if (!obs_get_video_info(&video)) {
			*message = QStringLiteral("Видео OBS не инициализировано");
			return false;
		}
		OBSData settings = obs_data_create();
		obs_data_set_string(settings, "url", sourceUrl().toUtf8().constData());
		obs_data_set_int(settings, "width", video.base_width);
		obs_data_set_int(settings, "height", video.base_height);
		obs_data_set_int(settings, "fps", 30);
		obs_data_set_bool(settings, "shutdown", false);
		obs_data_set_bool(settings, "restart_when_active", true);
		source = obs_source_create("browser_source", kSourceName.constData(), settings, nullptr);
		if (!source) {
			*message = QStringLiteral("OBS Browser Source недоступен: проверь obs-browser в сборке");
			return false;
		}
	}
	OBSData before = OBSBasic::BackupScene(scene);
	const auto *item = obs_scene_add(scene, source);
	obs_source_release(source);
	if (!item) {
		*message = QStringLiteral("Не удалось добавить источник в сцену");
		return false;
	}
	OBSData after = OBSBasic::BackupScene(scene);
	mainWindow->CreateSceneUndoRedoAction(QStringLiteral("XINKX: добавить эффекты и события"), before, after);
	*message = QStringLiteral("Слой XINKX добавлен в сцену · Ctrl+Z для отмены");
	return true;
}

void installOverlayWorkspace(OBSBasic *mainWindow, QDockWidget *sourceInspector)
{
	auto *runtime = new OverlayRuntime(mainWindow);
	runtime->setObjectName(QStringLiteral("xinkxOverlayRuntime"));
	auto *dock = new QDockWidget(QStringLiteral("События и эффекты"), mainWindow);
	dock->setObjectName(QStringLiteral("xinkxEffectsDock"));
	auto *content = new QWidget(dock);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(10, 8, 10, 8);
	layout->setSpacing(7);
	auto *status = new QLabel(runtime->available() ? QStringLiteral("Локальный движок · готов") : runtime->error(), content);
	status->setWordWrap(true);
	layout->addWidget(status);
	auto *add = new QPushButton(QStringLiteral("Добавить XINKX-слой в сцену"), content);
	add->setEnabled(runtime->available());
	layout->addWidget(add);
	QObject::connect(add, &QPushButton::clicked, dock, [=] {
		QString notice;
		runtime->addToScene(mainWindow, &notice);
		status->setText(notice);
	});
	auto *position = new QComboBox(content);
	position->addItem(QStringLiteral("Справа сверху"), QStringLiteral("top-right"));
	position->addItem(QStringLiteral("Слева сверху"), QStringLiteral("top-left"));
	position->addItem(QStringLiteral("По центру"), QStringLiteral("center"));
	auto *positionForm = new QFormLayout();
	positionForm->addRow(QStringLiteral("Позиция"), position);
	layout->addLayout(positionForm);
	QObject::connect(position, &QComboBox::currentIndexChanged, dock, [=] {
		runtime->setPosition(position->currentData().toString());
	});
	auto *effects = new QGridLayout();
	int effectIndex = 0;
	for (const auto &action : {QStringLiteral("GG"), QStringLiteral("BOOM"), QStringLiteral("LOW HP"),
			    QStringLiteral("Конфетти")}) {
		auto *button = new QPushButton(action, content);
		effects->addWidget(button, effectIndex / 2, effectIndex % 2);
		++effectIndex;
		QObject::connect(button, &QPushButton::clicked, dock, [=] {
			const QString type = action == QStringLiteral("GG") ? QStringLiteral("gg")
						: action == QStringLiteral("BOOM") ? QStringLiteral("boom")
						: action == QStringLiteral("LOW HP") ? QStringLiteral("low-hp")
										      : QStringLiteral("confetti");
			runtime->reaction(type);
		});
	}
	layout->addLayout(effects);
	auto *alerts = new QGridLayout();
	int alertIndex = 0;
	for (const auto &entry : {QStringLiteral("Фолловер"), QStringLiteral("Подписка"),
				 QStringLiteral("Донат"), QStringLiteral("Рейд")}) {
		auto *button = new QPushButton(entry, content);
		alerts->addWidget(button, alertIndex / 2, alertIndex % 2);
		++alertIndex;
		QObject::connect(button, &QPushButton::clicked, dock, [=] {
			const QString type = entry == QStringLiteral("Фолловер") ? QStringLiteral("follow")
						: entry == QStringLiteral("Подписка") ? QStringLiteral("sub")
						: entry == QStringLiteral("Донат") ? QStringLiteral("donation")
											     : QStringLiteral("raid");
			runtime->testAlert(type);
		});
	}
	layout->addLayout(alerts);
	auto *timerRow = new QGridLayout();
	auto *duration = new QSpinBox(content);
	duration->setRange(5, 3600);
	duration->setValue(180);
	duration->setSuffix(QStringLiteral(" с"));
	timerRow->addWidget(duration, 0, 0);
	auto *start = new QPushButton(QStringLiteral("Старт таймера"), content);
	auto *pause = new QPushButton(QStringLiteral("Пауза"), content);
	auto *reset = new QPushButton(QStringLiteral("Скрыть"), content);
	timerRow->addWidget(start, 0, 1);
	timerRow->addWidget(pause, 1, 0);
	timerRow->addWidget(reset, 1, 1);
	layout->addLayout(timerRow);
	QObject::connect(start, &QPushButton::clicked, dock, [=] { runtime->startTimer(duration->value()); });
	QObject::connect(pause, &QPushButton::clicked, dock, [=] { runtime->pauseTimer(); });
	QObject::connect(reset, &QPushButton::clicked, dock, [=] { runtime->resetTimer(); });
	auto *clock = new QLabel(QStringLiteral("Таймер скрыт"), content);
	layout->addWidget(clock);
	QObject::connect(runtime, &OverlayRuntime::timerChanged, dock, [=](int seconds, bool running) {
		clock->setText(seconds ? QStringLiteral("%1:%2 · %3")
						.arg(seconds / 60, 2, 10, QLatin1Char('0'))
						.arg(seconds % 60, 2, 10, QLatin1Char('0'))
						.arg(running ? QStringLiteral("идёт") : QStringLiteral("пауза"))
				 : QStringLiteral("Таймер скрыт"));
	});
	auto *chatRow = new QHBoxLayout();
	auto *testChat = new QLineEdit(content);
	testChat->setPlaceholderText(QStringLiteral("Проверить реакцию: gg, !boom, low hp, !hype"));
	testChat->setMaxLength(200);
	chatRow->addWidget(testChat);
	auto *sendTest = new QPushButton(QStringLiteral("Тест чата"), content);
	chatRow->addWidget(sendTest);
	layout->addLayout(chatRow);
	QObject::connect(sendTest, &QPushButton::clicked, dock, [=] {
		runtime->publishChat(QStringLiteral("test"), QStringLiteral("Тестовый зритель"), testChat->text());
	});
	auto *paused = new QCheckBox(QStringLiteral("Отключить эффекты (аварийно)"), content);
	layout->addWidget(paused);
	QObject::connect(paused, &QCheckBox::toggled, dock, [=](bool value) { runtime->setPaused(value); });
	auto *history = new QListWidget(content);
	history->setMaximumHeight(100);
	layout->addWidget(history);
	QObject::connect(runtime, &OverlayRuntime::eventDispatched, dock, [=](const QString &label) {
		history->insertItem(0, QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")) +
						  QStringLiteral(" · ") + label);
		while (history->count() > 20) {
			delete history->takeItem(history->count() - 1);
		}
	});
	dock->setWidget(content);
	mainWindow->AddDockWidget(dock, Qt::RightDockWidgetArea);
	// Qt window geometry is OBS-owned. XINKX only stores its own dock visibility and options.
	char path[512];
	const int length = GetAppConfigPath(path, sizeof(path), "xinkx-studio/studio.ini");
	bool visible = true;
	if (length > 0 && static_cast<size_t>(length) < sizeof(path)) {
		QDir().mkpath(QFileInfo(QString::fromUtf8(path)).absolutePath());
		auto *settings = new QSettings(QString::fromUtf8(path), QSettings::IniFormat, dock);
		visible = settings->value(QStringLiteral("workspace/effectsVisible"), true).toBool();
		duration->setValue(settings->value(QStringLiteral("effects/timerSeconds"), 180).toInt());
		const int index = position->findData(settings->value(QStringLiteral("effects/position"),
									    QStringLiteral("top-right")));
		if (index >= 0) {
			position->setCurrentIndex(index);
		}
		QObject::connect(dock, &QDockWidget::visibilityChanged, dock, [settings](bool value) {
			settings->setValue(QStringLiteral("workspace/effectsVisible"), value);
		});
		QObject::connect(duration, &QSpinBox::valueChanged, dock, [settings](int value) {
			settings->setValue(QStringLiteral("effects/timerSeconds"), value);
		});
		QObject::connect(position, &QComboBox::currentIndexChanged, dock, [settings, position] {
			settings->setValue(QStringLiteral("effects/position"), position->currentData());
		});
	}
	QTimer::singleShot(0, dock, [mainWindow, sourceInspector, dock, visible] {
		dock->setFloating(false);
		mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
		mainWindow->tabifyDockWidget(sourceInspector, dock);
		dock->setVisible(visible);
		sourceInspector->raise();
	});
}

} // namespace xinkx
