#include "StudioSettingsPages.hpp"

#include "DonationGateway.hpp"
#include "PresentationSettings.hpp"
#include "StudioAutomation.hpp"

#include <widgets/OBSBasic.hpp>

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace xinkx {
namespace {

QWidget *section(QWidget *parent, const QString &title, const QString &description, QVBoxLayout **body)
{
	auto *page = new QWidget(parent);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);
	auto *heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()), page);
	layout->addWidget(heading);
	auto *help = new QLabel(description, page);
	help->setWordWrap(true);
	layout->addWidget(help);
	*body = layout;
	return page;
}

QSpinBox *number(QWidget *parent, int min, int max, int value, const QString &suffix = {})
{
	auto *widget = new QSpinBox(parent);
	widget->setRange(min, max);
	widget->setValue(value);
	widget->setSuffix(suffix);
	widget->setKeyboardTracking(false);
	return widget;
}

bool commit(PresentationSettings *settings, const QJsonObject &doc, QWidget *parent)
{
	QString error;
	if (settings->save(doc, &error)) return true;
	QMessageBox::warning(parent, QStringLiteral("Настройки XINKX"), error);
	return false;
}

QWidget *chatPage(QWidget *parent, PresentationSettings *settings, const QString &surface)
{
	QVBoxLayout *layout;
	const bool streamer = surface == QStringLiteral("streamer");
	auto *page = section(parent, streamer ? QStringLiteral("Чат для меня") : QStringLiteral("Чат для зрителей"),
		streamer ? QStringLiteral("Оверлей поверх игры. Не захватывает мышь и не влияет на источник чата OBS.") :
		QStringLiteral("Оформление XINKX-чата в источнике OBS. Расположение и размер меняются на холсте."), &layout);
	const auto source = settings->snapshot().value(surface).toObject();
	const auto chat = source.value(QStringLiteral("chat")).toObject();
	auto *form = new QFormLayout();
	if (streamer) {
		auto *overlayEnabled = new QCheckBox(QStringLiteral("Показывать поверх игры · без взаимодействия с мышью"), page);
		if (auto *original = parent->window()->parentWidget()) {
			if (auto *toggle = original->findChild<QCheckBox *>(QStringLiteral("xinkxStreamerOverlayEnabled"))) {
				overlayEnabled->setChecked(toggle->isChecked());
				QObject::connect(overlayEnabled, &QCheckBox::toggled, toggle, &QCheckBox::setChecked);
			}
		}
		form->addRow(overlayEnabled);
	}
	auto *font = new QLineEdit(chat.value(QStringLiteral("font")).toString(), page);
	form->addRow(QStringLiteral("Шрифт"), font);
	auto *fontSize = number(page, 10, 90, chat.value(QStringLiteral("fontSize")).toInt(), QStringLiteral(" px"));
	form->addRow(QStringLiteral("Размер текста"), fontSize);
	auto *weight = number(page, 100, 900, chat.value(QStringLiteral("fontWeight")).toInt());
	form->addRow(QStringLiteral("Насыщенность шрифта"), weight);
	auto *messages = number(page, 1, 20, chat.value(QStringLiteral("maxMessages")).toInt());
	form->addRow(QStringLiteral("Сообщений"), messages);
	auto *duration = number(page, 0, 300000, chat.value(QStringLiteral("messageDurationMs")).toInt(), QStringLiteral(" мс"));
	form->addRow(QStringLiteral("Время сообщений (0 = всегда)"), duration);
	auto *gap = number(page, 0, 64, chat.value(QStringLiteral("gap")).toInt(), QStringLiteral(" px"));
	form->addRow(QStringLiteral("Между сообщениями"), gap);
	auto *padding = number(page, 0, 64, chat.value(QStringLiteral("padding")).toInt(), QStringLiteral(" px"));
	form->addRow(QStringLiteral("Отступ внутри"), padding);
	auto *radius = number(page, 0, 64, chat.value(QStringLiteral("radius")).toInt(), QStringLiteral(" px"));
	form->addRow(QStringLiteral("Скругление"), radius);
	auto *opacity = number(page, 10, 100, chat.value(QStringLiteral("opacity")).toInt(), QStringLiteral(" %"));
	form->addRow(QStringLiteral("Непрозрачность"), opacity);
	auto *width = number(page, 10, 100, chat.value(QStringLiteral("widthPercent")).toInt(), QStringLiteral(" %"));
	form->addRow(QStringLiteral("Ширина чата"), width);
	auto *position = new QComboBox(page);
	position->addItem(QStringLiteral("Слева"), QStringLiteral("left"));
	position->addItem(QStringLiteral("По центру"), QStringLiteral("center"));
	position->addItem(QStringLiteral("Справа"), QStringLiteral("right"));
	position->setCurrentIndex(qMax(0, position->findData(chat.value(QStringLiteral("horizontal")).toString())));
	form->addRow(QStringLiteral("Положение"), position);
	auto *vertical = new QComboBox(page);
	vertical->addItem(QStringLiteral("Сверху"), QStringLiteral("top"));
	vertical->addItem(QStringLiteral("По центру"), QStringLiteral("center"));
	vertical->addItem(QStringLiteral("Снизу"), QStringLiteral("bottom"));
	vertical->setCurrentIndex(qMax(0, vertical->findData(chat.value(QStringLiteral("vertical")).toString())));
	form->addRow(QStringLiteral("По вертикали"), vertical);
	auto *showPlatform = new QCheckBox(QStringLiteral("Иконка площадки"), page);
	showPlatform->setChecked(chat.value(QStringLiteral("showPlatform")).toBool());
	form->addRow(showPlatform);
	auto *showViewers = new QCheckBox(QStringLiteral("Показывать зрителей"), page);
	showViewers->setChecked(chat.value(QStringLiteral("showViewers")).toBool());
	form->addRow(showViewers);
	auto *showTime = new QCheckBox(QStringLiteral("Время сообщений"), page);
	showTime->setChecked(chat.value(QStringLiteral("showTime")).toBool());
	form->addRow(showTime);
	auto colorRow = [=](const QString &key, const QString &label) {
		auto *button = new QPushButton(chat.value(key).toString(), page);
		form->addRow(label, button);
		QObject::connect(button, &QPushButton::clicked, page, [=] {
			const QColor current(button->text());
			const QColor selected = QColorDialog::getColor(current, page, label);
			if (selected.isValid()) button->setText(selected.name());
		});
		return button;
	};
	auto *textColor = colorRow(QStringLiteral("textColor"), QStringLiteral("Текст"));
	auto *nameColor = colorRow(QStringLiteral("nameColor"), QStringLiteral("Имя"));
	auto *background = colorRow(QStringLiteral("backgroundColor"), QStringLiteral("Фон сообщения"));
	layout->addLayout(form);
	auto *save = new QPushButton(QStringLiteral("Сохранить оформление"), page);
	layout->addWidget(save);
	layout->addStretch();
	QObject::connect(save, &QPushButton::clicked, page, [=] {
		QJsonObject document = settings->snapshot();
		QJsonObject profile = document.value(surface).toObject();
		QJsonObject value = profile.value(QStringLiteral("chat")).toObject();
		value.insert(QStringLiteral("font"), font->text());
		value.insert(QStringLiteral("fontSize"), fontSize->value());
		value.insert(QStringLiteral("fontWeight"), weight->value());
		value.insert(QStringLiteral("maxMessages"), messages->value());
		value.insert(QStringLiteral("messageDurationMs"), duration->value());
		value.insert(QStringLiteral("gap"), gap->value());
		value.insert(QStringLiteral("padding"), padding->value());
		value.insert(QStringLiteral("radius"), radius->value());
		value.insert(QStringLiteral("opacity"), opacity->value());
		value.insert(QStringLiteral("widthPercent"), width->value());
		value.insert(QStringLiteral("horizontal"), position->currentData().toString());
		value.insert(QStringLiteral("vertical"), vertical->currentData().toString());
		value.insert(QStringLiteral("showPlatform"), showPlatform->isChecked());
		value.insert(QStringLiteral("showViewers"), showViewers->isChecked());
		value.insert(QStringLiteral("showTime"), showTime->isChecked());
		value.insert(QStringLiteral("textColor"), textColor->text());
		value.insert(QStringLiteral("nameColor"), nameColor->text());
		value.insert(QStringLiteral("backgroundColor"), background->text());
		profile.insert(QStringLiteral("chat"), value);
		document.insert(surface, profile);
		commit(settings, document, page);
	});
	return page;
}

void editRule(QWidget *page, PresentationSettings *settings, bool bot, int index)
{
	const QString key = bot ? QStringLiteral("botCommands") : QStringLiteral("reactions");
	QJsonArray entries = settings->snapshot().value(key).toArray();
	if (index < 0 && entries.size() >= (bot ? 20 : 12)) return;
	QJsonObject rule = index < 0 ? QJsonObject{} : entries.at(index).toObject();
	QDialog editor(page);
	editor.setWindowTitle(bot ? QStringLiteral("Команда чат-бота") : QStringLiteral("Реакция зрителей"));
	auto *form = new QFormLayout(&editor);
	auto *phrase = new QLineEdit(rule.value(bot ? QStringLiteral("trigger") : QStringLiteral("phrase")).toString(), &editor);
	phrase->setMaxLength(40);
	phrase->setPlaceholderText(bot ? QStringLiteral("!discord") : QStringLiteral("gg, !boom, low hp"));
	form->addRow(bot ? QStringLiteral("Команда") : QStringLiteral("Фраза"), phrase);
	auto *target = new QComboBox(&editor);
	if (bot) {
		// A line edit, not a fabricated effect: sends a real chat message through the gateway.
	} else {
		for (const auto &effect : {"gg", "boom", "low-hp", "confetti"})
			target->addItem(QString::fromLatin1(effect));
		target->setCurrentIndex(qMax(0, target->findText(rule.value(QStringLiteral("effect")).toString())));
		form->addRow(QStringLiteral("Эффект"), target);
	}
	auto *reply = bot ? new QLineEdit(rule.value(QStringLiteral("reply")).toString(), &editor) : nullptr;
	if (reply) { reply->setMaxLength(200); form->addRow(QStringLiteral("Ответ"), reply); }
	auto *match = new QComboBox(&editor);
	if (!bot) {
		match->addItem(QStringLiteral("Точное совпадение"), QStringLiteral("exact"));
		match->addItem(QStringLiteral("Отдельное слово"), QStringLiteral("word"));
		match->addItem(QStringLiteral("Содержит"), QStringLiteral("contains"));
		match->setCurrentIndex(qMax(0, match->findData(rule.value(QStringLiteral("match")).toString())));
		form->addRow(QStringLiteral("Условие"), match);
	}
	auto *cooldown = number(&editor, 0, 300, rule.value(QStringLiteral("cooldownMs")).toInt() / 1000, QStringLiteral(" с"));
	form->addRow(QStringLiteral("Пауза"), cooldown);
	auto *enabled = new QCheckBox(QStringLiteral("Включено"), &editor);
	enabled->setChecked(rule.value(QStringLiteral("enabled")).toBool(true));
	form->addRow(enabled);
	auto *perViewer = new QCheckBox(QStringLiteral("Отдельная пауза для зрителя"), &editor);
	perViewer->setChecked(rule.value(QStringLiteral("perViewer")).toBool(true));
	form->addRow(perViewer);
	auto *save = new QPushButton(QStringLiteral("Сохранить"), &editor);
	form->addRow(save);
	QObject::connect(save, &QPushButton::clicked, &editor, [&] {
		if (phrase->text().trimmed().isEmpty() || (bot && (!phrase->text().startsWith(QLatin1Char('!')) ||
		    reply->text().trimmed().isEmpty()))) return;
		if (bot) {
			rule.insert(QStringLiteral("trigger"), phrase->text().trimmed());
			rule.insert(QStringLiteral("reply"), reply->text());
		} else {
			if (index < 0) rule.insert(QStringLiteral("id"), QStringLiteral("r-") +
				QUuid::createUuid().toString(QUuid::WithoutBraces).left(18));
			rule.insert(QStringLiteral("phrase"), phrase->text().trimmed());
			rule.insert(QStringLiteral("effect"), target->currentText());
			rule.insert(QStringLiteral("match"), match->currentData().toString());
			if (!rule.contains(QStringLiteral("durationMs"))) rule.insert(QStringLiteral("durationMs"), 5000);
		}
		rule.insert(QStringLiteral("cooldownMs"), cooldown->value() * 1000);
		rule.insert(QStringLiteral("enabled"), enabled->isChecked());
		rule.insert(QStringLiteral("perViewer"), perViewer->isChecked());
		if (index < 0) entries.append(rule);
		else entries.replace(index, rule);
		QJsonObject document = settings->snapshot();
		document.insert(key, entries);
		if (commit(settings, document, &editor)) editor.accept();
	});
	editor.exec();
}

QWidget *reactionsPage(QWidget *parent, PresentationSettings *settings)
{
	QVBoxLayout *layout;
	auto *page = section(parent, QStringLiteral("Реакции и чат-боты"),
		QStringLiteral("Каждая фраза вызывает эффект общего движка, команда бота отвечает в чат. "
		"Добавление новых визуальных типов источников — через стандартный плюс OBS."), &layout);
	auto *effectForm = new QFormLayout();
	auto *surface = new QComboBox(page);
	surface->addItem(QStringLiteral("Для зрителей · OBS"), QStringLiteral("viewer"));
	surface->addItem(QStringLiteral("Для меня · поверх игры"), QStringLiteral("streamer"));
	effectForm->addRow(QStringLiteral("Оформление"), surface);
	auto *enabledEffects = new QCheckBox(QStringLiteral("Показывать эффекты"), page);
	effectForm->addRow(enabledEffects);
	auto *position = new QComboBox(page);
	for (const auto &entry : {"random", "top-left", "top-right", "center", "bottom-left", "bottom-right"})
		position->addItem(QString::fromLatin1(entry));
	effectForm->addRow(QStringLiteral("Позиция"), position);
	auto *scale = number(page, 30, 200, 100, QStringLiteral(" %"));
	effectForm->addRow(QStringLiteral("Масштаб"), scale);
	auto *minParticles = number(page, 0, 200, 25);
	effectForm->addRow(QStringLiteral("Частиц от"), minParticles);
	auto *maxParticles = number(page, 0, 200, 65);
	effectForm->addRow(QStringLiteral("Частиц до"), maxParticles);
	auto *maxConcurrent = number(page, 1, 8, 2);
	effectForm->addRow(QStringLiteral("Одновременно"), maxConcurrent);
	auto *duration = number(page, 0, 60000, 5000, QStringLiteral(" мс"));
	effectForm->addRow(QStringLiteral("Длительность"), duration);
	auto *globalCooldown = number(page, 0, 300000,
		settings->snapshot().value(QStringLiteral("globalCooldownMs")).toInt(), QStringLiteral(" мс"));
	effectForm->addRow(QStringLiteral("Общая пауза"), globalCooldown);
	layout->addLayout(effectForm);
	auto fillEffects = [=] {
		const auto profile = settings->snapshot().value(surface->currentData().toString()).toObject();
		const auto effects = profile.value(QStringLiteral("effects")).toObject();
		enabledEffects->setChecked(effects.value(QStringLiteral("enabled")).toBool(true));
		position->setCurrentIndex(qMax(0, position->findText(effects.value(QStringLiteral("position")).toString())));
		scale->setValue(effects.value(QStringLiteral("scalePercent")).toInt(100));
		minParticles->setValue(effects.value(QStringLiteral("particleMin")).toInt());
		maxParticles->setValue(effects.value(QStringLiteral("particleMax")).toInt());
		maxConcurrent->setValue(effects.value(QStringLiteral("maxConcurrent")).toInt(2));
		duration->setValue(effects.value(QStringLiteral("durationMs")).toInt(5000));
	};
	QObject::connect(surface, &QComboBox::currentIndexChanged, page, fillEffects);
	fillEffects();
	auto *saveEffects = new QPushButton(QStringLiteral("Сохранить параметры эффектов"), page);
	layout->addWidget(saveEffects);
	QObject::connect(saveEffects, &QPushButton::clicked, page, [=] {
		if (minParticles->value() > maxParticles->value()) {
			QMessageBox::warning(page, QStringLiteral("Эффекты"),
				QStringLiteral("Частиц от должно быть не больше, чем частиц до"));
			return;
		}
		QJsonObject document = settings->snapshot();
		const QString surfaceName = surface->currentData().toString();
		QJsonObject profile = document.value(surfaceName).toObject();
		QJsonObject effects = profile.value(QStringLiteral("effects")).toObject();
		effects.insert(QStringLiteral("enabled"), enabledEffects->isChecked());
		effects.insert(QStringLiteral("position"), position->currentText());
		effects.insert(QStringLiteral("scalePercent"), scale->value());
		effects.insert(QStringLiteral("particleMin"), minParticles->value());
		effects.insert(QStringLiteral("particleMax"), maxParticles->value());
		effects.insert(QStringLiteral("maxConcurrent"), maxConcurrent->value());
		effects.insert(QStringLiteral("durationMs"), duration->value());
		profile.insert(QStringLiteral("effects"), effects);
		document.insert(surfaceName, profile);
		document.insert(QStringLiteral("globalCooldownMs"), globalCooldown->value());
		commit(settings, document, page);
	});
	for (const bool bot : {false, true}) {
		layout->addWidget(new QLabel(bot ? QStringLiteral("<b>Команды бота</b>") :
			QStringLiteral("<b>Реакции зрителей</b>"), page));
		auto *list = new QListWidget(page);
		list->setMinimumHeight(110);
		layout->addWidget(list);
		auto populate = [=] {
			const int selected = list->currentRow();
			list->clear();
			const QJsonArray values = settings->snapshot().value(bot ? QStringLiteral("botCommands") :
				QStringLiteral("reactions")).toArray();
			for (const auto &item : values) {
				const auto object = item.toObject();
				list->addItem(bot ? object.value(QStringLiteral("trigger")).toString() +
					QStringLiteral(" → ") + object.value(QStringLiteral("reply")).toString() :
					object.value(QStringLiteral("phrase")).toString() + QStringLiteral(" → ") +
					object.value(QStringLiteral("effect")).toString());
			}
			if (selected >= 0 && selected < list->count()) list->setCurrentRow(selected);
		};
		auto *row = new QHBoxLayout();
		auto *add = new QPushButton(QStringLiteral("Добавить"), page);
		auto *edit = new QPushButton(QStringLiteral("Изменить"), page);
		auto *remove = new QPushButton(QStringLiteral("Удалить"), page);
		for (auto *button : {add, edit, remove}) row->addWidget(button);
		layout->addLayout(row);
		QObject::connect(add, &QPushButton::clicked, page, [=] { editRule(page, settings, bot, -1); populate(); });
		QObject::connect(edit, &QPushButton::clicked, page, [=] {
			if (list->currentRow() >= 0) { editRule(page, settings, bot, list->currentRow()); populate(); }
		});
		QObject::connect(list, &QListWidget::itemDoubleClicked, page, [=] {
			if (list->currentRow() >= 0) { editRule(page, settings, bot, list->currentRow()); populate(); }
		});
		QObject::connect(remove, &QPushButton::clicked, page, [=] {
			if (list->currentRow() < 0) return;
			QJsonObject document = settings->snapshot();
			const QString key = bot ? QStringLiteral("botCommands") : QStringLiteral("reactions");
			auto values = document.value(key).toArray();
			values.removeAt(list->currentRow());
			document.insert(key, values);
			if (commit(settings, document, page)) populate();
		});
		populate();
	}
	layout->addStretch();
	return page;
}

QWidget *automationPage(QWidget *parent, OBSBasic *mainWindow)
{
	QVBoxLayout *layout;
	auto *page = section(parent, QStringLiteral("Автоматизация / LLM"),
		QStringLiteral("Локальные команды выполняются через белый список действий OBS. "
		"Подключение модели, слушающий HTTP-сервер и доступ к файлам здесь не включены."), &layout);
	auto *editor = new QPlainTextEdit(page);
	editor->setPlainText(QStringLiteral("{\"command\":\"overlay.add\",\"layer\":\"chat\"}"));
	editor->setMaximumHeight(110);
	layout->addWidget(editor);
	auto *buttons = new QHBoxLayout();
	auto *describe = new QPushButton(QStringLiteral("Доступные команды"), page);
	auto *execute = new QPushButton(QStringLiteral("Выполнить локально"), page);
	buttons->addWidget(describe);
	buttons->addWidget(execute);
	layout->addLayout(buttons);
	auto *result = new QPlainTextEdit(page);
	result->setReadOnly(true);
	result->setMaximumHeight(170);
	layout->addWidget(result);
	auto *automation = mainWindow->findChild<StudioAutomation *>();
	QObject::connect(describe, &QPushButton::clicked, page, [=] {
		if (automation) result->setPlainText(QString::fromUtf8(QJsonDocument(automation->describe()).toJson(QJsonDocument::Indented)));
	});
	QObject::connect(execute, &QPushButton::clicked, page, [=] {
		if (!automation || editor->toPlainText().size() > 8192) return;
		const QJsonDocument payload = QJsonDocument::fromJson(editor->toPlainText().toUtf8());
		result->setPlainText(payload.isObject()
			? QString::fromUtf8(QJsonDocument(automation->execute(payload.object())).toJson(QJsonDocument::Indented))
			: QStringLiteral("Нужен JSON-объект"));
	});
	layout->addStretch();
	return page;
}

} // namespace

void installStudioSettingsPages(QWidget *settingsDialog)
{
	auto *nav = settingsDialog->findChild<QListWidget *>(QStringLiteral("listWidget"));
	auto *stack = settingsDialog->findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
	auto *mainWindow = qobject_cast<OBSBasic *>(settingsDialog->parentWidget());
	if (!nav || !stack || !mainWindow) return;
	auto *presentation = mainWindow->findChild<PresentationSettings *>(QStringLiteral("xinkxPresentationSettings"));
	if (!presentation) return;
	const auto add = [=](const QString &name, QWidget *page, const QString &icon) {
		auto *scroll = new QScrollArea(stack);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidgetResizable(true);
		scroll->setWidget(page);
		stack->addWidget(scroll);
		auto *item = new QListWidgetItem(QIcon(icon), name, nav);
		item->setToolTip(name);
	};
	add(QStringLiteral("Донаты"), createDonationsSettingsPage(mainWindow, stack),
		QStringLiteral(":/settings/images/settings/stream.svg"));
	add(QStringLiteral("Чат для зрителей"), chatPage(stack, presentation, QStringLiteral("viewer")),
		QStringLiteral(":/settings/images/settings/appearance.svg"));
	add(QStringLiteral("Чат для меня"), chatPage(stack, presentation, QStringLiteral("streamer")),
		QStringLiteral(":/settings/images/settings/appearance.svg"));
	add(QStringLiteral("Реакции и боты"), reactionsPage(stack, presentation),
		QStringLiteral(":/settings/images/settings/hotkeys.svg"));
	add(QStringLiteral("Автоматизация / LLM"), automationPage(stack, mainWindow),
		QStringLiteral(":/settings/images/settings/advanced.svg"));
}

} // namespace xinkx
