#include "MarkerEngine.hpp"

#include "EngineRegistry.hpp"
#include "RecordingEngine.hpp"
#include "StreamAccounts.hpp"
#include "StudioWorkspace.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace xinkx {

namespace {

const QStringList kAutoTypes{QStringLiteral("donation"), QStringLiteral("sub"),
	QStringLiteral("raid"), QStringLiteral("follow")};

QString sanitizedLabel(QString value)
{
	value = value.trimmed();
	value.replace(QLatin1Char('\n'), QLatin1Char(' '));
	value.replace(QLatin1Char('\r'), QLatin1Char(' '));
	value.remove(QLatin1Char('\0'));
	return value.left(120);
}

QString quotedCsv(QString value)
{
	if (!value.isEmpty() && QStringLiteral("=+-@").contains(value.at(0)))
		value.prepend(QLatin1Char(0x27)); // Avoid spreadsheet formula evaluation.
	value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
	return QLatin1Char('"') + value + QLatin1Char('"');
}

} // namespace

MarkerEngine::MarkerEngine(QObject *parent) : EngineExtension(parent)
{
	setObjectName(QStringLiteral("xinkxMarkerEngine"));
	const auto settings = studioSettingsPath();
	if (!settings.isEmpty())
		path = QFileInfo(settings).absoluteDir().filePath(QStringLiteral("markers.json"));
	session.start();
	saveTimer.setSingleShot(true);
	saveTimer.setInterval(500);
	connect(&saveTimer, &QTimer::timeout, this, [this] { flush(); });
	load();
}

MarkerEngine::~MarkerEngine()
{
	stop();
}

void MarkerEngine::stop()
{
	if (saveTimer.isActive()) flush();
}

void MarkerEngine::load()
{
	if (path.isEmpty()) return;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return;
	if (file.size() > 32 * 1024 * 1024) return;
	QJsonParseError error{};
	const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) return;
	const auto config = doc.object();
	prefix = sanitizedLabel(config.value(QStringLiteral("momentPrefix")).toString(QStringLiteral("Момент")));
	if (prefix.isEmpty()) prefix = QStringLiteral("Момент");
	category = std::clamp(config.value(QStringLiteral("kdenliveType")).toInt(), 0, 4);
	sidecarCsv = config.value(QStringLiteral("sidecarCsv")).toBool(false);
	momentCounter = std::max(0, config.value(QStringLiteral("momentCounter")).toInt());
	for (const auto &value : config.value(QStringLiteral("autoTypes")).toArray()) {
		const auto type = value.toString();
		if (kAutoTypes.contains(type)) autoTypes.insert(type);
	}
	const auto saved = config.value(QStringLiteral("markers")).toArray();
	for (const auto &value : saved) {
		if (!value.isObject()) continue;
		const auto entry = value.toObject();
		const auto when = entry.value(QStringLiteral("time")).toString();
		if (when.size() > 50) continue;
		const auto label = sanitizedLabel(entry.value(QStringLiteral("label")).toString());
		if (label.isEmpty()) continue;
		QJsonArray recordings;
		for (const auto &value : entry.value(QStringLiteral("recordings")).toArray()) {
			if (recordings.size() >= 16) break;
			if (!value.isObject()) continue;
			const auto recording = value.toObject();
			const QString jobId = recording.value(QStringLiteral("jobId")).toString().left(80);
			if (jobId.isEmpty()) continue;
			recordings.append(QJsonObject{
				{QStringLiteral("jobId"), jobId},
				{QStringLiteral("name"), recording.value(QStringLiteral("name")).toString().left(120)},
				{QStringLiteral("file"), recording.value(QStringLiteral("file")).toString().left(1024)},
				{QStringLiteral("frame"), recording.value(QStringLiteral("frame")).toDouble(-1)},
				{QStringLiteral("elapsedMs"), recording.value(QStringLiteral("elapsedMs")).toDouble(-1)}});
		}
		entries.append(QJsonObject{
			{QStringLiteral("time"), when},
			{QStringLiteral("label"), label},
			{QStringLiteral("type"), entry.value(QStringLiteral("type")).toString().left(32)},
			{QStringLiteral("platform"), entry.value(QStringLiteral("platform")).toString().left(32)},
			{QStringLiteral("elapsedMs"), entry.value(QStringLiteral("elapsedMs")).toDouble()},
			{QStringLiteral("recordings"), recordings}});
		if (entries.size() > 1000) entries.removeAt(0);
	}
}

bool MarkerEngine::flush(QString *error)
{
	saveTimer.stop();
	if (path.isEmpty()) {
		storageError = QStringLiteral("Папка настроек XINKX недоступна");
		if (error) *error = storageError;
		return false;
	}
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		storageError = QStringLiteral("Не удалось создать папку для маркеров");
		if (error) *error = storageError;
		return false;
	}
	QJsonArray automatic;
	for (const auto &type : kAutoTypes)
		if (autoTypes.contains(type)) automatic.append(type);
	const QJsonDocument doc(QJsonObject{{QStringLiteral("version"), 2},
		{QStringLiteral("autoTypes"), automatic}, {QStringLiteral("markers"), entries},
		{QStringLiteral("momentPrefix"), prefix}, {QStringLiteral("momentCounter"), momentCounter},
		{QStringLiteral("kdenliveType"), category}, {QStringLiteral("sidecarCsv"), sidecarCsv}});
	const auto bytes = doc.toJson(QJsonDocument::Indented);
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
		storageError = QStringLiteral("Не удалось сохранить маркеры: ") + file.errorString();
		if (error) *error = storageError;
		return false;
	}
	storageError.clear();
	return true;
}

bool MarkerEngine::add(const QString &label, const QString &type, const QString &platform, bool requireRecording)
{
	const auto value = sanitizedLabel(label);
	if (value.isEmpty()) return false;
	// One marker command fans out to each running opted-in output. Every output
	// uses its own encoded PTS and atomically persists its own Kdenlive JSON.
	QJsonArray recordings;
	if (auto *window = qobject_cast<OBSBasic *>(parent() ? parent()->parent() : nullptr))
		recordings = captureRecordingMarker(window, value, category, sidecarCsv);
	if (requireRecording && recordings.isEmpty()) return false;
	while (recordings.size() > 16) recordings.removeAt(recordings.size() - 1);
	entries.append(QJsonObject{{QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
		{QStringLiteral("label"), value}, {QStringLiteral("type"), type.left(32)},
		{QStringLiteral("platform"), platform.left(32)},
		{QStringLiteral("elapsedMs"), static_cast<double>(session.elapsed())},
		{QStringLiteral("recordings"), recordings}});
	while (entries.size() > 1000) entries.removeAt(0);
	saveTimer.start();
	emit changed();
	return true;
}

bool MarkerEngine::markMoment()
{
	auto *window = qobject_cast<OBSBasic *>(parent() ? parent()->parent() : nullptr);
	if (!window || !hasMarkedRecording(window)) return false;
	const auto label = QStringLiteral("%1 %2").arg(prefix).arg(momentCounter + 1);
	if (!add(label, QStringLiteral("moment"), QStringLiteral("local"), true)) return false;
	++momentCounter;
	flush();
	emit momentMarked(label);
	return true;
}

bool MarkerEngine::setMomentOptions(const QString &newPrefix, int newType, bool newCsv, QString *error)
{
	const auto clean = sanitizedLabel(newPrefix);
	if (clean.isEmpty() || newType < 0 || newType > 4) {
		if (error) *error = QStringLiteral("Название и тип маркера указаны неверно");
		return false;
	}
	const auto oldPrefix = prefix;
	const auto oldType = category;
	const auto oldCsv = sidecarCsv;
	prefix = clean;
	category = newType;
	sidecarCsv = newCsv;
	if (flush(error)) { emit changed(); return true; }
	prefix = oldPrefix;
	category = oldType;
	sidecarCsv = oldCsv;
	return false;
}

QJsonObject MarkerEngine::execute(const QString &actionId, const QJsonObject &arguments)
{
	if (actionId != QStringLiteral("markers.add"))
		return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), QStringLiteral("Неизвестная команда")}};
	const QString label = sanitizedLabel(arguments.value(QStringLiteral("label")).toString());
	if (label.isEmpty())
		return {{QStringLiteral("ok"), false}, {QStringLiteral("message"), QStringLiteral("Укажи название маркера")}};
	const bool added = add(label, QStringLiteral("manual"), QStringLiteral("local"));
	return {{QStringLiteral("ok"), added},
		{QStringLiteral("message"), added ? QStringLiteral("Маркер сохранён в очереди записи") :
			QStringLiteral("Маркер не добавлен")}};
}

void MarkerEngine::onEvent(const StudioEvent &event)
{
	if (!autoTypes.contains(event.type)) return;
	// Do not persist the viewer name, chat message, account ID or paid amount.
	add(QStringLiteral("Событие: %1").arg(event.type), event.type, event.platform);
}

QJsonObject MarkerEngine::health() const
{
	return {{QStringLiteral("status"), storageError.isEmpty() ? QStringLiteral("ready") : QStringLiteral("error")},
		{QStringLiteral("markers"), static_cast<int>(entries.size())}, {QStringLiteral("message"), storageError}};
}

QStringList MarkerEngine::automaticTypes() const
{
	QStringList types;
	for (const auto &type : kAutoTypes)
		if (autoTypes.contains(type)) types.append(type);
	return types;
}

bool MarkerEngine::setAutomaticTypes(const QStringList &types, QString *error)
{
	QSet<QString> requested;
	for (const auto &type : types) {
		if (!kAutoTypes.contains(type)) {
			if (error) *error = QStringLiteral("Неизвестный тип события");
			return false;
		}
		requested.insert(type);
	}
	const auto previous = autoTypes;
	autoTypes = requested;
	if (flush(error)) { emit changed(); return true; }
	autoTypes = previous;
	return false;
}

bool MarkerEngine::clear(QString *error)
{
	const auto previous = entries;
	entries = {};
	if (flush(error)) { emit changed(); return true; }
	entries = previous;
	return false;
}

bool MarkerEngine::exportCsv(const QString &fileName, QString *error) const
{
	QSaveFile file(fileName);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		if (error) *error = file.errorString();
		return false;
	}
	QTextStream out(&file);
	out << "time,elapsed_since_xinkx_launch_ms,type,platform,label,recording_job,recording_file,recording_elapsed_ms\n";
	for (const auto &value : entries) {
		const auto entry = value.toObject();
		const QString prefix = quotedCsv(entry.value(QStringLiteral("time")).toString()) + QLatin1Char(',') +
			QString::number(entry.value(QStringLiteral("elapsedMs")).toDouble(), 'f', 0) + QLatin1Char(',') +
			quotedCsv(entry.value(QStringLiteral("type")).toString()) + QLatin1Char(',') +
			quotedCsv(entry.value(QStringLiteral("platform")).toString()) + QLatin1Char(',') +
			quotedCsv(entry.value(QStringLiteral("label")).toString());
		const auto recordings = entry.value(QStringLiteral("recordings")).toArray();
		if (recordings.isEmpty()) {
			out << prefix << ",,,\n";
			continue;
		}
		for (const auto &value : recordings) {
			const auto recording = value.toObject();
			const double elapsed = recording.value(QStringLiteral("elapsedMs")).toDouble(-1);
			out << prefix << ','
				<< quotedCsv(recording.value(QStringLiteral("name")).toString()) << ','
				<< quotedCsv(recording.value(QStringLiteral("file")).toString()) << ','
				<< (elapsed < 0 ? QString() : QString::number(elapsed, 'f', 0)) << '\n';
		}
	}
	out.flush();
	if (out.status() != QTextStream::Ok || !file.commit()) {
		if (error) *error = file.errorString();
		return false;
	}
	return true;
}

void installMarkerWorkspace(OBSBasic *mainWindow)
{
	auto *registry = mainWindow->findChild<EngineRegistry *>(QStringLiteral("xinkxEngineRegistry"));
	if (!registry) return;
	auto *engine = mainWindow->findChild<MarkerEngine *>(QStringLiteral("xinkxMarkerEngine"));
	if (!engine) return;
	auto *tabs = studioTabs(mainWindow);
	if (!tabs) return;
	auto *page = new QWidget(tabs);
	auto *layout = new QVBoxLayout(page);
	auto *description = new QLabel(QStringLiteral(
		"Метки важных моментов для монтажа. «Отметить момент» настраивается в «Настройки → "
		"Горячие клавиши». Для записей с галочкой «Вести метки» рядом с видео сразу "
		"сохраняется Kdenlive JSON. Позиция вычисляется из PTS видеопакетов каждого выхода."), page);
	description->setWordWrap(true);
	layout->addWidget(description);
	auto *list = new QTreeWidget(page);
	list->setHeaderLabels({QStringLiteral("Время"), QStringLiteral("Тип"),
		QStringLiteral("Площадка"), QStringLiteral("Заметка"), QStringLiteral("Записи")});
	list->setRootIsDecorated(false);
	layout->addWidget(list);
	const auto refresh = [engine, list] {
		list->clear();
		for (const auto &value : engine->markers()) {
			const auto marker = value.toObject();
			QStringList files;
			QStringList paths;
			for (const auto &value : marker.value(QStringLiteral("recordings")).toArray()) {
				const auto recording = value.toObject();
				const double frame = recording.value(QStringLiteral("frame")).toDouble(-1);
				const double elapsed = recording.value(QStringLiteral("elapsedMs")).toDouble(-1);
				const QString at = frame >= 0 ? QStringLiteral("кадр %1").arg(frame, 0, 'f', 0) : elapsed < 0 ? QStringLiteral("?") :
					QStringLiteral("%1:%2").arg(static_cast<qint64>(elapsed) / 60000)
						.arg((static_cast<qint64>(elapsed) / 1000) % 60, 2, 10, QLatin1Char('0'));
				files.append(recording.value(QStringLiteral("name")).toString() +
					QStringLiteral(" (") + at + QLatin1Char(')'));
				paths.append(recording.value(QStringLiteral("file")).toString());
			}
			auto *row = new QTreeWidgetItem(list, {
				marker.value(QStringLiteral("time")).toString(),
				marker.value(QStringLiteral("type")).toString(),
				marker.value(QStringLiteral("platform")).toString(),
				marker.value(QStringLiteral("label")).toString(), files.join(QStringLiteral(", "))});
			row->setToolTip(4, paths.join(QLatin1Char('\n')));
		}
		list->scrollToBottom();
	};
	refresh();
	QObject::connect(engine, &MarkerEngine::changed, page, refresh);
	auto *entryRow = new QHBoxLayout;
	auto *label = new QLineEdit(page);
	label->setPlaceholderText(QStringLiteral("Что произошло?"));
	label->setMaxLength(120);
	entryRow->addWidget(label);
	auto *add = new QPushButton(QStringLiteral("Добавить маркер"), page);
	entryRow->addWidget(add);
	layout->addLayout(entryRow);
	const auto addNow = [registry, label] {
		const QJsonObject command{{QStringLiteral("command"), QStringLiteral("markers.add")},
			{QStringLiteral("label"), label->text()}};
		const auto result = registry->executeAction(command);
		if (result.value(QStringLiteral("ok")).toBool()) label->clear();
	};
	QObject::connect(add, &QPushButton::clicked, page, addNow);
	QObject::connect(label, &QLineEdit::returnPressed, page, addNow);
	layout->addWidget(new QLabel(QStringLiteral("Автоматические метки по реальным событиям Event Core:"), page));
	auto *checks = new QHBoxLayout;
	const QStringList labels{QStringLiteral("Донаты"), QStringLiteral("Подписки"),
		QStringLiteral("Рейды"), QStringLiteral("Фолловеры")};
	const auto saved = engine->automaticTypes();
	QList<QCheckBox *> toggles;
	for (int i = 0; i < kAutoTypes.size(); ++i) {
		auto *check = new QCheckBox(labels.at(i), page);
		check->setChecked(saved.contains(kAutoTypes.at(i)));
		checks->addWidget(check);
		toggles.append(check);
	}
	layout->addLayout(checks);
	for (auto *check : toggles)
		QObject::connect(check, &QCheckBox::toggled, page, [page, engine, toggles] {
			QStringList types;
			for (int i = 0; i < toggles.size(); ++i)
				if (toggles.at(i)->isChecked()) types << kAutoTypes.at(i);
			QString error;
			if (!engine->setAutomaticTypes(types, &error)) {
				const auto actual = engine->automaticTypes();
				for (int i = 0; i < toggles.size(); ++i) {
					const QSignalBlocker blocked(toggles.at(i));
					toggles.at(i)->setChecked(actual.contains(kAutoTypes.at(i)));
				}
				QMessageBox::warning(page, QStringLiteral("Маркеры"), error);
			}
		});
	auto *buttons = new QHBoxLayout;
	auto *exportButton = new QPushButton(QStringLiteral("Экспорт CSV"), page);
	buttons->addWidget(exportButton);
	auto *clearButton = new QPushButton(QStringLiteral("Очистить список"), page);
	buttons->addWidget(clearButton);
	layout->addLayout(buttons);
	QObject::connect(exportButton, &QPushButton::clicked, page, [page, engine] {
		const auto fileName = QFileDialog::getSaveFileName(page, QStringLiteral("Экспорт маркеров"),
			QStringLiteral("xinkx-markers.csv"), QStringLiteral("CSV (*.csv)"));
		if (fileName.isEmpty()) return;
		QString error;
		if (!engine->exportCsv(fileName, &error))
			QMessageBox::warning(page, QStringLiteral("Экспорт маркеров"), error);
	});
	QObject::connect(clearButton, &QPushButton::clicked, page, [page, engine] {
		if (QMessageBox::question(page, QStringLiteral("Очистить маркеры"),
			QStringLiteral("Удалить все локальные маркеры?")) != QMessageBox::Yes) return;
		QString error;
		if (!engine->clear(&error))
			QMessageBox::warning(page, QStringLiteral("Маркеры"), error);
	});
	tabs->addTab(page, QStringLiteral("Маркеры"));
}

} // namespace xinkx
