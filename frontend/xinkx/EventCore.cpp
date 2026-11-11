#include "EventCore.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace xinkx {

namespace {

constexpr int kQueueLimit = 2048;
constexpr int kHistoryLimit = 1000;
constexpr int kDedupLimit = 4096;
constexpr int kDeliveryBatch = 64;

QString cleaned(QString value, int length)
{
	value = value.trimmed().left(length);
	value.remove(QLatin1Char('\0'));
	return value;
}

QJsonObject serialize(const StudioEvent &event)
{
	return {{QStringLiteral("id"), event.id},
		{QStringLiteral("externalId"), event.externalId},
		{QStringLiteral("sourceId"), event.sourceId},
		{QStringLiteral("platform"), event.platform},
		{QStringLiteral("accountId"), event.accountId},
		{QStringLiteral("type"), event.type},
		{QStringLiteral("subtype"), event.subtype},
		{QStringLiteral("paid"), event.paid},
		{QStringLiteral("name"), event.name},
		{QStringLiteral("message"), event.message},
		{QStringLiteral("amount"), event.amount},
		{QStringLiteral("receivedAt"), static_cast<double>(event.receivedAt)}};
}

StudioEvent deserialize(const QJsonObject &object)
{
	StudioEvent event;
	event.id = object.value(QStringLiteral("id")).toString();
	event.externalId = object.value(QStringLiteral("externalId")).toString();
	event.sourceId = object.value(QStringLiteral("sourceId")).toString();
	event.platform = object.value(QStringLiteral("platform")).toString();
	event.accountId = object.value(QStringLiteral("accountId")).toString();
	event.type = object.value(QStringLiteral("type")).toString();
	event.subtype = object.value(QStringLiteral("subtype")).toString();
	event.paid = object.value(QStringLiteral("paid")).toBool();
	event.name = object.value(QStringLiteral("name")).toString();
	event.message = object.value(QStringLiteral("message")).toString();
	event.amount = object.value(QStringLiteral("amount")).toString();
	event.receivedAt = static_cast<qint64>(object.value(QStringLiteral("receivedAt")).toDouble());
	return event;
}

} // namespace

EventCore::EventCore(const QString &journalPath, QObject *parent) : QObject(parent), path(journalPath)
{
	setObjectName(QStringLiteral("xinkxEventCore"));
	saveTimer.setSingleShot(true);
	saveTimer.setInterval(1500);
	connect(&saveTimer, &QTimer::timeout, this, &EventCore::flush);
	load();
}

EventCore::~EventCore()
{
	flush();
}

bool EventCore::sanitize(StudioEvent &event)
{
	static const QRegularExpression typeFormat(QStringLiteral("^[a-z][a-z0-9_.-]{0,39}$"));
	event.platform = cleaned(event.platform.toLower(), 32);
	event.type = cleaned(event.type.toLower(), 40);
	event.sourceId = cleaned(event.sourceId, 128);
	event.accountId = cleaned(event.accountId, 128);
	event.externalId = cleaned(event.externalId, 180);
	event.subtype = cleaned(event.subtype, 64);
	event.name = cleaned(event.name, 80);
	event.message = cleaned(event.message, 500);
	event.amount = cleaned(event.amount, 60);
	if (event.platform.isEmpty() || !typeFormat.match(event.type).hasMatch()) return false;
	if (event.type == QStringLiteral("chat") && event.message.isEmpty()) return false;
	if (event.sourceId.isEmpty()) event.sourceId = event.platform;
	if (event.receivedAt <= 0) event.receivedAt = QDateTime::currentMSecsSinceEpoch();
	if (event.id.isEmpty()) event.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	return true;
}

QString EventCore::dedupKey(const StudioEvent &event)
{
	if (event.externalId.isEmpty()) return {};
	// Length prefixes keep arbitrary (but bounded) provider IDs unambiguous.
	// sourceId and accountId survive UI/provider renames. A custom donation
	// feed keeps its stable UUID even when the user changes its display name.
	const QStringList parts{event.accountId, event.sourceId, event.externalId};
	QString key;
	for (const auto &part : parts) key += QString::number(part.size()) + QLatin1Char(':') + part;
	return key;
}

void EventCore::remember(const StudioEvent &event)
{
	const auto key = dedupKey(event);
	if (!key.isEmpty() && !seen.contains(key)) {
		seen.insert(key);
		seenOrder.enqueue(key);
		while (seenOrder.size() > kDedupLimit) seen.remove(seenOrder.dequeue());
	}
	history.append(event);
	while (history.size() > kHistoryLimit) history.removeFirst();
	dirty = true;
	saveTimer.start();
}

bool EventCore::publish(StudioEvent event)
{
	// Callers (provider adapters) enter on the OBS/UI thread. This explicit
	// check prevents accidental synchronous UI delivery on a curl worker.
	if (QThread::currentThread() != thread() || !sanitize(event)) return false;
	const auto key = dedupKey(event);
	if (!key.isEmpty() && seen.contains(key)) {
		++duplicateCount;
		emit metricsChanged();
		return false;
	}
	if (pending.size() >= kQueueLimit) {
		if (event.type != QStringLiteral("chat")) {
			// Prefer a paid or follow event over pending chat during a burst.
			const auto chat = std::find_if(pending.cbegin(), pending.cend(), [](const StudioEvent &item) {
				return item.type == QStringLiteral("chat");
			});
			if (chat != pending.cend()) pending.removeAt(static_cast<int>(chat - pending.cbegin()));
			else {
				++overflowCount;
				emit metricsChanged();
				return false;
			}
		} else {
			++overflowCount;
			emit metricsChanged();
			return false;
		}
		++overflowCount;
	}
	remember(event);
	pending.enqueue(std::move(event));
	++acceptedCount;
	if (!draining) {
		draining = true;
		QTimer::singleShot(0, this, &EventCore::drain);
	}
	emit metricsChanged();
	return true;
}

void EventCore::drain()
{
	int count = 0;
	while (!pending.isEmpty() && count++ < kDeliveryBatch) {
		const auto event = pending.dequeue();
		emit delivered(event);
	}
	if (!pending.isEmpty()) QTimer::singleShot(0, this, &EventCore::drain);
	else draining = false;
	emit metricsChanged();
}

EventMetrics EventCore::metrics() const
{
	// Both collections are hard-capped well below INT_MAX.
	return {acceptedCount, duplicateCount, overflowCount,
		static_cast<int>(pending.size()), static_cast<int>(history.size())};
}

QList<StudioEvent> EventCore::recent(int limit) const
{
	const qsizetype count = std::clamp<qsizetype>(limit, 0, kHistoryLimit);
	return history.mid(std::max<qsizetype>(0, history.size() - count));
}

void EventCore::load()
{
	if (path.isEmpty()) return;
	QFile file(path);
	if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
	while (!file.atEnd()) {
		const QByteArray line = file.readLine(8192);
		if (line.size() >= 8192) { file.readLine(); continue; }
		QJsonParseError error{};
		const auto json = QJsonDocument::fromJson(line, &error);
		if (error.error != QJsonParseError::NoError || !json.isObject()) continue;
		auto event = deserialize(json.object());
		if (!sanitize(event)) continue;
		const auto key = dedupKey(event);
		if (!key.isEmpty() && seen.contains(key)) continue;
		remember(event);
	}
	saveTimer.stop();
	dirty = false; // Reading history must never re-emit events or rewrite the file.
}

void EventCore::flush()
{
	if (!dirty || path.isEmpty()) return;
	const QFileInfo info(path);
	if (!QDir().mkpath(info.absolutePath())) {
		emit storageWarning(QStringLiteral("Не удалось создать каталог журнала событий"));
		return;
	}
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		emit storageWarning(QStringLiteral("Не удалось открыть журнал событий"));
		return;
	}
	for (const auto &event : history) {
		const auto data = QJsonDocument(serialize(event)).toJson(QJsonDocument::Compact) + '\n';
		if (file.write(data) != data.size()) {
			file.cancelWriting();
			emit storageWarning(QStringLiteral("Не удалось записать журнал событий"));
			return;
		}
	}
	if (!file.commit()) {
		emit storageWarning(QStringLiteral("Не удалось сохранить журнал событий"));
		return;
	}
	dirty = false;
}

} // namespace xinkx
