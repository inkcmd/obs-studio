#include "ModerationEngine.hpp"

#include "StreamAccounts.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>

namespace xinkx {

namespace {

constexpr int kViewerLimit = 1024;
constexpr qint64 kViewerExpiryMs = 120000;
constexpr qint64 kRepeatWindowMs = 15000;

QStringList normalizedPhrases(const QStringList &phrases)
{
	QStringList result;
	QSet<QString> seen;
	for (const auto &phrase : phrases) {
		const QString value = phrase.simplified().toCaseFolded();
		if (value.isEmpty() || seen.contains(value)) continue;
		seen.insert(value);
		result.append(value);
	}
	return result;
}

bool isLink(const QString &message)
{
	// Lightweight local filter, not a URL parser or a remote moderation API.
	static const QRegularExpression pattern(QStringLiteral(
		R"((?:https?://|www\.|\b[a-z0-9-]+\.(?:com|net|org|gg|io|ru|am|tv|me|xyz)\b))"),
		QRegularExpression::CaseInsensitiveOption);
	return pattern.match(message).hasMatch();
}

} // namespace

ModerationEngine::ModerationEngine(QObject *parent) : QObject(parent)
{
	setObjectName(QStringLiteral("xinkxModerationEngine"));
	const auto settings = studioSettingsPath();
	if (!settings.isEmpty())
		path = QFileInfo(settings).absoluteDir().filePath(QStringLiteral("moderation.json"));
	load();
}

bool ModerationEngine::validate(const Configuration &value, QString *error)
{
	if (value.maxMessages < 2 || value.maxMessages > 30 ||
	    value.windowSeconds < 3 || value.windowSeconds > 120 ||
	    value.blockedPhrases.size() > 100) {
		if (error) *error = QStringLiteral("Недопустимые ограничения модерации");
		return false;
	}
	for (const auto &phrase : value.blockedPhrases) {
		if (phrase.size() > 80 || phrase.contains(QLatin1Char('\0')) ||
		    phrase.contains(QLatin1Char('\n')) || phrase.contains(QLatin1Char('\r'))) {
			if (error) *error = QStringLiteral("Стоп-фраза должна быть не длиннее 80 символов и занимать одну строку");
			return false;
		}
	}
	return true;
}

void ModerationEngine::load()
{
	if (path.isEmpty()) return;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024) return;
	QJsonParseError error{};
	const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) return;
	const QJsonObject obj = doc.object();
	if (obj.value(QStringLiteral("version")).toInt() != 1) return;
	Configuration loaded;
	loaded.enabled = obj.value(QStringLiteral("enabled")).toBool(false);
	loaded.blockLinks = obj.value(QStringLiteral("blockLinks")).toBool(false);
	loaded.blockRepeats = obj.value(QStringLiteral("blockRepeats")).toBool(true);
	loaded.blockFlood = obj.value(QStringLiteral("blockFlood")).toBool(true);
	loaded.maxMessages = obj.value(QStringLiteral("maxMessages")).toInt(5);
	loaded.windowSeconds = obj.value(QStringLiteral("windowSeconds")).toInt(10);
	for (const auto &item : obj.value(QStringLiteral("blockedPhrases")).toArray()) {
		if (!item.isString()) return;
		loaded.blockedPhrases.append(item.toString());
	}
	if (!validate(loaded, nullptr)) return;
	loaded.blockedPhrases = normalizedPhrases(loaded.blockedPhrases);
	config = loaded;
}

bool ModerationEngine::updateConfiguration(const Configuration &value, QString *error)
{
	if (!validate(value, error)) return false;
	if (path.isEmpty()) {
		if (error) *error = QStringLiteral("Каталог конфигурации XINKX недоступен");
		return false;
	}
	Configuration normalized = value;
	normalized.blockedPhrases = normalizedPhrases(value.blockedPhrases);
	const QJsonObject content{{QStringLiteral("version"), 1},
		{QStringLiteral("enabled"), normalized.enabled},
		{QStringLiteral("blockLinks"), normalized.blockLinks},
		{QStringLiteral("blockRepeats"), normalized.blockRepeats},
		{QStringLiteral("blockFlood"), normalized.blockFlood},
		{QStringLiteral("maxMessages"), normalized.maxMessages},
		{QStringLiteral("windowSeconds"), normalized.windowSeconds},
		{QStringLiteral("blockedPhrases"), QJsonArray::fromStringList(normalized.blockedPhrases)}};
	const QByteArray json = QJsonDocument(content).toJson(QJsonDocument::Indented);
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
		if (error) *error = QStringLiteral("Не удалось сохранить moderation.json");
		return false;
	}
	config = normalized;
	viewers.clear(); // New limits take effect immediately.
	emit configurationChanged();
	return true;
}

void ModerationEngine::notifyCounters()
{
	if (notifyPending) return;
	notifyPending = true;
	QTimer::singleShot(200, this, [this] {
		notifyPending = false;
		emit countersChanged();
	});
}

void ModerationEngine::pruneViewers(qint64 now)
{
	for (auto it = viewers.begin(); it != viewers.end();) {
		if (now - it.value().lastSeen > kViewerExpiryMs) it = viewers.erase(it);
		else ++it;
	}
	while (viewers.size() >= kViewerLimit) viewers.erase(viewers.begin());
}

bool ModerationEngine::allowChat(const QString &platform, const QString &accountId,
				 const QString &viewer, const QString &message, qint64 nowMs)
{
	if (!config.enabled) return true;
	const qint64 now = nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
	const QString text = message.simplified().toCaseFolded();
	for (const auto &phrase : config.blockedPhrases) {
		if (text.contains(phrase)) {
			++stats.phrases;
			notifyCounters();
			return false;
		}
	}
	if (config.blockLinks && isLink(message)) {
		++stats.links;
		notifyCounters();
		return false;
	}
	const QString name = viewer.simplified().toCaseFolded();
	if (!name.isEmpty() && (config.blockRepeats || config.blockFlood)) {
		const QString key = platform.toCaseFolded() + QLatin1Char(':') + accountId +
			QLatin1Char(':') + name;
		if (!viewers.contains(key) && viewers.size() >= kViewerLimit) pruneViewers(now);
		ViewerState &state = viewers[key];
		state.lastSeen = now;
		if (config.blockRepeats && state.previous == text &&
		    !text.isEmpty() && now - state.previousAt < kRepeatWindowMs) {
			state.previousAt = now;
			++stats.repeats;
			notifyCounters();
			return false;
		}
		state.previous = text;
		state.previousAt = now;
		if (config.blockFlood) {
			const qint64 threshold = now - static_cast<qint64>(config.windowSeconds) * 1000;
			while (!state.messages.isEmpty() && state.messages.head() <= threshold)
				state.messages.dequeue();
			if (state.messages.size() >= config.maxMessages) {
				++stats.flood;
				notifyCounters();
				return false;
			}
			state.messages.enqueue(now);
		}
	}
	++stats.passed;
	notifyCounters();
	return true;
}

} // namespace xinkx
