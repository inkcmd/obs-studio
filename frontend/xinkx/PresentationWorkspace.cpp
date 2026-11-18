#include "PresentationSettings.hpp"

#include "ChatGateway.hpp"
#include "MarkerEngine.hpp"
#include "OverlayRuntime.hpp"
#include "StudioWorkspace.hpp"
#include "WheelFocusGuard.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDockWidget>
#include <QDynamicPropertyChangeEvent>
#include <QFontDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegion>
#include <QScrollArea>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace xinkx {

namespace {

class StreamerOverlay final : public QWidget {
public:
	struct Chat {
		QString platform;
		QString name;
		QString text;
		qint64 at = 0;
	};
	struct Effect {
		QString text;
		QPointF location;
		qint64 born = 0;
		qint64 until = 0;
		int particleCount = 0;
		quint32 seed = 0;
		QString kind;
		QString name;
		QString detail;
		QString platform;
		QString alertPosition;
		QImage card;
	};

	explicit StreamerOverlay(QWidget *parent = nullptr)
		: QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
				Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setFocusPolicy(Qt::NoFocus);
		auto *timer = new QTimer(this);
		// Match the browser overlay's animation cadence while an alert is visible.
		timer->setInterval(16);
		timer->setTimerType(Qt::PreciseTimer);
		connect(timer, &QTimer::timeout, this, [this] {
			const qint64 now = QDateTime::currentMSecsSinceEpoch();
			const int before = events.size();
			const int previousMessages = messages.size();
			bool toastExpired = false;
			const int duration = profile.value(QStringLiteral("chat")).toObject()
				.value(QStringLiteral("messageDurationMs")).toInt(12000);
			if (duration > 0) messages.erase(std::remove_if(messages.begin(), messages.end(),
				[now, duration](const Chat &chat) { return now - chat.at > duration; }), messages.end());
			events.erase(std::remove_if(events.begin(), events.end(), [now](const Effect &event) {
				return event.until > 0 && event.until < now;
			}), events.end());
			const int max = std::max(1, profile.value(QStringLiteral("effects")).toObject()
				.value(QStringLiteral("maxConcurrent")).toInt(2));
			while (events.size() < max && !pendingEvents.isEmpty()) {
				Effect next = pendingEvents.takeFirst();
				if (next.until > 0) next.until = now + (next.until - next.born);
				next.born = now;
				events.push_back(std::move(next));
			}
			if (markerToastUntil > 0 && now >= markerToastUntil) {
				markerToastUntil = 0;
				markerToast.clear();
				toastExpired = true;
			}
			if (before != events.size() || previousMessages != messages.size() ||
			    !events.isEmpty() || markerToastUntil > 0 || toastExpired) update();
		});
		timer->start();
	}

	void toggle(bool enabled)
	{
		if (!enabled) { hide(); return; }
		if (auto *screen = QGuiApplication::primaryScreen()) setGeometry(screen->geometry());
		show();
		syncMouseMode();
	}

	void setLayoutWriter(std::function<void(const QJsonObject &)> writer) { saveLayout = std::move(writer); }

	void setEditing(bool value)
	{
		if (editing == value) return;
		const bool wasVisible = isVisible();
		editing = value;
		setWindowFlag(Qt::WindowTransparentForInput, !editing);
		setAttribute(Qt::WA_TransparentForMouseEvents, !editing);
		if (wasVisible) show();
		syncMouseMode();
		update();
	}

	void syncMouseMode()
	{
#ifdef _WIN32
		const auto handle = reinterpret_cast<HWND>(winId());
		const LONG_PTR flags = GetWindowLongPtrW(handle, GWL_EXSTYLE);
		LONG_PTR next = flags | WS_EX_LAYERED | WS_EX_NOACTIVATE;
		if (editing) next &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
		else next |= WS_EX_TRANSPARENT;
		SetWindowLongPtrW(handle, GWL_EXSTYLE, next);
		SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
		if (editing) setMask(editingRegion());
		else clearMask();
	}

	void configure(const QJsonObject &document)
	{
		profile = document.value(QStringLiteral("streamer")).toObject();
		if (editing) setMask(editingRegion());
		update();
	}

	void addChat(const ChatLine &line)
	{
		if (line.type != QStringLiteral("chat")) return;
		messages.push_back({line.platform, line.name.left(80), line.text.left(240),
			QDateTime::currentMSecsSinceEpoch()});
		const int limit = profile.value(QStringLiteral("chat")).toObject().value(QStringLiteral("maxMessages")).toInt(8);
		while (messages.size() > limit) messages.removeFirst();
		if (editing) setMask(editingRegion());
		update();
	}

	void addEffect(const QJsonObject &event)
	{
		const auto params = profile.value(QStringLiteral("effects")).toObject();
		if (!params.value(QStringLiteral("enabled")).toBool()) return;
		const int max = params.value(QStringLiteral("maxConcurrent")).toInt(2);
		const QString kind = event.value(QStringLiteral("type")).toString().toUpper();
		const QString text = kind == QStringLiteral("CONFETTI") ? QStringLiteral("✦ ✦ ✦")
			: kind == QStringLiteral("LOW-HP") ? QStringLiteral("LOW HP") : kind;
		const QString position = event.value(QStringLiteral("kind")).toString() == QStringLiteral("alert")
			? event.value(QStringLiteral("alertPosition")).toString(QStringLiteral("top-right"))
			: params.value(QStringLiteral("position")).toString();
		QPointF point;
		if (position == QStringLiteral("top-left")) point = {0.22, 0.18};
		else if (position == QStringLiteral("top-right")) point = {0.78, 0.18};
		else if (position == QStringLiteral("bottom-left")) point = {0.22, 0.78};
		else if (position == QStringLiteral("bottom-right")) point = {0.78, 0.78};
		else if (position == QStringLiteral("center")) point = {0.5, 0.5};
		else point = {0.15 + QRandomGenerator::global()->generateDouble() * 0.7,
			0.15 + QRandomGenerator::global()->generateDouble() * 0.7};
		int duration = event.contains(QStringLiteral("durationMs"))
			? event.value(QStringLiteral("durationMs")).toInt() : params.value(QStringLiteral("durationMs")).toInt(5000);
		const int min = params.value(QStringLiteral("particleMin")).toInt(12);
		const int maxParticles = std::max(min, params.value(QStringLiteral("particleMax")).toInt(30));
		const int count = (kind == QStringLiteral("GG") || kind == QStringLiteral("CONFETTI"))
			? min + QRandomGenerator::global()->bounded(maxParticles - min + 1) : 0;
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		const QString alertKind = event.value(QStringLiteral("kind")).toString();
		QStringList details;
		for (const auto &value : {event.value(QStringLiteral("amount")).toString(),
					 event.value(QStringLiteral("message")).toString()})
			if (!value.isEmpty()) details.append(value);
		const QString detail = details.join(QStringLiteral(" · "));
		const QString cardPng = event.value(QStringLiteral("cardPng")).toString();
		const QImage card = cardPng.isEmpty() ? QImage{} :
			QImage::fromData(QByteArray::fromBase64(cardPng.toLatin1()), "PNG");
		const qint64 until = duration == 0 ? 0 :
			now + std::max(200, duration) + (alertKind == QStringLiteral("alert") ? 350 : 0);
		Effect next{text, point, now, until,
			count, QRandomGenerator::global()->generate(), alertKind,
			event.value(QStringLiteral("name")).toString().left(80), detail.left(260),
			event.value(QStringLiteral("platform")).toString().left(24),
			event.value(QStringLiteral("alertPosition")).toString(), card};
		if (events.size() >= max && !events.isEmpty() && events.first().until == 0)
			events.removeFirst();
		if (events.size() >= max || !pendingEvents.isEmpty()) {
			if (pendingEvents.size() >= 20) pendingEvents.removeFirst();
			pendingEvents.push_back(std::move(next));
		} else {
			events.push_back(std::move(next));
		}
		update();
	}

	void showMarkerToast(const QString &label)
	{
		markerToast = QStringLiteral("✓ Метка создана · %1").arg(label.left(80));
		markerToastUntil = QDateTime::currentMSecsSinceEpoch() + 2600;
		update();
	}

	void clearEffects() { events.clear(); pendingEvents.clear(); update(); }
	void setViewers(const QString &platform, int count)
	{
		// An unavailable API count is distinct from zero viewers.
		viewers.insert(platform, count);
		update();
	}

protected:
	bool event(QEvent *event) override
	{
		if (event->type() == QEvent::DynamicPropertyChange) {
			auto *changed = static_cast<QDynamicPropertyChangeEvent *>(event);
			if (changed->propertyName() == QByteArray("xinkxEditChat")) setEditing(property("xinkxEditChat").toBool());
		}
		return QWidget::event(event);
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (editing && event->button() == Qt::LeftButton) {
			if (viewersBounds().adjusted(-10, -10, 10, 10).contains(event->position().toPoint())) {
				draggingViewers = true;
				press = event->globalPosition().toPoint();
				const auto settings = profile.value(QStringLiteral("viewers")).toObject();
				origin = QPoint(settings.value(QStringLiteral("offsetX")).toInt(),
					settings.value(QStringLiteral("offsetY")).toInt());
				event->accept();
				return;
			}
			const QRect bounds = chatBounds();
			resizing = QRect(bounds.right() - 28, bounds.bottom() - 28, 40, 40)
				.contains(event->position().toPoint());
			dragging = !resizing;
			press = event->globalPosition().toPoint();
			startSize = bounds.size();
			const auto chat = profile.value(QStringLiteral("chat")).toObject();
			origin = QPoint(chat.value(QStringLiteral("offsetX")).toInt(), chat.value(QStringLiteral("offsetY")).toInt());
			event->accept();
		}
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!dragging && !resizing && !draggingViewers) return;
		const QPoint delta = event->globalPosition().toPoint() - press;
		if (draggingViewers) {
			auto settings = profile.value(QStringLiteral("viewers")).toObject();
			settings.insert(QStringLiteral("offsetX"), qBound(-3840, origin.x() + delta.x(), 3840));
			settings.insert(QStringLiteral("offsetY"), qBound(-2160, origin.y() + delta.y(), 2160));
			profile.insert(QStringLiteral("viewers"), settings);
			setMask(editingRegion());
			update();
			return;
		}
		auto chat = profile.value(QStringLiteral("chat")).toObject();
		if (resizing) {
			chat.insert(QStringLiteral("widthPercent"), qBound(10,
				(startSize.width() + delta.x()) * 100 / std::max(1, width()), 100));
			chat.insert(QStringLiteral("heightPx"), qBound(100, startSize.height() + delta.y(), 4320));
		} else {
			chat.insert(QStringLiteral("offsetX"), qBound(-3840, origin.x() + delta.x(), 3840));
			chat.insert(QStringLiteral("offsetY"), qBound(-2160, origin.y() + delta.y(), 2160));
		}
		profile.insert(QStringLiteral("chat"), chat);
		setMask(editingRegion());
		update();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if ((!dragging && !resizing && !draggingViewers) || event->button() != Qt::LeftButton) return;
		if (draggingViewers) {
			draggingViewers = false;
			if (saveLayout) saveLayout(QJsonObject{{QStringLiteral("viewers"),
				profile.value(QStringLiteral("viewers"))}});
			return;
		}
		dragging = false;
		resizing = false;
		const auto chat = profile.value(QStringLiteral("chat")).toObject();
		if (saveLayout) saveLayout(QJsonObject{{QStringLiteral("chat"), chat}});
	}

	QRegion editingRegion() const
	{
		return QRegion(chatBounds().adjusted(-12, -12, 12, 12)) |
			QRegion(viewersBounds().adjusted(-12, -12, 12, 12));
	}

	QRect viewersBounds() const
	{
		const auto settings = profile.value(QStringLiteral("viewers")).toObject();
		QFont font(QStringLiteral("Segoe UI"));
		font.setPixelSize(settings.value(QStringLiteral("fontSize")).toInt(18));
		font.setWeight(QFont::DemiBold);
		const QFontMetrics metrics(font);
		const int padding = settings.value(QStringLiteral("padding")).toInt(10);
		int contentWidth = 0;
		for (const auto &platform : {QStringLiteral("kick"), QStringLiteral("youtube"), QStringLiteral("twitch")}) {
			if (!viewers.contains(platform)) continue;
			contentWidth += 22 + metrics.horizontalAdvance(viewers.value(platform) < 0
				? QStringLiteral("—") : QString::number(viewers.value(platform))) + 12;
		}
		const int boxWidth = std::max(92, contentWidth + padding * 2);
		const int boxHeight = metrics.height() + padding * 2;
		const QString horizontal = settings.value(QStringLiteral("horizontal")).toString(QStringLiteral("right"));
		const QString vertical = settings.value(QStringLiteral("vertical")).toString(QStringLiteral("top"));
		const int x = (horizontal == QStringLiteral("right") ? width() - boxWidth - 24 :
			horizontal == QStringLiteral("center") ? (width() - boxWidth) / 2 : 24) +
			settings.value(QStringLiteral("offsetX")).toInt();
		const int y = (vertical == QStringLiteral("bottom") ? height() - boxHeight - 24 :
			vertical == QStringLiteral("center") ? (height() - boxHeight) / 2 : 24) +
			settings.value(QStringLiteral("offsetY")).toInt();
		return QRect(x, y, boxWidth, boxHeight);
	}

	QRect chatBounds() const
	{
		const auto chat = profile.value(QStringLiteral("chat")).toObject();
		QFont font(chat.value(QStringLiteral("font")).toString(QStringLiteral("Segoe UI")));
		font.setPixelSize(chat.value(QStringLiteral("fontSize")).toInt(20));
		const int padding = chat.value(QStringLiteral("padding")).toInt(10);
		const int gap = chat.value(QStringLiteral("gap")).toInt(7);
		const int width = std::max(150, this->width() * chat.value(QStringLiteral("widthPercent")).toInt(60) / 100);
		const int automatic = std::max(120, int(messages.size()) *
			(QFontMetrics(font).height() * 2 + padding * 2 + 4 + gap));
		const int height = chat.value(QStringLiteral("heightPx")).toInt() > 0
			? chat.value(QStringLiteral("heightPx")).toInt() : automatic;
		const QString vertical = chat.value(QStringLiteral("vertical")).toString();
		const QString horizontal = chat.value(QStringLiteral("horizontal")).toString();
		const int x = (horizontal == QStringLiteral("right") ? this->width() - width - 36 :
			horizontal == QStringLiteral("center") ? (this->width() - width) / 2 : 36) +
			chat.value(QStringLiteral("offsetX")).toInt();
		const int y = (vertical == QStringLiteral("top") ? 36 :
			vertical == QStringLiteral("center") ? (this->height() - height) / 2 : this->height() - height - 36) +
			chat.value(QStringLiteral("offsetY")).toInt();
		return QRect(x, y, width, height);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const auto chat = profile.value(QStringLiteral("chat")).toObject();
		const auto viewerSettings = profile.value(QStringLiteral("viewers")).toObject();
		const QRect viewerRect = viewersBounds();
		if (chat.value(QStringLiteral("showViewers")).toBool() && (!viewers.isEmpty() || editing)) {
			QFont viewerFont(QStringLiteral("Segoe UI"));
			viewerFont.setPixelSize(viewerSettings.value(QStringLiteral("fontSize")).toInt(18));
			viewerFont.setWeight(QFont::DemiBold);
			painter.setFont(viewerFont);
			painter.setPen(Qt::NoPen);
			QColor viewerBg(viewerSettings.value(QStringLiteral("backgroundColor")).toString(QStringLiteral("#09152c")));
			viewerBg.setAlphaF(viewerSettings.value(QStringLiteral("backgroundOpacity")).toInt(0) / 100.0);
			painter.setBrush(viewerBg);
			const int viewerRadius = viewerSettings.value(QStringLiteral("radius")).toInt(10);
			painter.drawRoundedRect(viewerRect, viewerRadius, viewerRadius);
			painter.setPen(QColor(viewerSettings.value(QStringLiteral("textColor")).toString(QStringLiteral("#6be4ef"))));
			const int viewerPadding = viewerSettings.value(QStringLiteral("padding")).toInt(10);
			int x = viewerRect.x() + viewerPadding;
			for (const QString &platform : {QStringLiteral("kick"), QStringLiteral("youtube"), QStringLiteral("twitch")}) {
				if (!viewers.contains(platform)) continue;
				const int count = viewers.value(platform);
				const QString value = count < 0 ? QStringLiteral("—") : QString::number(count);
				const int textWidth = QFontMetrics(viewerFont).horizontalAdvance(value);
				const QPixmap icon(QStringLiteral(":/xinkx/%1.svg").arg(platform));
				if (!icon.isNull()) painter.drawPixmap(x, viewerRect.center().y() - 9, 18, 18, icon);
				x += 22;
				painter.drawText(QRect(x, viewerRect.y(), textWidth + 2, viewerRect.height()), Qt::AlignVCenter, value);
				x += textWidth + 12;
			}
		}
		if (markerToastUntil > QDateTime::currentMSecsSinceEpoch()) {
			painter.save();
			const qint64 left = markerToastUntil - QDateTime::currentMSecsSinceEpoch();
			painter.setOpacity(std::min(1.0, double(left) / 350.0));
			QFont toastFont(QStringLiteral("Segoe UI"));
			toastFont.setPixelSize(17);
			toastFont.setBold(true);
			painter.setFont(toastFont);
			const int toastWidth = std::max(viewerRect.width(), QFontMetrics(toastFont).horizontalAdvance(markerToast) + 28);
			const int toastX = qBound(8, viewerRect.x(), std::max(8, width() - toastWidth - 8));
			const int toastY = viewerRect.bottom() + 48 <= height()
				? viewerRect.bottom() + 9 : viewerRect.top() - 48;
			const QRect toast(toastX, toastY, toastWidth, 39);
			painter.setPen(QPen(QColor(QStringLiteral("#6be4ef")), 1));
			painter.setBrush(QColor(QStringLiteral("#09152c")));
			painter.drawRoundedRect(toast, 10, 10);
			painter.setPen(QColor(QStringLiteral("#dffaff")));
			painter.drawText(toast.adjusted(14, 0, -8, 0), Qt::AlignVCenter, markerToast);
			painter.restore();
		}
		QFont font(chat.value(QStringLiteral("font")).toString(QStringLiteral("Segoe UI")));
		font.setPixelSize(chat.value(QStringLiteral("fontSize")).toInt(20));
		font.setWeight(static_cast<QFont::Weight>(chat.value(QStringLiteral("fontWeight")).toInt(600)));
		painter.setFont(font);
		const auto metric = QFontMetrics(font);
		const int padding = chat.value(QStringLiteral("padding")).toInt(10);
		const int gap = chat.value(QStringLiteral("gap")).toInt(7);
		const int width = std::max(150, this->width() * chat.value(QStringLiteral("widthPercent")).toInt(60) / 100);
		const int rowHeight = metric.height() * 2 + padding * 2 + 4;
		const QRect bounds = chatBounds();
		const int usedHeight = int(messages.size()) * (rowHeight + gap) - (messages.isEmpty() ? 0 : gap);
		int y = bounds.bottom() + 1 - std::min(bounds.height(), std::max(0, usedHeight));
		const int x = bounds.x();
		QColor bg(chat.value(QStringLiteral("backgroundColor")).toString(QStringLiteral("#09152c")));
		bg.setAlphaF(chat.value(QStringLiteral("opacity")).toDouble(88) / 100.0);
		QColor panelBg(chat.value(QStringLiteral("panelColor")).toString(QStringLiteral("#09152c")));
		panelBg.setAlphaF(chat.value(QStringLiteral("panelOpacity")).toDouble(0) / 100.0);
		const QColor nameColor(chat.value(QStringLiteral("nameColor")).toString(QStringLiteral("#8de9fb")));
		const QColor textColor(chat.value(QStringLiteral("textColor")).toString(QStringLiteral("#f6faff")));
		if (panelBg.alpha() > 0) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(panelBg);
			painter.drawRoundedRect(bounds, chat.value(QStringLiteral("radius")).toInt(6),
				chat.value(QStringLiteral("radius")).toInt(6));
		}
		if (editing) {
			painter.setPen(QPen(QColor(QStringLiteral("#69d8ea")), 2, Qt::DashLine));
			painter.setBrush(QColor(30, 43, 66, 110));
			painter.drawRoundedRect(bounds, 7, 7);
			painter.setPen(Qt::white);
			painter.drawText(bounds.adjusted(14, 10, -14, -10), Qt::AlignTop,
				QStringLiteral("Перетащи чат · потяни за угол для изменения размеров"));
		}
		for (const auto &row : messages) {
			const QRect rect(x, y, width, rowHeight);
			painter.setPen(Qt::NoPen);
			painter.setBrush(bg);
			painter.drawRoundedRect(rect, chat.value(QStringLiteral("radius")).toInt(6),
				chat.value(QStringLiteral("radius")).toInt(6));
			painter.setPen(nameColor);
			const QString label = row.name +
				(chat.value(QStringLiteral("showTime")).toBool()
				? QStringLiteral(" · ") + QDateTime::fromMSecsSinceEpoch(row.at).toString(QStringLiteral("HH:mm")) : QString{});
			int labelX = x + padding;
			if (chat.value(QStringLiteral("showPlatform")).toBool()) {
				const QPixmap icon(QStringLiteral(":/xinkx/%1.svg").arg(row.platform));
				if (!icon.isNull()) painter.drawPixmap(labelX, y + padding + (metric.height() - 18) / 2, 18, 18, icon);
				labelX += 23;
			}
			painter.drawText(QRect(labelX, y + padding, width - (labelX - x) - padding, metric.height()),
				Qt::AlignVCenter, metric.elidedText(label, Qt::ElideRight, width - (labelX - x) - padding));
			painter.setPen(textColor);
			painter.drawText(QRect(x + padding, y + padding + metric.height(), width - padding * 2, metric.height()),
				Qt::AlignVCenter, metric.elidedText(row.text, Qt::ElideRight, width - padding * 2));
			y += rowHeight + gap;
		}
		if (editing) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(QStringLiteral("#69d8ea")));
			painter.drawRect(QRect(bounds.right() - 13, bounds.bottom() - 13, 13, 13));
			painter.setBrush(Qt::NoBrush);
			painter.setPen(QPen(QColor(QStringLiteral("#69d8ea")), 2, Qt::DashLine));
			painter.drawRoundedRect(viewerRect, 8, 8);
		}
		const auto effects = profile.value(QStringLiteral("effects")).toObject();
		QFont effectFont(QStringLiteral("Segoe UI"));
		effectFont.setBold(true);
		effectFont.setPixelSize(100 * effects.value(QStringLiteral("scalePercent")).toInt(70) / 100);
		painter.setFont(effectFont);
		painter.setPen(QColor(QStringLiteral("#6be4ef")));
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		for (const auto &event : events) {
			const double progress = event.until > event.born
				? qBound(0.0, double(now - event.born) / double(event.until - event.born), 1.0) : 0.35;
			painter.save();
			painter.setOpacity(std::min({1.0, progress * 7.0 + 0.1, (1.0 - progress) * 5.0}));
			if (event.kind == QStringLiteral("alert") && !event.card.isNull()) {
				// This image is the very same PNG sent to the OBS browser overlay.
				// Desktop and broadcast may independently position and zoom it.
				const double entry = qBound(0.0, double(now - event.born) / 240.0, 1.0);
				const double eased = 1.0 - std::pow(1.0 - entry, 3.0);
				const double exit = event.until > 0
					? qBound(0.0, double(event.until - now) / 350.0, 1.0) : 1.0;
				const double animatedScale = (0.78 + 0.22 * eased) * (0.94 + 0.06 * exit);
				painter.setOpacity(entry * exit);
				painter.setRenderHint(QPainter::SmoothPixmapTransform);
				const double zoom = effects.value(QStringLiteral("scalePercent")).toInt(70) / 100.0;
				const double cardWidth = event.card.width() * zoom;
				const double cardHeight = event.card.height() * zoom;
				const QString position = event.alertPosition;
				const double x = position == QStringLiteral("top-left") ? this->width() * 0.04 :
					position == QStringLiteral("center") ? (this->width() - cardWidth) / 2.0 :
					this->width() * 0.96 - cardWidth;
				const double y = position == QStringLiteral("center") ? height() * 0.40 - cardHeight / 2.0 :
					height() * 0.09;
				const QRectF target(x + cardWidth * (1.0 - animatedScale) / 2.0,
					y + cardHeight * (1.0 - animatedScale) / 2.0,
					cardWidth * animatedScale, cardHeight * animatedScale);
				painter.drawImage(target, event.card);
			} else if (event.kind == QStringLiteral("alert")) {
				QString title;
				if (event.text == QStringLiteral("FOLLOW")) title = QStringLiteral("НОВЫЙ ФОЛЛОВЕР");
				else if (event.text == QStringLiteral("SUB")) title = QStringLiteral("НОВАЯ ПОДПИСКА");
				else if (event.text == QStringLiteral("DONATION")) title = QStringLiteral("ДОНАТ");
				else if (event.text == QStringLiteral("RAID")) title = QStringLiteral("РЕЙД");
				if (!title.isEmpty()) {
					const QPoint center(event.location.x() * this->width(), event.location.y() * height());
					const int cardWidth = std::min(600, std::max(300, this->width() - 40));
					const QRect card(center.x() - cardWidth / 2,
						center.y() - 92 - static_cast<int>(progress * 18), cardWidth, 184);
					painter.setPen(QPen(QColor(QStringLiteral("#8be5f6")), 1));
					painter.setBrush(QColor(QStringLiteral("#0a1838")));
					painter.drawRoundedRect(card, 16, 16);
					painter.setPen(QColor(QStringLiteral("#6be4ef")));
					QFont titleFont(QStringLiteral("Segoe UI"));
					titleFont.setPixelSize(15);
					titleFont.setBold(true);
					painter.setFont(titleFont);
					painter.drawText(card.adjusted(24, 14, -24, -140), Qt::AlignLeft | Qt::AlignVCenter,
						title + (event.platform.isEmpty() ? QString{} : QStringLiteral(" · ") + event.platform));
					QFont nameFont(QStringLiteral("Segoe UI"));
					nameFont.setPixelSize(36 * effects.value(QStringLiteral("scalePercent")).toInt(70) / 100);
					nameFont.setBold(true);
					painter.setFont(nameFont);
					painter.setPen(Qt::white);
					const QRect nameBounds = card.adjusted(24, 52, -24, -74);
					painter.drawText(nameBounds, Qt::AlignLeft | Qt::AlignVCenter,
						QFontMetrics(nameFont).elidedText(event.name.isEmpty() ? QStringLiteral("Зритель") : event.name,
							Qt::ElideRight, nameBounds.width()));
					if (!event.detail.isEmpty()) {
						QFont detailsFont(QStringLiteral("Segoe UI"));
						detailsFont.setPixelSize(19);
						painter.setFont(detailsFont);
						painter.setPen(QColor(QStringLiteral("#e0e8ff")));
						const QRect detailsBounds = card.adjusted(24, 122, -24, -15);
						painter.drawText(detailsBounds, Qt::AlignLeft | Qt::AlignVCenter,
							QFontMetrics(detailsFont).elidedText(event.detail, Qt::ElideRight, detailsBounds.width()));
					}
				}
			} else if (event.particleCount > 0) {
				QFont particleFont = effectFont;
				particleFont.setPixelSize(34 * effects.value(QStringLiteral("scalePercent")).toInt(70) / 100);
				painter.setFont(particleFont);
				for (int i = 0; i < event.particleCount; ++i) {
					const quint32 seed = event.seed + static_cast<quint32>(i * 0x9e3779b9u);
					const int px = (seed % static_cast<quint32>(std::max(1, this->width())));
					const int py = ((seed >> 9) % static_cast<quint32>(std::max(1, height()))) -
						static_cast<int>(progress * 160);
					painter.drawText(QRect(px - 55, py - 20, 110, 40), Qt::AlignCenter,
						event.text == QStringLiteral("GG") ? QStringLiteral("GG") : QStringLiteral("✦"));
				}
				painter.setFont(effectFont);
			} else {
				const QPoint point(event.location.x() * this->width(), event.location.y() * height());
				if (event.text == QStringLiteral("LOW HP")) {
					painter.setPen(QPen(QColor(QStringLiteral("#ff384d")), 12));
					painter.setBrush(QColor(130, 12, 35, 36 + static_cast<int>(progress * 35)));
					painter.drawRect(rect().adjusted(9, 9, -9, -9));
					painter.setPen(QColor(QStringLiteral("#ff384d")));
				} else if (event.text == QStringLiteral("BOOM")) {
					painter.setPen(QColor(QStringLiteral("#ffd268")));
				}
				painter.drawText(QRect(point.x() - 400, point.y() - 100 - static_cast<int>(progress * 45),
					800, 200), Qt::AlignCenter, event.text);
			}
			painter.restore();
		}
	}

private:
	QJsonObject profile;
	QVector<Chat> messages;
	QVector<Effect> events;
	QVector<Effect> pendingEvents;
	QHash<QString, int> viewers;
	QString markerToast;
	qint64 markerToastUntil = 0;
	std::function<void(const QJsonObject &)> saveLayout;
	bool editing = false;
	bool dragging = false;
	bool resizing = false;
	bool draggingViewers = false;
	QPoint press;
	QPoint origin;
	QSize startSize;
};

QSpinBox *numeric(QWidget *parent, int min, int max, const QString &suffix = {})
{
	auto *field = new QSpinBox(parent);
	field->setRange(min, max);
	field->setSuffix(suffix);
	field->setKeyboardTracking(false);
	return field;
}

} // namespace

void installPresentationWorkspace(OBSBasic *mainWindow)
{
	auto *settings = new PresentationSettings(mainWindow);
	settings->setObjectName(QStringLiteral("xinkxPresentationSettings"));
	auto *runtime = mainWindow->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime"));
	auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"));
	// A Qt::Tool owned by the OBS main window is hidden automatically when OBS
	// is minimized. Keep the click-through overlay as an independent top-level
	// window while tying its lifetime to the main window explicitly.
	auto *streamer = new StreamerOverlay();
	QPointer<StreamerOverlay> guardedStreamer(streamer);
	QObject::connect(mainWindow, &QObject::destroyed, streamer, [guardedStreamer] {
		if (guardedStreamer) delete guardedStreamer.data();
	});
	streamer->setObjectName(QStringLiteral("xinkxStreamerOverlay"));
	streamer->configure(settings->snapshot());
	streamer->setLayoutWriter([settings](const QJsonObject &layout) {
		QJsonObject document = settings->snapshot();
		auto profile = document.value(QStringLiteral("streamer")).toObject();
		auto chat = profile.value(QStringLiteral("chat")).toObject();
		const auto chatLayout = layout.value(QStringLiteral("chat")).toObject();
		for (const auto &key : {QStringLiteral("offsetX"), QStringLiteral("offsetY"),
				       QStringLiteral("widthPercent"), QStringLiteral("heightPx")})
			if (chatLayout.contains(key)) chat.insert(key, chatLayout.value(key));
		profile.insert(QStringLiteral("chat"), chat);
		if (layout.contains(QStringLiteral("viewers"))) {
			auto viewers = profile.value(QStringLiteral("viewers")).toObject();
			if (viewers.isEmpty()) viewers = PresentationSettings::defaults()
				.value(QStringLiteral("streamer")).toObject().value(QStringLiteral("viewers")).toObject();
			const auto incoming = layout.value(QStringLiteral("viewers")).toObject();
			for (const auto &key : {QStringLiteral("offsetX"), QStringLiteral("offsetY")})
				if (incoming.contains(key)) viewers.insert(key, incoming.value(key));
			profile.insert(QStringLiteral("viewers"), viewers);
		}
		document.insert(QStringLiteral("streamer"), profile);
		QString error;
		settings->save(document, &error);
	});
	if (runtime) {
		runtime->setPresentation(settings->snapshot());
		QObject::connect(settings, &PresentationSettings::changed, runtime, &OverlayRuntime::setPresentation);
		QObject::connect(runtime, &OverlayRuntime::effectGenerated, streamer, &StreamerOverlay::addEffect);
		QObject::connect(runtime, &OverlayRuntime::effectsCleared, streamer, &StreamerOverlay::clearEffects);
	}
	if (gateway) {
		QObject::connect(gateway, &ChatGateway::incoming, streamer, &StreamerOverlay::addChat);
		QObject::connect(gateway, &ChatGateway::viewersReceived, streamer, [gateway, streamer](const QString &id, int count) {
			for (const auto &account : gateway->accounts())
				if (account.id == id) { streamer->setViewers(account.platform, count); break; }
		});
	}
	QObject::connect(settings, &PresentationSettings::changed, streamer, &StreamerOverlay::configure);
	if (auto *markers = mainWindow->findChild<MarkerEngine *>(QStringLiteral("xinkxMarkerEngine")))
		QObject::connect(markers, &MarkerEngine::momentMarked, streamer, &StreamerOverlay::showMarkerToast);

	auto *dock = studioDock(mainWindow);
	if (!dock) return;
	auto *panel = new QWidget(dock);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 10, 10, 10);
	auto *enable = new QCheckBox(QStringLiteral("Поверх всех окон · без реакции на мышь"), panel);
	enable->setObjectName(QStringLiteral("xinkxStreamerOverlayEnabled"));
	QObject::connect(enable, &QCheckBox::toggled, streamer, &StreamerOverlay::toggle);
	// Show automatically on every startup; it remains click-through until the
	// user explicitly enables placement mode in chat settings.
	QTimer::singleShot(0, enable, [enable] { enable->setChecked(true); });
	layout->addWidget(enable);
	auto *surface = new QComboBox(panel);
	surface->addItem(QStringLiteral("Для зрителей · OBS"), QStringLiteral("viewer"));
	surface->addItem(QStringLiteral("Для меня · поверх игры"), QStringLiteral("streamer"));
	auto *form = new QFormLayout();
	form->addRow(QStringLiteral("Профиль"), surface);
	auto *fontName = new QPushButton(QStringLiteral("Выбрать шрифт"), panel);
	form->addRow(QStringLiteral("Шрифт"), fontName);
	auto *fontSize = numeric(panel, 10, 90, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Размер"), fontSize);
	auto *fontWeight = numeric(panel, 100, 900);
	form->addRow(QStringLiteral("Насыщенность"), fontWeight);
	auto *maxMessages = numeric(panel, 1, 20);
	form->addRow(QStringLiteral("Сообщений"), maxMessages);
	auto *chatDuration = numeric(panel, 0, 300000, QStringLiteral(" мс"));
	form->addRow(QStringLiteral("Чат · время (0 = всегда)"), chatDuration);
	auto *gap = numeric(panel, 0, 64, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Между сообщениями"), gap);
	auto *padding = numeric(panel, 0, 64, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Внутренний отступ"), padding);
	auto *radius = numeric(panel, 0, 64, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Скругление"), radius);
	auto *opacity = numeric(panel, 0, 100, QStringLiteral(" %"));
	form->addRow(QStringLiteral("Фон сообщения · непрозрачность"), opacity);
	auto *panelOpacity = numeric(panel, 0, 100, QStringLiteral(" %"));
	form->addRow(QStringLiteral("Фон всей области · непрозрачность"), panelOpacity);
	auto *widthPercent = numeric(panel, 10, 100, QStringLiteral(" %"));
	form->addRow(QStringLiteral("Ширина чата"), widthPercent);
	auto *horizontal = new QComboBox(panel);
	horizontal->addItem(QStringLiteral("Слева"), QStringLiteral("left"));
	horizontal->addItem(QStringLiteral("Центр"), QStringLiteral("center"));
	horizontal->addItem(QStringLiteral("Справа"), QStringLiteral("right"));
	form->addRow(QStringLiteral("По горизонтали"), horizontal);
	auto *vertical = new QComboBox(panel);
	vertical->addItem(QStringLiteral("Сверху"), QStringLiteral("top"));
	vertical->addItem(QStringLiteral("Центр"), QStringLiteral("center"));
	vertical->addItem(QStringLiteral("Снизу"), QStringLiteral("bottom"));
	form->addRow(QStringLiteral("По вертикали"), vertical);
	const auto colorButton = [=](const QString &label) {
		auto *button = new QPushButton(panel);
		button->setToolTip(label);
		form->addRow(label, button);
		QObject::connect(button, &QPushButton::clicked, dock, [=] {
			const auto chosen = QColorDialog::getColor(QColor(button->text()), dock, label);
			if (chosen.isValid()) button->setText(chosen.name());
		});
		return button;
	};
	auto *textColor = colorButton(QStringLiteral("Цвет текста"));
	auto *nameColor = colorButton(QStringLiteral("Цвет имени"));
	auto *backgroundColor = colorButton(QStringLiteral("Фон сообщений"));
	auto *panelColor = colorButton(QStringLiteral("Фон всей области чата"));
	auto *showPlatform = new QCheckBox(QStringLiteral("Площадка"), panel);
	form->addRow(QStringLiteral("Показывать"), showPlatform);
	auto *showTime = new QCheckBox(QStringLiteral("Время сообщений"), panel);
	form->addRow(QString{}, showTime);
	auto *showViewers = new QCheckBox(QStringLiteral("Счётчик зрителей"), panel);
	form->addRow(QString{}, showViewers);
	auto *viewerHeader = new QLabel(QStringLiteral("Счётчик зрителей · отдельное оформление"), panel);
	viewerHeader->setStyleSheet(QStringLiteral("font-weight:600"));
	form->addRow(viewerHeader);
	auto *viewerHorizontal = new QComboBox(panel);
	viewerHorizontal->addItem(QStringLiteral("Слева"), QStringLiteral("left"));
	viewerHorizontal->addItem(QStringLiteral("Центр"), QStringLiteral("center"));
	viewerHorizontal->addItem(QStringLiteral("Справа"), QStringLiteral("right"));
	form->addRow(QStringLiteral("Зрители · горизонталь"), viewerHorizontal);
	auto *viewerVertical = new QComboBox(panel);
	viewerVertical->addItem(QStringLiteral("Сверху"), QStringLiteral("top"));
	viewerVertical->addItem(QStringLiteral("Центр"), QStringLiteral("center"));
	viewerVertical->addItem(QStringLiteral("Снизу"), QStringLiteral("bottom"));
	form->addRow(QStringLiteral("Зрители · вертикаль"), viewerVertical);
	auto *viewerOffsetX = numeric(panel, -3840, 3840, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Зрители · сдвиг X"), viewerOffsetX);
	auto *viewerOffsetY = numeric(panel, -2160, 2160, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Зрители · сдвиг Y"), viewerOffsetY);
	auto *viewerFontSize = numeric(panel, 10, 90, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Зрители · размер текста"), viewerFontSize);
	auto *viewerTextColor = colorButton(QStringLiteral("Зрители · цвет текста"));
	auto *viewerBgColor = colorButton(QStringLiteral("Зрители · цвет подложки"));
	auto *viewerOpacity = numeric(panel, 0, 100, QStringLiteral(" %"));
	form->addRow(QStringLiteral("Зрители · непрозрачность подложки"), viewerOpacity);
	auto *viewerPadding = numeric(panel, 0, 64, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Зрители · отступы подложки"), viewerPadding);
	auto *viewerRadius = numeric(panel, 0, 64, QStringLiteral(" px"));
	form->addRow(QStringLiteral("Зрители · скругление"), viewerRadius);
	auto *effectsEnabled = new QCheckBox(QStringLiteral("Эффекты включены"), panel);
	form->addRow(QStringLiteral("Эффекты"), effectsEnabled);
	auto *effectPosition = new QComboBox(panel);
	for (const auto &entry : {QStringLiteral("random"), QStringLiteral("top-left"),
		QStringLiteral("top-right"), QStringLiteral("center"), QStringLiteral("bottom-left"),
		QStringLiteral("bottom-right")}) effectPosition->addItem(entry, entry);
	form->addRow(QStringLiteral("Позиция эффектов"), effectPosition);
	auto *scale = numeric(panel, 30, 200, QStringLiteral(" %"));
	form->addRow(QStringLiteral("Масштаб эффектов"), scale);
	auto *maxConcurrent = numeric(panel, 1, 8);
	form->addRow(QStringLiteral("Одновременных эффектов"), maxConcurrent);
	auto *particleMin = numeric(panel, 0, 200);
	form->addRow(QStringLiteral("Частиц от"), particleMin);
	auto *particleMax = numeric(panel, 0, 200);
	form->addRow(QStringLiteral("Частиц до"), particleMax);
	auto *duration = numeric(panel, 0, 60000, QStringLiteral(" мс"));
	form->addRow(QStringLiteral("Длительность (0 = всегда)"), duration);
	auto *cooldown = numeric(panel, 0, 300000, QStringLiteral(" мс"));
	form->addRow(QStringLiteral("Общий cooldown (0 = нет)"), cooldown);
	layout->addLayout(form);
	auto *rulesTitle = new QLabel(QStringLiteral("Реакции зрителей · без JSON"), panel);
	rulesTitle->setStyleSheet(QStringLiteral("font-weight:600"));
	layout->addWidget(rulesTitle);
	auto *rulesList = new QListWidget(panel);
	rulesList->setMaximumHeight(150);
	layout->addWidget(rulesList);
	auto *ruleButtons = new QHBoxLayout();
	auto *ruleAdd = new QPushButton(QStringLiteral("Добавить"), panel);
	auto *ruleEdit = new QPushButton(QStringLiteral("Изменить"), panel);
	auto *ruleRemove = new QPushButton(QStringLiteral("Удалить"), panel);
	ruleButtons->addWidget(ruleAdd);
	ruleButtons->addWidget(ruleEdit);
	ruleButtons->addWidget(ruleRemove);
	layout->addLayout(ruleButtons);
	auto draft = std::make_shared<QJsonObject>(settings->snapshot());
	auto rules = std::make_shared<QJsonArray>(draft->value(QStringLiteral("reactions")).toArray());
	auto refreshRules = [=] {
		const int selected = rulesList->currentRow();
		rulesList->clear();
		for (const auto &entry : *rules) {
			const auto rule = entry.toObject();
			rulesList->addItem(QStringLiteral("%1 → %2 · %3 · пауза %4 с")
				.arg(rule.value(QStringLiteral("phrase")).toString(),
					rule.value(QStringLiteral("effect")).toString(),
					rule.value(QStringLiteral("enabled")).toBool() ? QStringLiteral("включено") : QStringLiteral("отключено"))
				.arg(rule.value(QStringLiteral("cooldownMs")).toInt() / 1000));
		}
		if (selected >= 0 && selected < rulesList->count()) rulesList->setCurrentRow(selected);
	};
	auto editRule = [=](int index) {
		if (index < 0 && rules->size() >= 12) return;
		QJsonObject rule = index >= 0 && index < rules->size() ? rules->at(index).toObject() : QJsonObject{};
		QDialog dialog(dock);
		dialog.setWindowTitle(index < 0 ? QStringLiteral("Новая реакция") : QStringLiteral("Редактирование реакции"));
		auto *box = new QVBoxLayout(&dialog);
		auto *fields = new QFormLayout();
		auto *phrase = new QLineEdit(rule.value(QStringLiteral("phrase")).toString(), &dialog);
		phrase->setMaxLength(40);
		fields->addRow(QStringLiteral("Слово или команда"), phrase);
		auto *effect = new QComboBox(&dialog);
		for (const auto &value : {QStringLiteral("gg"), QStringLiteral("boom"),
			QStringLiteral("low-hp"), QStringLiteral("confetti")}) effect->addItem(value, value);
		effect->setCurrentIndex(std::max(0, effect->findData(rule.value(QStringLiteral("effect")).toString())));
		fields->addRow(QStringLiteral("Эффект"), effect);
		auto *match = new QComboBox(&dialog);
		match->addItem(QStringLiteral("Содержит"), QStringLiteral("contains"));
		match->addItem(QStringLiteral("Отдельное слово"), QStringLiteral("word"));
		match->addItem(QStringLiteral("Точное совпадение"), QStringLiteral("exact"));
		match->setCurrentIndex(std::max(0, match->findData(rule.value(QStringLiteral("match")).toString())));
		fields->addRow(QStringLiteral("Совпадение"), match);
		auto *wait = numeric(&dialog, 0, 300, QStringLiteral(" с"));
		wait->setValue(rule.value(QStringLiteral("cooldownMs")).toInt() / 1000);
		fields->addRow(QStringLiteral("Пауза (0 = без ограничений)"), wait);
		auto *time = numeric(&dialog, 0, 60, QStringLiteral(" с"));
		time->setValue(rule.value(QStringLiteral("durationMs")).toInt(5000) / 1000);
		fields->addRow(QStringLiteral("Время (0 = не ограничивать)"), time);
		auto *perViewer = new QCheckBox(QStringLiteral("Отдельный cooldown у каждого зрителя"), &dialog);
		perViewer->setChecked(rule.value(QStringLiteral("perViewer")).toBool(true));
		fields->addRow(QString{}, perViewer);
		auto *enabled = new QCheckBox(QStringLiteral("Включено"), &dialog);
		enabled->setChecked(rule.value(QStringLiteral("enabled")).toBool(true));
		fields->addRow(QString{}, enabled);
		box->addLayout(fields);
		auto *commit = new QPushButton(QStringLiteral("Сохранить реакцию"), &dialog);
		box->addWidget(commit);
		QObject::connect(commit, &QPushButton::clicked, &dialog, [&] {
			if (phrase->text().trimmed().isEmpty()) return;
			rule.insert(QStringLiteral("id"), rule.value(QStringLiteral("id")).toString(
				QStringLiteral("custom-%1").arg(QRandomGenerator::global()->bounded(100000000))));
			rule.insert(QStringLiteral("phrase"), phrase->text().trimmed());
			rule.insert(QStringLiteral("effect"), effect->currentData().toString());
			rule.insert(QStringLiteral("match"), match->currentData().toString());
			rule.insert(QStringLiteral("cooldownMs"), wait->value() * 1000);
			rule.insert(QStringLiteral("durationMs"), time->value() * 1000);
			rule.insert(QStringLiteral("perViewer"), perViewer->isChecked());
			rule.insert(QStringLiteral("enabled"), enabled->isChecked());
			if (index >= 0 && index < rules->size()) rules->replace(index, rule);
			else rules->append(rule);
			refreshRules();
			dialog.accept();
		});
		dialog.exec();
	};
	QObject::connect(ruleAdd, &QPushButton::clicked, dock, [=] { editRule(-1); });
	QObject::connect(ruleEdit, &QPushButton::clicked, dock, [=] { editRule(rulesList->currentRow()); });
	QObject::connect(rulesList, &QListWidget::itemDoubleClicked, dock, [=] { editRule(rulesList->currentRow()); });
	QObject::connect(ruleRemove, &QPushButton::clicked, dock, [=] {
		if (rulesList->currentRow() < 0) return;
		rules->removeAt(rulesList->currentRow());
		refreshRules();
	});
	refreshRules();
	auto *botTitle = new QLabel(QStringLiteral("Команды чат-бота · без JSON"), panel);
	botTitle->setStyleSheet(QStringLiteral("font-weight:600"));
	layout->addWidget(botTitle);
	auto *botList = new QListWidget(panel);
	botList->setMaximumHeight(110);
	layout->addWidget(botList);
	auto *botRow = new QHBoxLayout();
	auto *botAdd = new QPushButton(QStringLiteral("Добавить команду"), panel);
	auto *botEdit = new QPushButton(QStringLiteral("Изменить"), panel);
	auto *botRemove = new QPushButton(QStringLiteral("Удалить"), panel);
	botRow->addWidget(botAdd);
	botRow->addWidget(botEdit);
	botRow->addWidget(botRemove);
	layout->addLayout(botRow);
	auto botCommands = std::make_shared<QJsonArray>(draft->value(QStringLiteral("botCommands")).toArray());
	auto refreshBot = [=] {
		const int selected = botList->currentRow();
		botList->clear();
		for (const auto &entry : *botCommands) {
			const auto item = entry.toObject();
			botList->addItem(item.value(QStringLiteral("trigger")).toString() + QStringLiteral(" → ") +
				item.value(QStringLiteral("reply")).toString().left(75));
		}
		if (selected >= 0 && selected < botList->count()) botList->setCurrentRow(selected);
	};
	auto editBot = [=](int index) {
		if (index < 0 && botCommands->size() >= 20) return;
		QJsonObject command = index >= 0 && index < botCommands->size() ? botCommands->at(index).toObject() : QJsonObject{};
		QDialog dialog(dock);
		dialog.setWindowTitle(QStringLiteral("Команда чат-бота"));
		auto *box = new QVBoxLayout(&dialog);
		auto *form = new QFormLayout();
		auto *trigger = new QLineEdit(command.value(QStringLiteral("trigger")).toString(), &dialog);
		trigger->setPlaceholderText(QStringLiteral("!discord"));
		trigger->setMaxLength(40);
		form->addRow(QStringLiteral("Команда"), trigger);
		auto *reply = new QLineEdit(command.value(QStringLiteral("reply")).toString(), &dialog);
		reply->setMaxLength(200);
		form->addRow(QStringLiteral("Ответ"), reply);
		auto *wait = numeric(&dialog, 0, 300, QStringLiteral(" с"));
		wait->setValue(command.value(QStringLiteral("cooldownMs")).toInt() / 1000);
		form->addRow(QStringLiteral("Cooldown"), wait);
		auto *perViewer = new QCheckBox(QStringLiteral("Отдельный cooldown у каждого зрителя"), &dialog);
		perViewer->setChecked(command.value(QStringLiteral("perViewer")).toBool(true));
		form->addRow(QString{}, perViewer);
		auto *enabled = new QCheckBox(QStringLiteral("Команда включена"), &dialog);
		enabled->setChecked(command.value(QStringLiteral("enabled")).toBool(true));
		form->addRow(QString{}, enabled);
		box->addLayout(form);
		auto *commit = new QPushButton(QStringLiteral("Сохранить команду"), &dialog);
		box->addWidget(commit);
		QObject::connect(commit, &QPushButton::clicked, &dialog, [&] {
			if (!trigger->text().startsWith(QLatin1Char('!')) || reply->text().trimmed().isEmpty()) return;
			command.insert(QStringLiteral("trigger"), trigger->text().trimmed());
			command.insert(QStringLiteral("reply"), reply->text().trimmed());
			command.insert(QStringLiteral("cooldownMs"), wait->value() * 1000);
			command.insert(QStringLiteral("perViewer"), perViewer->isChecked());
			command.insert(QStringLiteral("enabled"), enabled->isChecked());
			if (index >= 0 && index < botCommands->size()) botCommands->replace(index, command);
			else botCommands->append(command);
			refreshBot();
			dialog.accept();
		});
		dialog.exec();
	};
	QObject::connect(botAdd, &QPushButton::clicked, dock, [=] { editBot(-1); });
	QObject::connect(botEdit, &QPushButton::clicked, dock, [=] { editBot(botList->currentRow()); });
	QObject::connect(botList, &QListWidget::itemDoubleClicked, dock, [=] { editBot(botList->currentRow()); });
	QObject::connect(botRemove, &QPushButton::clicked, dock, [=] {
		if (botList->currentRow() < 0) return;
		botCommands->removeAt(botList->currentRow());
		refreshBot();
	});
	refreshBot();
	auto selectedFont = std::make_shared<QString>();
	auto refresh = [=] {
		const auto profile = draft->value(surface->currentData().toString()).toObject();
		const auto chat = profile.value(QStringLiteral("chat")).toObject();
		const auto effects = profile.value(QStringLiteral("effects")).toObject();
		auto viewerStyle = profile.value(QStringLiteral("viewers")).toObject();
		if (viewerStyle.isEmpty()) viewerStyle = PresentationSettings::defaults()
			.value(surface->currentData().toString()).toObject().value(QStringLiteral("viewers")).toObject();
		*selectedFont = chat.value(QStringLiteral("font")).toString();
		fontName->setText(*selectedFont);
		fontSize->setValue(chat.value(QStringLiteral("fontSize")).toInt());
		fontWeight->setValue(chat.value(QStringLiteral("fontWeight")).toInt());
		maxMessages->setValue(chat.value(QStringLiteral("maxMessages")).toInt());
		chatDuration->setValue(chat.value(QStringLiteral("messageDurationMs")).toInt());
		gap->setValue(chat.value(QStringLiteral("gap")).toInt());
		padding->setValue(chat.value(QStringLiteral("padding")).toInt());
		radius->setValue(chat.value(QStringLiteral("radius")).toInt());
		opacity->setValue(chat.value(QStringLiteral("opacity")).toInt());
		panelOpacity->setValue(chat.value(QStringLiteral("panelOpacity")).toInt());
		widthPercent->setValue(chat.value(QStringLiteral("widthPercent")).toInt());
		horizontal->setCurrentIndex(horizontal->findData(chat.value(QStringLiteral("horizontal")).toString()));
		vertical->setCurrentIndex(vertical->findData(chat.value(QStringLiteral("vertical")).toString()));
		textColor->setText(chat.value(QStringLiteral("textColor")).toString());
		nameColor->setText(chat.value(QStringLiteral("nameColor")).toString());
		backgroundColor->setText(chat.value(QStringLiteral("backgroundColor")).toString());
		panelColor->setText(chat.value(QStringLiteral("panelColor")).toString(QStringLiteral("#09152c")));
		showPlatform->setChecked(chat.value(QStringLiteral("showPlatform")).toBool());
		showTime->setChecked(chat.value(QStringLiteral("showTime")).toBool());
		showViewers->setChecked(chat.value(QStringLiteral("showViewers")).toBool());
		viewerHorizontal->setCurrentIndex(viewerHorizontal->findData(viewerStyle.value(QStringLiteral("horizontal")).toString()));
		viewerVertical->setCurrentIndex(viewerVertical->findData(viewerStyle.value(QStringLiteral("vertical")).toString()));
		viewerOffsetX->setValue(viewerStyle.value(QStringLiteral("offsetX")).toInt());
		viewerOffsetY->setValue(viewerStyle.value(QStringLiteral("offsetY")).toInt());
		viewerFontSize->setValue(viewerStyle.value(QStringLiteral("fontSize")).toInt());
		viewerTextColor->setText(viewerStyle.value(QStringLiteral("textColor")).toString());
		viewerBgColor->setText(viewerStyle.value(QStringLiteral("backgroundColor")).toString());
		viewerOpacity->setValue(viewerStyle.value(QStringLiteral("backgroundOpacity")).toInt());
		viewerPadding->setValue(viewerStyle.value(QStringLiteral("padding")).toInt());
		viewerRadius->setValue(viewerStyle.value(QStringLiteral("radius")).toInt());
		effectsEnabled->setChecked(effects.value(QStringLiteral("enabled")).toBool());
		effectPosition->setCurrentIndex(effectPosition->findData(effects.value(QStringLiteral("position")).toString()));
		scale->setValue(effects.value(QStringLiteral("scalePercent")).toInt());
		maxConcurrent->setValue(effects.value(QStringLiteral("maxConcurrent")).toInt());
		particleMin->setValue(effects.value(QStringLiteral("particleMin")).toInt());
		particleMax->setValue(effects.value(QStringLiteral("particleMax")).toInt());
		duration->setValue(effects.value(QStringLiteral("durationMs")).toInt());
		cooldown->setValue(draft->value(QStringLiteral("globalCooldownMs")).toInt());
	};
	QObject::connect(fontName, &QPushButton::clicked, dock, [=] {
		bool ok = false;
		const auto choice = QFontDialog::getFont(&ok, QFont(*selectedFont), dock);
		if (ok) { *selectedFont = choice.family(); fontName->setText(*selectedFont); }
	});
	refresh();
	auto *save = new QPushButton(QStringLiteral("Сохранить оформление"), panel);
	// Keep the save action internal: edits persist automatically, including before profile changes.
	auto *saveStatus = new QLabel(QStringLiteral("Все изменения сохраняются автоматически"), panel);
	saveStatus->setWordWrap(true);
	layout->addWidget(saveStatus);
	auto *jsonButton = new QPushButton(QStringLiteral("Редактировать все правила и стили JSON…"), panel);
	layout->addWidget(jsonButton);
	auto *info = new QLabel(QStringLiteral("Реакции: GG, !boom, low hp, !hype. В JSON можно задать каждому правилу cooldownMs:0, durationMs:0, perViewer:true; отдельно настроить viewer/streamer, позиции и количество частиц."), panel);
	info->setWordWrap(true);
	layout->addWidget(info);
	layout->addStretch();
	QObject::connect(save, &QPushButton::clicked, dock, [=] {
		*draft = settings->snapshot();
		const QString key = surface->currentData().toString();
		auto profile = draft->value(key).toObject();
		auto chat = profile.value(QStringLiteral("chat")).toObject();
		auto effects = profile.value(QStringLiteral("effects")).toObject();
		auto viewerStyle = profile.value(QStringLiteral("viewers")).toObject();
		chat.insert(QStringLiteral("font"), *selectedFont);
		chat.insert(QStringLiteral("fontSize"), fontSize->value());
		chat.insert(QStringLiteral("fontWeight"), fontWeight->value());
		chat.insert(QStringLiteral("maxMessages"), maxMessages->value());
		chat.insert(QStringLiteral("messageDurationMs"), chatDuration->value());
		chat.insert(QStringLiteral("gap"), gap->value());
		chat.insert(QStringLiteral("padding"), padding->value());
		chat.insert(QStringLiteral("radius"), radius->value());
		chat.insert(QStringLiteral("opacity"), opacity->value());
		chat.insert(QStringLiteral("panelOpacity"), panelOpacity->value());
		chat.insert(QStringLiteral("widthPercent"), widthPercent->value());
		chat.insert(QStringLiteral("horizontal"), horizontal->currentData().toString());
		chat.insert(QStringLiteral("vertical"), vertical->currentData().toString());
		chat.insert(QStringLiteral("textColor"), textColor->text());
		chat.insert(QStringLiteral("nameColor"), nameColor->text());
		chat.insert(QStringLiteral("backgroundColor"), backgroundColor->text());
		chat.insert(QStringLiteral("panelColor"), panelColor->text());
		chat.insert(QStringLiteral("showPlatform"), showPlatform->isChecked());
		chat.insert(QStringLiteral("showTime"), showTime->isChecked());
		chat.insert(QStringLiteral("showViewers"), showViewers->isChecked());
		viewerStyle.insert(QStringLiteral("horizontal"), viewerHorizontal->currentData().toString());
		viewerStyle.insert(QStringLiteral("vertical"), viewerVertical->currentData().toString());
		viewerStyle.insert(QStringLiteral("offsetX"), viewerOffsetX->value());
		viewerStyle.insert(QStringLiteral("offsetY"), viewerOffsetY->value());
		viewerStyle.insert(QStringLiteral("fontSize"), viewerFontSize->value());
		viewerStyle.insert(QStringLiteral("textColor"), viewerTextColor->text());
		viewerStyle.insert(QStringLiteral("backgroundColor"), viewerBgColor->text());
		viewerStyle.insert(QStringLiteral("backgroundOpacity"), viewerOpacity->value());
		viewerStyle.insert(QStringLiteral("padding"), viewerPadding->value());
		viewerStyle.insert(QStringLiteral("radius"), viewerRadius->value());
		effects.insert(QStringLiteral("enabled"), effectsEnabled->isChecked());
		effects.insert(QStringLiteral("position"), effectPosition->currentData().toString());
		effects.insert(QStringLiteral("scalePercent"), scale->value());
		effects.insert(QStringLiteral("maxConcurrent"), maxConcurrent->value());
		effects.insert(QStringLiteral("particleMin"), particleMin->value());
		effects.insert(QStringLiteral("particleMax"), particleMax->value());
		effects.insert(QStringLiteral("durationMs"), duration->value());
		profile.insert(QStringLiteral("chat"), chat);
		profile.insert(QStringLiteral("viewers"), viewerStyle);
		profile.insert(QStringLiteral("effects"), effects);
		draft->insert(key, profile);
		draft->insert(QStringLiteral("globalCooldownMs"), cooldown->value());
		draft->insert(QStringLiteral("reactions"), *rules);
		draft->insert(QStringLiteral("botCommands"), *botCommands);
		QString error;
		if (!settings->save(*draft, &error))
			saveStatus->setText(QStringLiteral("Не сохранено: ") + error);
		else
			saveStatus->setText(QStringLiteral("Сохранено автоматически"));
	});
	auto *saveDelay = new QTimer(panel);
	saveDelay->setSingleShot(true);
	saveDelay->setInterval(350);
	QObject::connect(saveDelay, &QTimer::timeout, save, &QPushButton::click);
	const auto scheduleSave = [saveDelay] { saveDelay->start(); };
	for (auto *field : panel->findChildren<QSpinBox *>())
		QObject::connect(field, &QSpinBox::valueChanged, panel, scheduleSave);
	for (auto *field : panel->findChildren<QCheckBox *>())
		QObject::connect(field, &QCheckBox::toggled, panel, scheduleSave);
	for (auto *field : {horizontal, vertical, viewerHorizontal, viewerVertical, effectPosition})
		QObject::connect(field, &QComboBox::currentIndexChanged, panel, scheduleSave);
	for (auto *button : {fontName, textColor, nameColor, backgroundColor, panelColor, viewerTextColor, viewerBgColor,
			      ruleAdd, ruleEdit, ruleRemove, botAdd, botEdit, botRemove})
		QObject::connect(button, &QPushButton::clicked, panel, scheduleSave);
	QObject::connect(rulesList, &QListWidget::itemDoubleClicked, panel, scheduleSave);
	QObject::connect(botList, &QListWidget::itemDoubleClicked, panel, scheduleSave);
	auto previousSurface = std::make_shared<QString>(surface->currentData().toString());
	QObject::connect(surface, &QComboBox::currentIndexChanged, panel, [=] {
		const QString next = surface->currentData().toString();
		if (saveDelay->isActive()) {
			// Persist the fields that are still displaying the previous profile.
			const QSignalBlocker blocker(surface);
			surface->setCurrentIndex(surface->findData(*previousSurface));
			saveDelay->stop();
			save->click();
			surface->setCurrentIndex(surface->findData(next));
		}
		*previousSurface = next;
		refresh();
	});
	QObject::connect(jsonButton, &QPushButton::clicked, dock, [=] {
		QDialog dialog(dock);
		dialog.setWindowTitle(QStringLiteral("XINKX · JSON оформление и реакции"));
		dialog.resize(860, 690);
		auto *box = new QVBoxLayout(&dialog);
		auto *editor = new QPlainTextEdit(&dialog);
		editor->setPlainText(QString::fromUtf8(QJsonDocument(settings->snapshot()).toJson(QJsonDocument::Indented)));
		box->addWidget(editor);
		auto *line = new QHBoxLayout();
		auto *restore = new QPushButton(QStringLiteral("По умолчанию"), &dialog);
		auto *commit = new QPushButton(QStringLiteral("Проверить и сохранить"), &dialog);
		line->addWidget(restore);
		line->addStretch();
		line->addWidget(commit);
		box->addLayout(line);
		QObject::connect(restore, &QPushButton::clicked, &dialog, [=] {
			editor->setPlainText(QString::fromUtf8(QJsonDocument(PresentationSettings::defaults()).toJson(QJsonDocument::Indented)));
		});
		QObject::connect(commit, &QPushButton::clicked, &dialog, [&] {
			QJsonParseError parse;
			const auto document = QJsonDocument::fromJson(editor->toPlainText().toUtf8(), &parse);
			QString error;
			if (!document.isObject() || !settings->save(document.object(), &error)) {
				QMessageBox::warning(&dialog, QStringLiteral("JSON"), document.isObject() ? error : parse.errorString());
				return;
			}
			*draft = settings->snapshot();
			*rules = draft->value(QStringLiteral("reactions")).toArray();
			refreshRules();
			*botCommands = draft->value(QStringLiteral("botCommands")).toArray();
			refreshBot();
			refresh();
			dialog.accept();
		});
		dialog.exec();
	});
	auto *dialog = new QDialog(mainWindow);
	dialog->setObjectName(QStringLiteral("xinkxAppearanceSettingsDialog"));
	dialog->setWindowTitle(QStringLiteral("XINKX · Чат, реакции и боты"));
	dialog->resize(760, 740);
	auto *dialogLayout = new QVBoxLayout(dialog);
	auto *scroll = new QScrollArea(dialog);
	scroll->setWidgetResizable(true);
	guardWheelFields(panel);
	scroll->setWidget(panel);
	dialogLayout->addWidget(scroll);
}

} // namespace xinkx
