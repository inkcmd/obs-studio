#include "MarkerEngine.hpp"

#include "EngineRegistry.hpp"
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
	if (saveTimer.isActive()) flush();
}

void MarkerEngine::load()
{
	if (path.isEmpty()) return;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return;
	if (file.size() > 512 * 1024) return;
	QJsonParseError error{};
	const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) return;
	const auto config = doc.object();
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
		entries.append(QJsonObject{
			{QStringLiteral("time"), when},
			{QStringLiteral("label"), label},
			{QStringLiteral("type"), entry.value(QStringLiteral("type")).toString().left(32)},
			{QStringLiteral("platform"), entry.value(QStringLiteral("platform")).toString().left(32)},
			{QStringLiteral("elapsedMs"), entry.value(QStringLiteral("elapsedMs")).toDouble()}});
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
	const QJsonDocument doc(QJsonObject{{QStringLiteral("version"), 1},
		{QStringLiteral("autoTypes"), automatic}, {QStringLiteral("markers"), entries}});
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

bool MarkerEngine::add(const QString &label, const QString &type, const QString &platform)
{
	const auto value = sanitizedLabel(label);
	if (value.isEmpty()) return false;
	entries.append(QJsonObject{{QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
		{QStringLiteral("label"), value}, {QStringLiteral("type"), type.left(32)},
		{QStringLiteral("platform"), platform.left(32)},
		{QStringLiteral("elapsedMs"), static_cast<double>(session.elapsed())}});
	while (entries.size() > 1000) entries.removeAt(0);
	saveTimer.start();
	emit changed();
	return true;
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
	out << "time,elapsed_since_xinkx_launch_ms,type,platform,label\n";
	for (const auto &value : entries) {
		const auto entry = value.toObject();
		out << quotedCsv(entry.value(QStringLiteral("time")).toString()) << ','
			<< QString::number(entry.value(QStringLiteral("elapsedMs")).toDouble(), 'f', 0) << ','
			<< quotedCsv(entry.value(QStringLiteral("type")).toString()) << ','
			<< quotedCsv(entry.value(QStringLiteral("platform")).toString()) << ','
			<< quotedCsv(entry.value(QStringLiteral("label")).toString()) << '\n';
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
		"Метки важных моментов для монтажа. Сохраняются локально с системным временем. "
		"Таймер в CSV отсчитывает время с запуска XINKX, не точный таймкод видео."), page);
	description->setWordWrap(true);
	layout->addWidget(description);
	auto *list = new QTreeWidget(page);
	list->setHeaderLabels({QStringLiteral("Время"), QStringLiteral("Тип"),
		QStringLiteral("Площадка"), QStringLiteral("Заметка")});
	list->setRootIsDecorated(false);
	layout->addWidget(list);
	const auto refresh = [engine, list] {
		list->clear();
		for (const auto &value : engine->markers()) {
			const auto marker = value.toObject();
			new QTreeWidgetItem(list, {
				marker.value(QStringLiteral("time")).toString(),
				marker.value(QStringLiteral("type")).toString(),
				marker.value(QStringLiteral("platform")).toString(),
				marker.value(QStringLiteral("label")).toString()});
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
