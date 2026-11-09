#include "PresentationSettings.hpp"

#include "StreamAccounts.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPair>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <tuple>

namespace xinkx {

namespace {

QJsonObject profile(bool streamer)
{
	return {
		{QStringLiteral("chat"), QJsonObject{
			{QStringLiteral("font"), QStringLiteral("Segoe UI")},
			{QStringLiteral("fontSize"), streamer ? 20 : 26},
			{QStringLiteral("fontWeight"), 600},
			{QStringLiteral("maxMessages"), 8},
			{QStringLiteral("messageDurationMs"), 12000},
			{QStringLiteral("gap"), 7},
			{QStringLiteral("padding"), 10},
			{QStringLiteral("radius"), 6},
			{QStringLiteral("opacity"), 88},
			{QStringLiteral("widthPercent"), 60},
			{QStringLiteral("horizontal"), QStringLiteral("left")},
			{QStringLiteral("vertical"), QStringLiteral("bottom")},
			{QStringLiteral("textColor"), QStringLiteral("#f6faff")},
			{QStringLiteral("nameColor"), QStringLiteral("#8de9fb")},
			{QStringLiteral("backgroundColor"), QStringLiteral("#09152c")},
			{QStringLiteral("showPlatform"), true},
			{QStringLiteral("showTime"), false},
			{QStringLiteral("showViewers"), true},
		}},
		{QStringLiteral("effects"), QJsonObject{
			{QStringLiteral("enabled"), true},
			{QStringLiteral("position"), QStringLiteral("random")},
			{QStringLiteral("scalePercent"), streamer ? 70 : 100},
			{QStringLiteral("particleMin"), streamer ? 12 : 25},
			{QStringLiteral("particleMax"), streamer ? 30 : 65},
			{QStringLiteral("durationMs"), 5000},
			{QStringLiteral("maxConcurrent"), 2},
		}},
	};
}

QJsonObject rule(const QString &id, const QString &phrase, const QString &mode,
		 const QString &effect, int cooldown, int duration)
{
	return {{QStringLiteral("id"), id}, {QStringLiteral("phrase"), phrase},
		{QStringLiteral("match"), mode}, {QStringLiteral("effect"), effect},
		{QStringLiteral("cooldownMs"), cooldown}, {QStringLiteral("durationMs"), duration},
		{QStringLiteral("perViewer"), true}, {QStringLiteral("enabled"), true}};
}

bool within(const QJsonObject &obj, const QString &key, int low, int high)
{
	const auto value = obj.value(key);
	return value.isDouble() && value.toDouble() >= low && value.toDouble() <= high &&
		static_cast<double>(value.toInt()) == value.toDouble();
}

bool oneOf(const QString &value, const QStringList &choices)
{
	return choices.contains(value);
}

} // namespace

QJsonObject PresentationSettings::defaults()
{
	return {
		{QStringLiteral("version"), 1},
		{QStringLiteral("viewer"), profile(false)},
		{QStringLiteral("streamer"), profile(true)},
		{QStringLiteral("globalCooldownMs"), 500},
		{QStringLiteral("botCommands"), QJsonArray{}},
		{QStringLiteral("reactions"), QJsonArray{
			rule(QStringLiteral("gg"), QStringLiteral("gg"), QStringLiteral("word"),
			     QStringLiteral("gg"), 20000, 4700),
			rule(QStringLiteral("boom"), QStringLiteral("!boom"), QStringLiteral("exact"),
			     QStringLiteral("boom"), 12000, 2700),
			rule(QStringLiteral("lowhp"), QStringLiteral("low hp"), QStringLiteral("contains"),
			     QStringLiteral("low-hp"), 30000, 4000),
			rule(QStringLiteral("hype"), QStringLiteral("!hype"), QStringLiteral("exact"),
			     QStringLiteral("confetti"), 30000, 5500),
		}},
	};
}

QString PresentationSettings::path() const
{
	const auto settings = studioSettingsPath();
	return settings.isEmpty() ? QString{} : QFileInfo(settings).absoluteDir().filePath(QStringLiteral("presentation.json"));
}

PresentationSettings::PresentationSettings(QObject *parent) : QObject(parent), document(defaults())
{
	QFile file(path());
	if (file.open(QIODevice::ReadOnly) && file.size() <= 65536) {
		const auto parsed = QJsonDocument::fromJson(file.readAll());
		QString error;
		if (parsed.isObject() && validate(parsed.object(), &error)) document = parsed.object();
	}
}

bool PresentationSettings::validate(const QJsonObject &value, QString *error)
{
	const auto invalid = [error](const QString &reason) {
		if (error) *error = reason;
		return false;
	};
	if (value.value(QStringLiteral("version")).toInt() != 1) return invalid(QStringLiteral("Нужна версия JSON 1"));
	for (const auto &surface : {QStringLiteral("viewer"), QStringLiteral("streamer")}) {
		const auto entry = value.value(surface).toObject();
		const auto chat = entry.value(QStringLiteral("chat")).toObject();
		const auto effects = entry.value(QStringLiteral("effects")).toObject();
		if (chat.isEmpty() || effects.isEmpty()) return invalid(surface + QStringLiteral(": нет настроек"));
		for (const auto &key : {QStringLiteral("fontSize"), QStringLiteral("fontWeight"),
				       QStringLiteral("maxMessages"), QStringLiteral("messageDurationMs"), QStringLiteral("gap"),
				       QStringLiteral("padding"), QStringLiteral("radius"),
				       QStringLiteral("opacity"), QStringLiteral("widthPercent")}) {
			const auto limits = key == QStringLiteral("fontSize") ? QPair(10, 90)
				: key == QStringLiteral("fontWeight") ? QPair(100, 900)
				: key == QStringLiteral("maxMessages") ? QPair(1, 20)
				: key == QStringLiteral("messageDurationMs") ? QPair(0, 300000)
				: key == QStringLiteral("opacity") || key == QStringLiteral("widthPercent") ? QPair(10, 100)
				: QPair(0, 64);
			if (!within(chat, key, limits.first, limits.second)) return invalid(surface + QStringLiteral("/chat/") + key);
		}
		if (!chat.value(QStringLiteral("font")).isString() ||
		    chat.value(QStringLiteral("font")).toString().size() > 80 ||
		    !oneOf(chat.value(QStringLiteral("horizontal")).toString(),
			   {QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")}) ||
		    !oneOf(chat.value(QStringLiteral("vertical")).toString(),
			   {QStringLiteral("top"), QStringLiteral("center"), QStringLiteral("bottom")}))
			return invalid(surface + QStringLiteral("/chat: неверное оформление"));
		for (const auto &key : {QStringLiteral("textColor"), QStringLiteral("nameColor"),
				       QStringLiteral("backgroundColor")}) {
			if (!QRegularExpression(QStringLiteral("^#[a-fA-F0-9]{6}$"))
					.match(chat.value(key).toString()).hasMatch()) return invalid(surface + QStringLiteral("/chat/") + key);
		}
		for (const auto &key : {QStringLiteral("showPlatform"), QStringLiteral("showTime"), QStringLiteral("showViewers")}) {
			if (!chat.value(key).isBool()) return invalid(surface + QStringLiteral("/chat/") + key);
		}
		if (!effects.value(QStringLiteral("enabled")).isBool() ||
		    !oneOf(effects.value(QStringLiteral("position")).toString(),
			   {QStringLiteral("random"), QStringLiteral("top-left"), QStringLiteral("top-right"),
			    QStringLiteral("center"), QStringLiteral("bottom-left"), QStringLiteral("bottom-right")}))
			return invalid(surface + QStringLiteral("/effects/position"));
		for (const auto &spec : {std::tuple{QStringLiteral("scalePercent"), 30, 200},
				     std::tuple{QStringLiteral("particleMin"), 0, 200},
				     std::tuple{QStringLiteral("particleMax"), 0, 200},
				     std::tuple{QStringLiteral("durationMs"), 0, 60000},
				     std::tuple{QStringLiteral("maxConcurrent"), 1, 8}}) {
			const auto &[key, low, high] = spec;
			if (!within(effects, key, low, high)) return invalid(surface + QStringLiteral("/effects/") + key);
		}
		if (effects.value(QStringLiteral("particleMin")).toInt() > effects.value(QStringLiteral("particleMax")).toInt())
			return invalid(surface + QStringLiteral(": минимальное количество частиц больше максимального"));
	}
	if (!within(value, QStringLiteral("globalCooldownMs"), 0, 300000))
		return invalid(QStringLiteral("globalCooldownMs: 0–300000"));
	const auto rules = value.value(QStringLiteral("reactions")).toArray();
	if (rules.size() > 12) return invalid(QStringLiteral("Не больше 12 реакций"));
	QSet<QString> used;
	for (const auto &entry : rules) {
		const auto item = entry.toObject();
		const QString id = item.value(QStringLiteral("id")).toString();
		const QString phrase = item.value(QStringLiteral("phrase")).toString();
		if (!QRegularExpression(QStringLiteral("^[a-z][a-z0-9-]{0,29}$")).match(id).hasMatch() ||
		    used.contains(id) || phrase.trimmed().isEmpty() || phrase.size() > 40 ||
		    !oneOf(item.value(QStringLiteral("match")).toString(),
			   {QStringLiteral("exact"), QStringLiteral("word"), QStringLiteral("contains")}) ||
		    !oneOf(item.value(QStringLiteral("effect")).toString(),
			   {QStringLiteral("gg"), QStringLiteral("boom"), QStringLiteral("low-hp"),
			    QStringLiteral("confetti")}) ||
		    !within(item, QStringLiteral("cooldownMs"), 0, 300000) ||
		    !within(item, QStringLiteral("durationMs"), 0, 60000) ||
		    !item.value(QStringLiteral("enabled")).isBool() ||
		    !item.value(QStringLiteral("perViewer")).isBool())
			return invalid(QStringLiteral("Неверная реакция: ") + id.left(30));
		used.insert(id);
	}
	const auto commands = value.value(QStringLiteral("botCommands")).toArray();
	if (commands.size() > 20) return invalid(QStringLiteral("Не больше 20 команд бота"));
	for (const auto &entry : commands) {
		const auto item = entry.toObject();
		const auto trigger = item.value(QStringLiteral("trigger")).toString();
		const auto reply = item.value(QStringLiteral("reply")).toString();
		if (!entry.isObject() || trigger.isEmpty() || !trigger.startsWith(QLatin1Char('!')) ||
		    trigger.size() > 40 || reply.trimmed().isEmpty() || reply.size() > 200 ||
		    !within(item, QStringLiteral("cooldownMs"), 0, 300000) ||
		    !item.value(QStringLiteral("enabled")).isBool() || !item.value(QStringLiteral("perViewer")).isBool())
			return invalid(QStringLiteral("Неверная команда бота: ") + trigger.left(40));
	}
	return true;
}

bool PresentationSettings::save(const QJsonObject &value, QString *error)
{
	if (!validate(value, error)) return false;
	const auto location = path();
	if (location.isEmpty()) return false;
	QSaveFile file(location);
	if (!file.open(QIODevice::WriteOnly)) {
		if (error) *error = file.errorString();
		return false;
	}
	const auto payload = QJsonDocument(value).toJson(QJsonDocument::Indented);
	if (payload.size() > 65536 || file.write(payload) != payload.size() || !file.commit()) {
		if (error) *error = QStringLiteral("Не удалось сохранить presentation.json");
		return false;
	}
	document = value;
	emit changed(document);
	return true;
}

} // namespace xinkx
