#include "OverlayRuntime.hpp"
#include "PresentationSettings.hpp"
#include "StudioWorkspace.hpp"
#include "StudioAutomation.hpp"

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
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QHostAddress>
#include <QTcpSocket>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <utility>

namespace xinkx {

namespace {

constexpr quint16 kPort = 8766;
QString layerName(const QString &layer)
{
	if (layer == QStringLiteral("chat")) return QStringLiteral("XINKX · Чат");
	if (layer == QStringLiteral("effects")) return QStringLiteral("XINKX · Реакции");
	if (layer == QStringLiteral("alerts")) return QStringLiteral("XINKX · Донаты и события");
	if (layer == QStringLiteral("timer")) return QStringLiteral("XINKX · Таймер");
	if (layer == QStringLiteral("viewers")) return QStringLiteral("XINKX · Зрители");
	if (layer == QStringLiteral("frame")) return QStringLiteral("XINKX · Рамка камеры");
	return QStringLiteral("XINKX · Эффекты и события");
}

bool validLayer(const QString &layer)
{
	return layer == QStringLiteral("all") || layer == QStringLiteral("chat") ||
	       layer == QStringLiteral("effects") || layer == QStringLiteral("alerts") ||
	       layer == QStringLiteral("timer") || layer == QStringLiteral("viewers") ||
	       layer == QStringLiteral("frame");
}

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
	presentation = PresentationSettings::defaults();
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

QString OverlayRuntime::sourceUrl(const QString &layer) const
{
	return QStringLiteral("http://127.0.0.1:8766/overlay?layer=%1").arg(validLayer(layer) ? layer : QStringLiteral("all"));
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
		QJsonObject currentViewers;
		for (auto it = viewers.cbegin(); it != viewers.cend(); ++it) currentViewers.insert(it.key(), it.value());
		sendEvent(socket, "state", {{QStringLiteral("position"), position},
						    {QStringLiteral("paused"), effectsPaused},
						    {QStringLiteral("visible"), timerVisible},
						    {QStringLiteral("running"), deadlineMs > 0},
						    {QStringLiteral("remaining"), secondsRemaining()},
						    {QStringLiteral("deadline"), deadlineMs},
						    {QStringLiteral("presentation"), presentation},
						    {QStringLiteral("viewers"), currentViewers}});
		return;
	}
	const QList<QByteArray> requestParts = requestLine.split(' ');
	const QUrl requested = requestParts.size() == 3 ? QUrl::fromEncoded(requestParts.at(1)) : QUrl();
	const QUrlQuery query(requested);
	if (requestParts.size() == 3 && requestParts.at(0) == "GET" &&
	    requested.path() == QStringLiteral("/overlay") &&
	    validLayer(query.queryItemValue(QStringLiteral("layer")).isEmpty()
		? QStringLiteral("all") : query.queryItemValue(QStringLiteral("layer")))) {
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
	QJsonObject currentViewers;
	for (auto it = viewers.cbegin(); it != viewers.cend(); ++it) currentViewers.insert(it.key(), it.value());
	broadcast("state", {{QStringLiteral("position"), position},
			    {QStringLiteral("paused"), effectsPaused},
			    {QStringLiteral("visible"), timerVisible},
			    {QStringLiteral("running"), deadlineMs > 0},
			    {QStringLiteral("remaining"), secondsRemaining()},
			    {QStringLiteral("deadline"), deadlineMs},
			    {QStringLiteral("presentation"), presentation},
			    {QStringLiteral("viewers"), currentViewers}});
}

void OverlayRuntime::setPresentation(const QJsonObject &value)
{
	if (!PresentationSettings::validate(value)) return;
	presentation = value;
	cooldowns.clear();
	lastReactionAt = 0;
	stateChanged();
}

void OverlayRuntime::publishViewers(const QString &platform, int count)
{
	if (count < 0) viewers.remove(platform);
	else viewers.insert(platform.left(24), count);
	stateChanged();
}

void OverlayRuntime::dispatch(QJsonObject event)
{
	if (!available() || effectsPaused) {
		return;
	}
	event.insert(QStringLiteral("at"), QDateTime::currentMSecsSinceEpoch());
	broadcast("effect", event);
	emit effectGenerated(event);
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

void OverlayRuntime::clearEffects()
{
	broadcast("clear", {});
	emit effectsCleared();
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
	const auto words = normalized.split(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}_]+")),
						 Qt::SkipEmptyParts);
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (const auto &entry : presentation.value(QStringLiteral("reactions")).toArray()) {
		const auto item = entry.toObject();
		if (!item.value(QStringLiteral("enabled")).toBool()) continue;
		const auto phrase = item.value(QStringLiteral("phrase")).toString().toCaseFolded().trimmed();
		const auto mode = item.value(QStringLiteral("match")).toString();
		const bool match = mode == QStringLiteral("exact") ? normalized == phrase
			: mode == QStringLiteral("contains") ? normalized.contains(phrase) : words.contains(phrase);
		if (!match) continue;
		const auto key = item.value(QStringLiteral("id")).toString() +
			(item.value(QStringLiteral("perViewer")).toBool() ? QStringLiteral("/") +
			 platform.toCaseFolded() + QStringLiteral("/") + name.toCaseFolded() : QString{});
		const int cooldown = item.value(QStringLiteral("cooldownMs")).toInt();
		const int globalCooldown = presentation.value(QStringLiteral("globalCooldownMs")).toInt();
		if ((cooldown > 0 && cooldowns.contains(key) && now - cooldowns.value(key) < cooldown) ||
		    (globalCooldown > 0 && lastReactionAt > 0 && now - lastReactionAt < globalCooldown)) break;
		cooldowns[key] = now;
		lastReactionAt = now;
		dispatch({{QStringLiteral("kind"), QStringLiteral("reaction")},
			{QStringLiteral("type"), item.value(QStringLiteral("effect")).toString()},
			{QStringLiteral("durationMs"), item.value(QStringLiteral("durationMs")).toInt()},
			{QStringLiteral("viewer"), shortText(name, 80)},
			{QStringLiteral("platform"), shortText(platform, 24)}});
		break;
	}
	if (cooldowns.size() > 2048) cooldowns.clear();
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

bool OverlayRuntime::addToScene(OBSBasic *mainWindow, QString *message, const QString &layer)
{
	if (!validLayer(layer)) {
		*message = QStringLiteral("Неизвестный тип элемента XINKX");
		return false;
	}
	if (!available()) {
		*message = error();
		return false;
	}
	OBSScene scene = mainWindow->GetCurrentScene();
	if (!scene) {
		*message = QStringLiteral("Выбери сцену OBS");
		return false;
	}
	const QByteArray sourceName = layerName(layer).toUtf8();
	if (auto *existing = obs_scene_find_source(scene, sourceName.constData())) {
		obs_sceneitem_select(existing, true);
		*message = QStringLiteral("Слой XINKX выделен в предпросмотре OBS. Перемещай и меняй размер мышью");
		return true;
	}
	obs_source_t *source = obs_get_source_by_name(sourceName.constData());
	const QByteArray nativeId = layer == QStringLiteral("chat") ? "xinkx_chat" :
		layer == QStringLiteral("effects") ? "xinkx_reactions" :
		layer == QStringLiteral("alerts") ? "xinkx_alerts" :
		layer == QStringLiteral("timer") ? "xinkx_timer" :
		layer == QStringLiteral("viewers") ? "xinkx_viewers" :
		layer == QStringLiteral("frame") ? "xinkx_frame" : "browser_source";
	if (source && std::strcmp(obs_source_get_id(source), nativeId.constData()) != 0) {
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
		if (layer == QStringLiteral("all"))
			obs_data_set_string(settings, "url", sourceUrl(layer).toUtf8().constData());
		const int width = layer == QStringLiteral("chat") ? 720 :
			layer == QStringLiteral("timer") ? 480 :
			layer == QStringLiteral("alerts") ? 960 :
			layer == QStringLiteral("viewers") ? 520 :
			layer == QStringLiteral("frame") ? 800 : static_cast<int>(video.base_width);
		const int height = layer == QStringLiteral("chat") ? 560 :
			layer == QStringLiteral("timer") ? 180 :
			layer == QStringLiteral("alerts") ? 440 :
			layer == QStringLiteral("viewers") ? 100 :
			layer == QStringLiteral("frame") ? 450 : static_cast<int>(video.base_height);
		obs_data_set_int(settings, "width", width);
		obs_data_set_int(settings, "height", height);
		obs_data_set_int(settings, "fps", 30);
		obs_data_set_bool(settings, "shutdown", false);
		obs_data_set_bool(settings, "restart_when_active", true);
		source = obs_source_create(nativeId.constData(), sourceName.constData(), settings, nullptr);
		obs_data_release(settings);
		if (!source) {
			*message = QStringLiteral("OBS Browser Source недоступен: проверь obs-browser в сборке");
			return false;
		}
	}
	OBSData before = OBSBasic::BackupScene(scene);
	auto *item = obs_scene_add(scene, source);
	obs_source_release(source);
	if (!item) {
		*message = QStringLiteral("Не удалось добавить источник в сцену");
		return false;
	}
	obs_sceneitem_select(item, true);
	OBSData after = OBSBasic::BackupScene(scene);
	mainWindow->CreateSceneUndoRedoAction(QStringLiteral("XINKX: добавить ") + layerName(layer), before, after);
	*message = QStringLiteral("Слой XINKX добавлен и выбран: его размер и позиция меняются в предпросмотре OBS · Ctrl+Z для отмены");
	return true;
}

void installOverlayWorkspace(OBSBasic *mainWindow, QDockWidget *sourceInspector)
{
	Q_UNUSED(sourceInspector);
	auto *runtime = new OverlayRuntime(mainWindow);
	runtime->setObjectName(QStringLiteral("xinkxOverlayRuntime"));
	auto *dock = studioDock(mainWindow);
	if (!dock) return;
	auto *content = new QWidget(dock);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(10, 8, 10, 8);
	layout->setSpacing(7);
	auto *status = new QLabel(runtime->available() ? QStringLiteral("Локальный движок · готов") : runtime->error(), content);
	status->setWordWrap(true);
	layout->addWidget(status);
	auto *sourceHint = new QLabel(QStringLiteral("Источники XINKX добавляются штатной кнопкой «+» в OBS. "
		"Двойной щелчок по источнику открывает его свойства; размеры и положение меняются в предпросмотре."), content);
	sourceHint->setWordWrap(true);
	layout->addWidget(sourceHint);
	auto *testGroup = new QGroupBox(QStringLiteral("Реакции и тест событий"), content);
	auto *testLayout = new QVBoxLayout(testGroup);
	auto *position = new QComboBox(content);
	position->addItem(QStringLiteral("Справа сверху"), QStringLiteral("top-right"));
	position->addItem(QStringLiteral("Слева сверху"), QStringLiteral("top-left"));
	position->addItem(QStringLiteral("По центру"), QStringLiteral("center"));
	auto *positionForm = new QFormLayout();
	positionForm->addRow(QStringLiteral("Позиция"), position);
	testLayout->addLayout(positionForm);
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
	testLayout->addLayout(effects);
	auto *clear = new QPushButton(QStringLiteral("Очистить все эффекты"), content);
	testLayout->addWidget(clear);
	QObject::connect(clear, &QPushButton::clicked, dock, [runtime] { runtime->clearEffects(); });
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
	testLayout->addLayout(alerts);
	layout->addWidget(testGroup);
	auto *timerGroup = new QGroupBox(QStringLiteral("Таймер"), content);
	auto *timerLayout = new QVBoxLayout(timerGroup);
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
	timerLayout->addLayout(timerRow);
	QObject::connect(start, &QPushButton::clicked, dock, [=] { runtime->startTimer(duration->value()); });
	QObject::connect(pause, &QPushButton::clicked, dock, [=] { runtime->pauseTimer(); });
	QObject::connect(reset, &QPushButton::clicked, dock, [=] { runtime->resetTimer(); });
	auto *clock = new QLabel(QStringLiteral("Таймер скрыт"), content);
	timerLayout->addWidget(clock);
	layout->addWidget(timerGroup);
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
	layout->addStretch(1);
	studioTabs(mainWindow)->addTab(content, QStringLiteral("Эффекты"));
	// Qt window geometry is OBS-owned. XINKX only stores its own dock visibility and options.
	char path[512];
	const int length = GetAppConfigPath(path, sizeof(path), "xinkx-studio/studio.ini");
	if (length > 0 && static_cast<size_t>(length) < sizeof(path)) {
		QDir().mkpath(QFileInfo(QString::fromUtf8(path)).absolutePath());
		auto *settings = new QSettings(QString::fromUtf8(path), QSettings::IniFormat, dock);
		duration->setValue(settings->value(QStringLiteral("effects/timerSeconds"), 180).toInt());
		const int index = position->findData(settings->value(QStringLiteral("effects/position"),
									    QStringLiteral("top-right")));
		if (index >= 0) {
			position->setCurrentIndex(index);
		}
		QObject::connect(duration, &QSpinBox::valueChanged, dock, [settings](int value) {
			settings->setValue(QStringLiteral("effects/timerSeconds"), value);
		});
		QObject::connect(position, &QComboBox::currentIndexChanged, dock, [settings, position] {
			settings->setValue(QStringLiteral("effects/position"), position->currentData());
		});
	}
}

} // namespace xinkx
