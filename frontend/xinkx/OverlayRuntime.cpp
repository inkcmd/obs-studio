#include "OverlayRuntime.hpp"
#include "StudioDiagnostics.hpp"
#include "PresentationSettings.hpp"
#include "StudioWorkspace.hpp"
#include "StudioAutomation.hpp"
#include "ChatGateway.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QCheckBox>
#include <QColor>
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
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QDialog>
#include <QColorDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QIcon>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QSize>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QHostAddress>
#include <QTcpSocket>
#include <QVBoxLayout>
#include <QDialogButtonBox>

#include <algorithm>
#include <cstring>
#include <memory>
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

struct AlertSourcePresence {
	bool visible = false;
	bool hidden = false;
};

bool inspectAlertSource(obs_scene_t *, obs_sceneitem_t *item, void *context)
{
	auto *presence = static_cast<AlertSourcePresence *>(context);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source) return true;
	if (obs_sceneitem_is_group(item)) {
		if (auto *group = obs_group_from_source(source)) {
			AlertSourcePresence nested;
			obs_scene_enum_items(group, inspectAlertSource, &nested);
			if (obs_sceneitem_visible(item)) {
				presence->visible |= nested.visible;
				presence->hidden |= nested.hidden;
			} else presence->hidden |= nested.visible || nested.hidden;
		}
	}
	const char *id = obs_source_get_id(source);
	bool isAlert = id && std::strcmp(id, "xinkx_alerts") == 0;
	if (!isAlert && id && std::strcmp(id, "browser_source") == 0) {
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		const QString url = QString::fromUtf8(obs_data_get_string(settings, "url"));
		isAlert = url == QStringLiteral("http://127.0.0.1:8766/overlay?layer=alerts") ||
			url == QStringLiteral("http://127.0.0.1:8766/overlay?layer=all");
	}
	if (isAlert) {
		if (obs_sceneitem_visible(item)) presence->visible = true;
		else presence->hidden = true;
	}
	return true;
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

struct SourcePreset {
	QString title;
	QString description;
	QString layer;
	QString appearance;
	QString reaction;
	QJsonObject settings;
};

QString catalogPath()
{
	char path[512];
	const int length = GetAppConfigPath(path, sizeof(path), "xinkx-studio/studio.ini");
	return length > 0 && static_cast<size_t>(length) < sizeof(path) ? QString::fromUtf8(path) : QString{};
}

QList<SourcePreset> catalogPresets()
{
	QList<SourcePreset> items;
	const auto add = [&items](const QString &title, const QString &layer,
			const QString &appearance, const QString &reaction, const QString &description) {
		QJsonObject settings{{QStringLiteral("preset"), appearance},
			{QStringLiteral("reaction"), reaction}, {QStringLiteral("customize"), appearance != QStringLiteral("default")}};
		if (layer == QStringLiteral("chat")) {
			settings.insert(QStringLiteral("customize"), true);
			settings.insert(QStringLiteral("messageColor"), static_cast<qint64>(0xdc2b1508u));
			settings.insert(QStringLiteral("panelColor"), static_cast<qint64>(0x002c1509u));
			if (appearance == QStringLiteral("glass"))
				settings.insert(QStringLiteral("messageColor"), static_cast<qint64>(0x882c1509u));
			if (appearance == QStringLiteral("minimal"))
				settings.insert(QStringLiteral("messageColor"), static_cast<qint64>(0x002c1509u));
		}
		items.append({title, description, layer, appearance, reaction, settings});
	};
	add(QStringLiteral("Чат · классика"), QStringLiteral("chat"), QStringLiteral("default"),
		QStringLiteral("all"), QStringLiteral("Сообщения на всю ширину области. Фон каждого сообщения настраивается."));
	add(QStringLiteral("Чат · компактный"), QStringLiteral("chat"), QStringLiteral("compact"),
		QStringLiteral("all"), QStringLiteral("Плотные сообщения, минимальные рамки."));
	add(QStringLiteral("Чат · стекло"), QStringLiteral("chat"), QStringLiteral("glass"),
		QStringLiteral("all"), QStringLiteral("Полупрозрачные карточки с размытием."));
	add(QStringLiteral("Чат · неон"), QStringLiteral("chat"), QStringLiteral("neon"),
		QStringLiteral("all"), QStringLiteral("Подсвеченная рамка у сообщений."));
	add(QStringLiteral("Чат · без подложки"), QStringLiteral("chat"), QStringLiteral("minimal"),
		QStringLiteral("all"), QStringLiteral("Только текст, без фона сообщений."));
	add(QStringLiteral("GG · частицы"), QStringLiteral("effects"), QStringLiteral("neon"),
		QStringLiteral("gg"), QStringLiteral("Летящие GG при событии GG."));
	add(QStringLiteral("BOOM · удар"), QStringLiteral("effects"), QStringLiteral("default"),
		QStringLiteral("boom"), QStringLiteral("Большая ударная надпись."));
	add(QStringLiteral("LOW HP · тревога"), QStringLiteral("effects"), QStringLiteral("default"),
		QStringLiteral("low-hp"), QStringLiteral("Красное затемнение по краям."));
	add(QStringLiteral("Конфетти"), QStringLiteral("effects"), QStringLiteral("default"),
		QStringLiteral("confetti"), QStringLiteral("Падающие цветные частицы."));
	add(QStringLiteral("Все реакции"), QStringLiteral("effects"), QStringLiteral("default"),
		QStringLiteral("all"), QStringLiteral("Все разрешённые реакции в одном слое."));
	add(QStringLiteral("Своя реакция · текст"), QStringLiteral("effects"), QStringLiteral("default"),
		QStringLiteral("custom"), QStringLiteral("Создай свою реакцию с отдельным ID и надписью. "
		"После добавления назначь ей фразу в настройках «Реакции и боты»."));
	add(QStringLiteral("Камера · голубая рамка"), QStringLiteral("frame"), QStringLiteral("default"),
		QStringLiteral("all"), QStringLiteral("Тонкая подсвеченная рамка поверх камеры."));
	add(QStringLiteral("Камера · неон"), QStringLiteral("frame"), QStringLiteral("neon"),
		QStringLiteral("all"), QStringLiteral("Фиолетовое свечение."));
	add(QStringLiteral("Камера · золото"), QStringLiteral("frame"), QStringLiteral("gold"),
		QStringLiteral("all"), QStringLiteral("Тёплый золотой контур."));
	add(QStringLiteral("Камера · минимализм"), QStringLiteral("frame"), QStringLiteral("minimal"),
		QStringLiteral("all"), QStringLiteral("Прямые тонкие линии без свечения."));
	add(QStringLiteral("Камера · скруглённая"), QStringLiteral("frame"), QStringLiteral("round"),
		QStringLiteral("all"), QStringLiteral("Широкие скруглённые углы."));
	add(QStringLiteral("Оповещения и донаты"), QStringLiteral("alerts"), QStringLiteral("default"),
		QStringLiteral("all"), QStringLiteral("Подписки, донаты, фолловеры и рейды."));
	add(QStringLiteral("Таймер"), QStringLiteral("timer"), QStringLiteral("default"),
		QStringLiteral("all"), QStringLiteral("Обратный отсчёт."));
	add(QStringLiteral("Счётчик зрителей"), QStringLiteral("viewers"), QStringLiteral("default"),
		QStringLiteral("all"), QStringLiteral("Количество зрителей Twitch, YouTube и Kick."));
	const QString path = catalogPath();
	if (!path.isEmpty()) {
		QSettings store(path, QSettings::IniFormat);
		const QByteArray saved = store.value(QStringLiteral("catalog/presets")).toByteArray();
		for (const auto &entry : QJsonDocument::fromJson(saved).array()) {
			const auto object = entry.toObject();
			const QString layer = object.value(QStringLiteral("layer")).toString();
			if (!validLayer(layer) || layer == QStringLiteral("all")) continue;
			items.append({object.value(QStringLiteral("name")).toString().left(80),
				QStringLiteral("Мой пресет · настройки сохранены локально"), layer,
				object.value(QStringLiteral("settings")).toObject().value(QStringLiteral("preset")).toString(),
				object.value(QStringLiteral("settings")).toObject().value(QStringLiteral("reaction")).toString(),
				object.value(QStringLiteral("settings")).toObject()});
		}
	}
	return items;
}

QPixmap catalogPreview(const SourcePreset &item)
{
	QPixmap image(640, 360);
	image.fill(QColor(QStringLiteral("#172338")));
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	QLinearGradient gradient(0, 0, 640, 360);
	gradient.setColorAt(0, QColor(QStringLiteral("#1d3153")));
	gradient.setColorAt(1, QColor(QStringLiteral("#121828")));
	painter.fillRect(image.rect(), gradient);
	painter.setPen(QColor(QStringLiteral("#5b6b87")));
	painter.drawText(QRect(20, 14, 600, 25), Qt::AlignLeft, QStringLiteral("МАКЕТ ОФОРМЛЕНИЯ · 16:9"));
	const QColor accent = item.appearance == QStringLiteral("gold") ? QColor(QStringLiteral("#f8d17a")) :
		item.appearance == QStringLiteral("neon") ? QColor(QStringLiteral("#bf8bff")) : QColor(QStringLiteral("#69dffa"));
	if (item.layer == QStringLiteral("chat")) {
		const auto color = [&](const QString &key, uint32_t fallback) {
			const uint32_t value = static_cast<uint32_t>(item.settings.value(key).toDouble(fallback));
			return QColor(value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >> 24) & 0xff);
		};
		const QColor panel = color(QStringLiteral("panelColor"), 0x002c1509u);
		const QColor bubble = color(QStringLiteral("messageColor"), 0xdc2b1508u);
		painter.setPen(Qt::NoPen);
		painter.setBrush(panel);
		painter.drawRoundedRect(QRect(27, 113, 463, 217), 13, 13);
		for (int i = 0; i < 3; ++i) {
			const QRect box(35, 124 + i * 66, 445, 54);
			painter.setPen(QPen(accent, item.appearance == QStringLiteral("neon") ? 5 : 2));
			painter.setBrush(bubble);
			painter.drawRoundedRect(box, item.appearance == QStringLiteral("compact") ? 1 : 10,
				item.appearance == QStringLiteral("compact") ? 1 : 10);
			painter.setPen(accent);
			painter.drawText(box.adjusted(12, 5, -8, -25), Qt::AlignVCenter | Qt::AlignLeft,
				QStringLiteral("Зритель %1").arg(i + 1));
			painter.setPen(Qt::white);
			painter.drawText(box.adjusted(12, 26, -8, -5), Qt::AlignVCenter | Qt::AlignLeft,
				QStringLiteral("Всем привет! Хорошего стрима ✦"));
		}
	} else if (item.layer == QStringLiteral("frame")) {
		painter.setPen(QPen(accent, item.appearance == QStringLiteral("minimal") ? 3 : 9));
		painter.setBrush(Qt::NoBrush);
		painter.drawRoundedRect(QRect(100, 60, 440, 260), item.appearance == QStringLiteral("round") ? 65 : 20,
			item.appearance == QStringLiteral("round") ? 65 : 20);
		painter.setPen(QColor(QStringLiteral("#9baecc")));
		painter.drawText(QRect(100, 150, 440, 40), Qt::AlignCenter, QStringLiteral("КАМЕРА"));
	} else if (item.layer == QStringLiteral("timer")) {
		painter.setPen(accent);
		QFont font = painter.font(); font.setPointSize(66); font.setBold(true); painter.setFont(font);
		painter.drawText(QRect(40, 65, 560, 210), Qt::AlignCenter, QStringLiteral("04:59"));
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(QStringLiteral("#354760")));
		painter.drawRoundedRect(QRect(110, 289, 420, 10), 5, 5);
		painter.setBrush(accent);
		painter.drawRoundedRect(QRect(110, 289, 315, 10), 5, 5);
	} else if (item.layer == QStringLiteral("alerts")) {
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(QStringLiteral("#1d2943")));
		painter.drawRoundedRect(QRect(80, 105, 480, 175), 18, 18);
		painter.setPen(accent);
		QFont heading = painter.font(); heading.setPointSize(28); heading.setBold(true); painter.setFont(heading);
		painter.drawText(QRect(95, 119, 450, 65), Qt::AlignCenter, QStringLiteral("✦ ДОНАТ · 500 ₽ ✦"));
		painter.setPen(Qt::white);
		QFont message = painter.font(); message.setPointSize(18); message.setBold(false); painter.setFont(message);
		painter.drawText(QRect(100, 187, 440, 70), Qt::AlignCenter | Qt::TextWordWrap,
			QStringLiteral("Зритель42: Удачного стрима!"));
	} else if (item.layer == QStringLiteral("viewers")) {
		painter.setPen(accent);
		QFont font = painter.font(); font.setPointSize(32); font.setBold(true); painter.setFont(font);
		painter.drawText(QRect(35, 90, 570, 180), Qt::AlignCenter, QStringLiteral("● 128 зрителей"));
	} else {
		painter.setPen(accent);
		QFont font = painter.font(); font.setPointSize(38); font.setBold(true); painter.setFont(font);
		const QString text = !item.settings.value(QStringLiteral("customText")).toString().isEmpty()
			? item.settings.value(QStringLiteral("customText")).toString() : item.layer == QStringLiteral("effects") ?
			(item.reaction == QStringLiteral("gg") ? QStringLiteral("GG ✦") :
			item.reaction == QStringLiteral("boom") ? QStringLiteral("BOOM!") :
			item.reaction == QStringLiteral("low-hp") ? QStringLiteral("LOW HP") :
			item.reaction == QStringLiteral("confetti") ? QStringLiteral("✦ ✧ ✦") : QStringLiteral("GG  BOOM  ✦")) : item.title;
		painter.drawText(QRect(30, 70, 580, 230), Qt::AlignCenter | Qt::TextWordWrap, text);
	}
	return image;
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
	    type != QStringLiteral("low-hp") && type != QStringLiteral("confetti") &&
	    !QRegularExpression(QStringLiteral("^custom-[a-z0-9]{8}$")).match(type).hasMatch()) {
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

QString OverlayRuntime::alertSourceHealth(OBSBasic *mainWindow) const
{
	if (!available()) return error();
	if (effectsPaused) return QStringLiteral("Оповещения отключены аварийной паузой эффектов");
	if (!mainWindow) return QStringLiteral("Нет окна OBS для проверки оповещений");
	OBSScene scene = mainWindow->GetCurrentScene();
	if (!scene) return QStringLiteral("Выбери сцену OBS для проверки оповещений");
	AlertSourcePresence presence;
	obs_scene_enum_items(scene, inspectAlertSource, &presence);
	if (presence.visible) return QStringLiteral("Готово: источник оповещений включён в текущей сцене");
	if (presence.hidden) return QStringLiteral("Источник оповещений скрыт в текущей сцене OBS");
	return QStringLiteral("Источник оповещений отсутствует в текущей сцене OBS");
}

bool OverlayRuntime::addToScene(OBSBasic *mainWindow, QString *message, const QString &layer,
				const QString &requestedName, const QJsonObject &presetSettings, const QString &placement)
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
	const QByteArray sourceName = (requestedName.isEmpty() ? layerName(layer) : requestedName).toUtf8();
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
		if (!presetSettings.isEmpty()) {
			const QByteArray json = QJsonDocument(presetSettings).toJson(QJsonDocument::Compact);
			OBSDataAutoRelease selected = obs_data_create_from_json(json.constData());
			if (selected) obs_data_apply(settings, selected);
		}
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
	if (!placement.isEmpty()) {
		obs_video_info video{};
		if (obs_get_video_info(&video)) {
			const double w = std::max(1u, obs_source_get_width(obs_sceneitem_get_source(item)));
			const double h = std::max(1u, obs_source_get_height(obs_sceneitem_get_source(item)));
			const double factor = std::min({1.0, (video.base_width - 32.0) / w,
				(video.base_height - 32.0) / h});
			vec2 scale = {static_cast<float>(factor), static_cast<float>(factor)};
			obs_sceneitem_set_scale(item, &scale);
			const double actualW = w * factor, actualH = h * factor;
			const double left = placement.contains(QStringLiteral("right"))
				? video.base_width - actualW - 16.0 : placement == QStringLiteral("center")
				? (video.base_width - actualW) / 2.0 : 16.0;
			const double top = placement.contains(QStringLiteral("bottom"))
				? video.base_height - actualH - 16.0 : placement == QStringLiteral("center")
				? (video.base_height - actualH) / 2.0 : 16.0;
			vec2 position = {static_cast<float>(std::max(0.0, left)),
				static_cast<float>(std::max(0.0, top))};
			obs_sceneitem_set_pos(item, &position);
		}
	}
	OBSData after = OBSBasic::BackupScene(scene);
	mainWindow->CreateSceneUndoRedoAction(QStringLiteral("XINKX: добавить ") + layerName(layer), before, after);
	*message = QStringLiteral("Слой XINKX добавлен и выбран: его размер и позиция меняются в предпросмотре OBS · Ctrl+Z для отмены");
	return true;
}

void openSourceCatalog(OBSBasic *mainWindow, OverlayRuntime *runtime, QWidget *parent)
{
	// A non-modal library: selecting an OBS source and changing the scene must
	// remain possible while browsing presets.
	if (auto *existing = mainWindow->findChild<QDialog *>(QStringLiteral("xinkxPresetCatalog"))) {
		existing->show();
		existing->raise();
		existing->activateWindow();
		return;
	}
	auto *dialog = new QDialog(mainWindow);
	dialog->setObjectName(QStringLiteral("xinkxPresetCatalog"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(QStringLiteral("XINKX · Библиотека источников"));
	dialog->resize(940, 600);
	dialog->setWindowModality(Qt::NonModal);
	auto *page = new QHBoxLayout(dialog);
	auto *items = new QListWidget(dialog);
	items->setIconSize(QSize(128, 72));
	items->setMinimumWidth(345);
	page->addWidget(items, 1);
	auto *detail = new QVBoxLayout();
	auto *preview = new QLabel(dialog);
	preview->setMinimumSize(430, 242);
	preview->setAlignment(Qt::AlignCenter);
	detail->addWidget(preview);
	auto *description = new QLabel(dialog);
	description->setWordWrap(true);
	detail->addWidget(description);
	auto *form = new QFormLayout();
	auto *name = new QLineEdit(dialog);
	name->setMaxLength(100);
	form->addRow(QStringLiteral("Название источника"), name);
	auto *width = new QSpinBox(dialog);
	width->setRange(80, 3840);
	width->setSuffix(QStringLiteral(" px"));
	auto *height = new QSpinBox(dialog);
	height->setRange(50, 2160);
	height->setSuffix(QStringLiteral(" px"));
	form->addRow(QStringLiteral("Ширина области"), width);
	form->addRow(QStringLiteral("Высота области"), height);
	auto *placement = new QComboBox(dialog);
	placement->addItem(QStringLiteral("Слева сверху"), QStringLiteral("top-left"));
	placement->addItem(QStringLiteral("Справа сверху"), QStringLiteral("top-right"));
	placement->addItem(QStringLiteral("По центру"), QStringLiteral("center"));
	placement->addItem(QStringLiteral("Слева снизу"), QStringLiteral("bottom-left"));
	placement->addItem(QStringLiteral("Справа снизу"), QStringLiteral("bottom-right"));
	form->addRow(QStringLiteral("Размещение на сцене"), placement);
	auto *caption = new QLineEdit(dialog);
	caption->setMaxLength(64);
	caption->setPlaceholderText(QStringLiteral("Свой текст эффекта (необязательно)"));
	auto *captionLabel = new QLabel(QStringLiteral("Текст реакции"), dialog);
	form->addRow(captionLabel, caption);
	auto *messageAlpha = new QSpinBox(dialog);
	messageAlpha->setRange(0, 100);
	messageAlpha->setSuffix(QStringLiteral(" %"));
	auto *messageAlphaLabel = new QLabel(QStringLiteral("Фон сообщений"), dialog);
	form->addRow(messageAlphaLabel, messageAlpha);
	auto *panelAlpha = new QSpinBox(dialog);
	panelAlpha->setRange(0, 100);
	panelAlpha->setSuffix(QStringLiteral(" %"));
	auto *panelAlphaLabel = new QLabel(QStringLiteral("Фон всей области"), dialog);
	form->addRow(panelAlphaLabel, panelAlpha);
	detail->addLayout(form);
	auto *help = new QLabel(QStringLiteral("Выбери пресет и добавь его прямо на текущую сцену OBS. "
		"Размер и размещение можно менять на холсте. Библиотека не блокирует OBS."), dialog);
	help->setWordWrap(true);
	detail->addWidget(help);
	detail->addStretch();
	auto *buttons = new QHBoxLayout();
	auto *savePreset = new QPushButton(QStringLiteral("Сохранить вариант как мой пресет"), dialog);
	savePreset->setToolTip(QStringLiteral("Сохраняет выбранный шаблон с изменениями из этой формы."));
	buttons->addWidget(savePreset);
	auto *removePreset = new QPushButton(QStringLiteral("Удалить мой"), dialog);
	buttons->addWidget(removePreset);
	detail->addLayout(buttons);
	auto *updateSelected = new QPushButton(QStringLiteral("Применить к выделенному источнику"), dialog);
	updateSelected->setToolTip(QStringLiteral("Обновить оформление выделенного источника XINKX на сцене без удаления и повторного добавления."));
	detail->addWidget(updateSelected);
	auto *add = new QPushButton(QStringLiteral("Добавить на сцену"), dialog);
	add->setDefault(true);
	detail->addWidget(add);
	page->addLayout(detail, 2);

	auto catalog = std::make_shared<QList<SourcePreset>>();
	const int builtinCount = 19;
	const auto rebuild = [=] {
		*catalog = catalogPresets();
		items->clear();
		for (const auto &entry : std::as_const(*catalog)) {
			auto *row = new QListWidgetItem(QIcon(catalogPreview(entry).scaled(128, 72, Qt::KeepAspectRatio,
				Qt::SmoothTransformation)), entry.title, items);
			row->setSizeHint(QSize(320, 82));
			row->setToolTip(entry.description);
		}
		if (!catalog->isEmpty()) items->setCurrentRow(0);
	};
	QObject::connect(items, &QListWidget::currentRowChanged, dialog, [=](int index) {
		if (index < 0 || index >= catalog->size()) return;
		const auto &entry = catalog->at(index);
		preview->setPixmap(catalogPreview(entry).scaled(600, 340, Qt::KeepAspectRatio,
			Qt::SmoothTransformation));
		description->setText(entry.description);
		name->setText(entry.title);
		width->setValue(entry.settings.value(QStringLiteral("width")).toInt(entry.layer == QStringLiteral("chat") ? 720 :
			entry.layer == QStringLiteral("frame") ? 800 : entry.layer == QStringLiteral("timer") ? 480 :
			entry.layer == QStringLiteral("viewers") ? 520 : 1920));
		height->setValue(entry.settings.value(QStringLiteral("height")).toInt(entry.layer == QStringLiteral("chat") ? 560 :
			entry.layer == QStringLiteral("frame") ? 450 : entry.layer == QStringLiteral("timer") ? 180 :
			entry.layer == QStringLiteral("viewers") ? 100 : 1080));
		caption->setVisible(entry.layer == QStringLiteral("effects"));
		captionLabel->setVisible(entry.layer == QStringLiteral("effects"));
		caption->setText(entry.settings.value(QStringLiteral("customText")).toString());
		const bool isChat = entry.layer == QStringLiteral("chat");
		messageAlphaLabel->setVisible(isChat);
		messageAlpha->setVisible(isChat);
		panelAlphaLabel->setVisible(isChat);
		panelAlpha->setVisible(isChat);
		messageAlpha->setValue(static_cast<int>((static_cast<uint32_t>(entry.settings
			.value(QStringLiteral("messageColor")).toDouble(0xdc2b1508u)) >> 24) * 100 / 255));
		panelAlpha->setValue(static_cast<int>((static_cast<uint32_t>(entry.settings
			.value(QStringLiteral("panelColor")).toDouble(0x002c1509u)) >> 24) * 100 / 255));
		placement->setCurrentIndex(placement->findData(entry.layer == QStringLiteral("chat")
			? QStringLiteral("bottom-left") : entry.layer == QStringLiteral("frame")
			? QStringLiteral("top-right") : QStringLiteral("center")));
		removePreset->setEnabled(index >= builtinCount);
	});
	const auto refreshPreview = [=] {
		const int index = items->currentRow();
		if (index < 0 || index >= catalog->size()) return;
		SourcePreset modified = catalog->at(index);
		if (modified.layer == QStringLiteral("effects"))
			modified.settings.insert(QStringLiteral("customText"), caption->text());
		if (modified.layer == QStringLiteral("chat")) {
			for (const auto &row : {std::pair{QStringLiteral("messageColor"), messageAlpha->value()},
					std::pair{QStringLiteral("panelColor"), panelAlpha->value()}}) {
				const uint32_t before = static_cast<uint32_t>(modified.settings.value(row.first).toDouble());
				const uint32_t combined = (before & 0x00ffffffu) |
					(static_cast<uint32_t>(row.second * 255 / 100) << 24);
				modified.settings.insert(row.first, static_cast<qint64>(combined));
			}
		}
		preview->setPixmap(catalogPreview(modified).scaled(600, 340,
			Qt::KeepAspectRatio, Qt::SmoothTransformation));
	};
	QObject::connect(caption, &QLineEdit::textChanged, dialog, refreshPreview);
	QObject::connect(messageAlpha, &QSpinBox::valueChanged, dialog, refreshPreview);
	QObject::connect(panelAlpha, &QSpinBox::valueChanged, dialog, refreshPreview);
	const auto configurationFor = [=](const SourcePreset &entry, const QString &originalReaction = QString{}) {
		QJsonObject configuration = entry.settings;
		configuration.insert(QStringLiteral("width"), width->value());
		configuration.insert(QStringLiteral("height"), height->value());
		configuration.insert(QStringLiteral("customize"), true);
		if (entry.layer == QStringLiteral("chat")) {
			for (const auto &row : {std::pair{QStringLiteral("messageColor"), messageAlpha->value()},
					std::pair{QStringLiteral("panelColor"), panelAlpha->value()}}) {
				const uint32_t before = static_cast<uint32_t>(configuration.value(row.first).toDouble());
				configuration.insert(row.first, static_cast<qint64>((before & 0x00ffffffu) |
					(static_cast<uint32_t>(row.second * 255 / 100) << 24)));
			}
		}
		if (entry.layer == QStringLiteral("effects")) {
			configuration.insert(QStringLiteral("customText"), caption->text().trimmed());
			if (configuration.value(QStringLiteral("reaction")).toString() == QStringLiteral("custom")) {
				configuration.insert(QStringLiteral("reaction"), originalReaction.startsWith(QStringLiteral("custom-"))
					? originalReaction : QStringLiteral("custom-") +
					QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
			}
		}
		return configuration;
	};
	const auto customEntries = [] {
		const QString path = catalogPath();
		if (path.isEmpty()) return QJsonArray{};
		QSettings settings(path, QSettings::IniFormat);
		return QJsonDocument::fromJson(settings.value(QStringLiteral("catalog/presets")).toByteArray()).array();
	};
	const auto writeCustom = [](const QJsonArray &value) {
		const QString path = catalogPath();
		if (path.isEmpty()) return false;
		QSettings settings(path, QSettings::IniFormat);
		settings.setValue(QStringLiteral("catalog/presets"), QJsonDocument(value).toJson(QJsonDocument::Compact));
		settings.sync();
		return settings.status() == QSettings::NoError;
	};
	QObject::connect(savePreset, &QPushButton::clicked, dialog, [=] {
		const int index = items->currentRow();
		if (index < 0 || index >= catalog->size()) return;
		const auto &selected = catalog->at(index);
		bool ok = false;
		const QString title = QInputDialog::getText(dialog, QStringLiteral("Мой пресет"),
			QStringLiteral("Название"), QLineEdit::Normal, name->text().trimmed(), &ok).trimmed().left(80);
		if (!ok || title.isEmpty()) return;
		auto entries = customEntries();
		if (entries.size() >= 100) {
			QMessageBox::warning(dialog, QStringLiteral("Пресеты"), QStringLiteral("Достигнут предел 100 пользовательских пресетов"));
			return;
		}
		entries.append(QJsonObject{{QStringLiteral("name"), title}, {QStringLiteral("layer"), selected.layer},
			{QStringLiteral("settings"), configurationFor(selected)}});
		if (writeCustom(entries)) { rebuild(); items->setCurrentRow(catalog->size() - 1); }
	});
	QObject::connect(removePreset, &QPushButton::clicked, dialog, [=] {
		const int index = items->currentRow() - builtinCount;
		auto entries = customEntries();
		if (index < 0 || index >= entries.size()) return;
		entries.removeAt(index);
		if (writeCustom(entries)) rebuild();
	});
	QObject::connect(updateSelected, &QPushButton::clicked, dialog, [=] {
		const int index = items->currentRow();
		if (index < 0 || index >= catalog->size()) return;
		OBSScene scene = mainWindow->GetCurrentScene();
		if (!scene) return;
		struct Selected { obs_sceneitem_t *item = nullptr; } selected;
		obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			if (!obs_sceneitem_selected(item)) return true;
			static_cast<Selected *>(data)->item = item;
			return false;
		}, &selected);
		if (!selected.item) {
			QMessageBox::information(dialog, QStringLiteral("Источник XINKX"),
				QStringLiteral("Выдели источник на сцене или в списке OBS."));
			return;
		}
		obs_source_t *source = obs_sceneitem_get_source(selected.item);
		const QString id = QString::fromUtf8(obs_source_get_id(source));
		const QString expected = QStringLiteral("xinkx_") + (catalog->at(index).layer == QStringLiteral("effects")
			? QStringLiteral("reactions") : catalog->at(index).layer);
		if (id != expected) {
			QMessageBox::warning(dialog, QStringLiteral("Источник XINKX"),
				QStringLiteral("Выбранное оформление не соответствует типу выделенного источника."));
			return;
		}
		OBSDataAutoRelease beforeSettings = obs_source_get_settings(source);
		const QString oldReaction = QString::fromUtf8(obs_data_get_string(beforeSettings, "reaction"));
		const QByteArray json = QJsonDocument(configurationFor(catalog->at(index), oldReaction))
			.toJson(QJsonDocument::Compact);
		OBSDataAutoRelease changes = obs_data_create_from_json(json.constData());
		OBSDataAutoRelease applied = obs_data_create();
		obs_data_apply(applied, beforeSettings);
		obs_data_apply(applied, changes);
		OBSData before = OBSBasic::BackupScene(scene);
		obs_source_update(source, applied);
		OBSData after = OBSBasic::BackupScene(scene);
		mainWindow->CreateSceneUndoRedoAction(QStringLiteral("XINKX: изменить оформление источника"), before, after);
		mainWindow->statusBar()->showMessage(QStringLiteral("Оформление источника обновлено"), 5000);
		// Keep the library open for working with several OBS sources.
	});
	QObject::connect(add, &QPushButton::clicked, dialog, [=] {
		const int index = items->currentRow();
		if (index < 0 || index >= catalog->size()) return;
		const auto &entry = catalog->at(index);
		QString desired = name->text().trimmed();
		if (desired.isEmpty()) return;
		if (!desired.startsWith(QStringLiteral("XINKX · "))) desired.prepend(QStringLiteral("XINKX · "));
		QString unique = desired;
		for (int suffix = 2; suffix < 1000; ++suffix) {
			auto *existing = obs_get_source_by_name(unique.toUtf8().constData());
			if (!existing) break;
			obs_source_release(existing);
			unique = QStringLiteral("%1 (%2)").arg(desired).arg(suffix);
		}
		QJsonObject configuration = configurationFor(entry);
		QString message;
		if (runtime->addToScene(mainWindow, &message, entry.layer, unique, configuration,
				placement->currentData().toString())) {
			mainWindow->statusBar()->showMessage(message, 7000);
			// Stay in the library; the live OBS scene remains interactive.
		} else {
			QMessageBox::warning(dialog, QStringLiteral("Источник XINKX"), message);
		}
	});
	rebuild();
	dialog->show();
}

void OverlayRuntime::showCatalog(OBSBasic *mainWindow, QWidget *parent)
{
	openSourceCatalog(mainWindow, this, parent);
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
	auto *catalogButton = new QPushButton(QStringLiteral("Каталог источников · чаты, реакции, рамки"), content);
	layout->addWidget(catalogButton);
	QObject::connect(catalogButton, &QPushButton::clicked, dock, [=] {
		runtime->showCatalog(mainWindow, dock);
	});
	auto *sourceHint = new QLabel(QStringLiteral("Выбери оформление в каталоге, посмотри пример и добавь на сцену. "
		"Перемещай и меняй размер за маркеры в предпросмотре OBS. "
		"Настройки каждого источника и свои пресеты сохраняются отдельно."), content);
	sourceHint->setWordWrap(true);
	layout->addWidget(sourceHint);
	auto *testSelected = new QPushButton(QStringLiteral("Проверить выделенный источник на сцене"), content);
	layout->addWidget(testSelected);
	QObject::connect(testSelected, &QPushButton::clicked, dock, [=] {
		OBSScene scene = mainWindow->GetCurrentScene();
		if (!scene) return;
		struct Selected { obs_source_t *source = nullptr; } selected;
		obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			if (!obs_sceneitem_selected(item)) return true;
			auto *source = obs_sceneitem_get_source(item);
			if (source && QString::fromUtf8(obs_source_get_id(source)).startsWith(QStringLiteral("xinkx_"))) {
				static_cast<Selected *>(data)->source = source;
				return false;
			}
			return true;
		}, &selected);
		if (!selected.source) {
			status->setText(QStringLiteral("Выдели источник XINKX в OBS для проверки"));
			return;
		}
		const QString type = QString::fromUtf8(obs_source_get_id(selected.source));
		if (type == QStringLiteral("xinkx_chat"))
			runtime->publishChat(QStringLiteral("twitch"), QStringLiteral("Тестовый зритель"),
				QStringLiteral("Пример сообщения — ширина и фон задаются размером источника"));
		else if (type == QStringLiteral("xinkx_reactions")) {
			OBSDataAutoRelease data = obs_source_get_settings(selected.source);
			const QString reaction = QString::fromUtf8(obs_data_get_string(data, "reaction"));
			runtime->reaction(reaction == QStringLiteral("all") ? QStringLiteral("gg") : reaction);
		} else if (type == QStringLiteral("xinkx_alerts")) runtime->testAlert(QStringLiteral("donation"));
		else if (type == QStringLiteral("xinkx_timer")) runtime->startTimer(60);
		else status->setText(QStringLiteral("Этот источник всегда виден на сцене"));
	});
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
	auto *alertReadiness = new QLabel(testGroup);
	alertReadiness->setObjectName(QStringLiteral("xinkxAlertReadiness"));
	alertReadiness->setWordWrap(true);
	testLayout->addWidget(alertReadiness);
	auto *attachAlerts = new QPushButton(QStringLiteral("Добавить оповещения в текущую сцену"), testGroup);
	attachAlerts->setObjectName(QStringLiteral("xinkxAttachAlerts"));
	testLayout->addWidget(attachAlerts);
	auto *lastRealAlert = new QLabel(QStringLiteral("Реальных событий пока не поступало"), testGroup);
	lastRealAlert->setObjectName(QStringLiteral("xinkxLastRealAlert"));
	lastRealAlert->setWordWrap(true);
	testLayout->addWidget(lastRealAlert);
	auto *alertTransport = new QLabel(QStringLiteral("Twitch: ожидаем проверку EventSub подписок и фолловеров\n"
		"YouTube: уведомления о платном членстве приходят из активного чата\n"
		"Kick: фолловеры/подписки требуют официального webhook; без публичного адреса не подключены"), testGroup);
	alertTransport->setObjectName(QStringLiteral("xinkxAlertTransportHealth"));
	alertTransport->setWordWrap(true);
	alertTransport->setTextInteractionFlags(Qt::TextSelectableByMouse);
	testLayout->addWidget(alertTransport);
	auto twitchEventStatus = std::make_shared<QHash<QString, QString>>();
	const auto refreshAlertReadiness = [=] {
		const QString health = runtime->alertSourceHealth(mainWindow);
		const bool ready = health.startsWith(QStringLiteral("Готово:"));
		alertReadiness->setText(health);
		alertReadiness->setStyleSheet(ready ? QStringLiteral("color:#82d8d3")
			: QStringLiteral("color:#efbd6d"));
		attachAlerts->setEnabled(health.contains(QStringLiteral("отсутствует")));
	};
	QObject::connect(attachAlerts, &QPushButton::clicked, content, [=] {
		QString message;
		runtime->addToScene(mainWindow, &message, QStringLiteral("alerts"));
		status->setText(message);
		refreshAlertReadiness();
	});
	auto *alertStateTimer = new QTimer(testGroup);
	alertStateTimer->setInterval(2000);
	QObject::connect(alertStateTimer, &QTimer::timeout, testGroup, refreshAlertReadiness);
	alertStateTimer->start();
	refreshAlertReadiness();
	// The ChatGateway is installed after the Effects tab. Listen only to
	// genuine provider notifications, not the local test buttons above.
	QTimer::singleShot(0, testGroup, [=] {
		if (auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"))) {
			QObject::connect(gateway, &ChatGateway::alertTransportStatus, testGroup,
				[=](const QString &, const QString &eventType, bool active, const QString &details) {
					QString eventName;
					if (eventType == QStringLiteral("channel.subscribe")) eventName = QStringLiteral("подписки");
					else if (eventType == QStringLiteral("channel.follow")) eventName = QStringLiteral("фолловеры");
					else if (eventType == QStringLiteral("channel.cheer")) eventName = QStringLiteral("Bits");
					else return;
					(*twitchEventStatus)[eventName] = QStringLiteral("Twitch %1: %2 · %3")
						.arg(eventName, active ? QStringLiteral("готово") : QStringLiteral("ошибка"), details.left(120));
					QStringList messages;
					for (const auto &kind : {QStringLiteral("подписки"), QStringLiteral("фолловеры"), QStringLiteral("Bits")}) {
						if (twitchEventStatus->contains(kind)) messages << twitchEventStatus->value(kind);
					}
					messages << QStringLiteral("YouTube: платное членство — события активного чата")
						<< QStringLiteral("Kick: подписки/фолловеры без публичного webhook не подключены");
					alertTransport->setText(messages.join(QLatin1Char('\n')));
				});
			QObject::connect(gateway, &ChatGateway::incoming, testGroup, [=](const ChatLine &event) {
				if (event.type == QStringLiteral("chat")) return;
				lastRealAlert->setText(QStringLiteral("%1 · %2 · %3 · %4")
					.arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
						platformTitle(event.platform), event.type, event.name));
				refreshAlertReadiness();
			});
		}
	});
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
	QObject::connect(runtime, &OverlayRuntime::eventDispatched, dock, [](const QString &label) {
		writeDiagnostic(QStringLiteral("effect"), label);
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
