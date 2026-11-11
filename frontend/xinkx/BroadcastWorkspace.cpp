#include "ChatGateway.hpp"
#include "BroadcastTagsEditor.hpp"
#include "StudioWorkspace.hpp"
#include "StudioDiagnostics.hpp"

#include <widgets/OBSBasic.hpp>

#include <QComboBox>
#include <QCompleter>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPair>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QStringListModel>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {
namespace {

QString key(const QString &part) { return QStringLiteral("broadcast/editor/") + part; }

void choose(QComboBox *combo, const QString &id, const QString &caption)
{
	if (!combo) return;
	const QSignalBlocker blocked(combo);
	int index = combo->findData(id);
	if (index < 0 && !id.isEmpty()) {
		combo->addItem(caption.isEmpty() ? QStringLiteral("Сохранённая категория") : caption, id);
		index = combo->count() - 1;
	}
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

} // namespace

void installBroadcastWorkspace(OBSBasic *mainWindow)
{
	auto *gateway = mainWindow->findChild<ChatGateway *>(QStringLiteral("xinkxChatGateway"));
	auto *dock = studioDock(mainWindow);
	if (!gateway || !dock) return;

	auto *panel = new QWidget(dock);
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);
	auto *local = new QSettings(studioSettingsPath(), QSettings::IniFormat, panel);
	auto *flush = new QTimer(panel);
	flush->setSingleShot(true);
	flush->setInterval(500);
	QObject::connect(flush, &QTimer::timeout, panel, [local] { local->sync(); });

	auto *form = new QFormLayout();
	form->setSpacing(6);
	auto *title = new QLineEdit(local->value(key(QStringLiteral("title"))).toString(), panel);
	title->setObjectName(QStringLiteral("xinkxBroadcastTitle"));
	title->setMaxLength(100);
	form->addRow(QStringLiteral("Название"), title);
	auto *description = new QPlainTextEdit(panel);
	description->setObjectName(QStringLiteral("xinkxBroadcastDescription"));
	description->setMaximumHeight(95);
	description->setPlaceholderText(QStringLiteral("Описание трансляции"));
	description->setPlainText(local->value(key(QStringLiteral("description"))).toString());
	form->addRow(QStringLiteral("Описание"), description);
	auto *tags = new BroadcastTagsEditor(true, panel);
	tags->setObjectName(QStringLiteral("xinkxBroadcastTags"));
	tags->setTags(local->value(key(QStringLiteral("tags"))).toStringList());
	form->addRow(QStringLiteral("Теги Twitch / Kick"), tags);
	auto *youtubeTags = new BroadcastTagsEditor(false, panel);
	youtubeTags->setObjectName(QStringLiteral("xinkxBroadcastYouTubeTags"));
	youtubeTags->setTags(local->value(key(QStringLiteral("youtubeTags"))).toStringList());
	form->addRow(QStringLiteral("Теги YouTube"), youtubeTags);
	auto *game = new QLineEdit(local->value(key(QStringLiteral("game"))).toString(), panel);
	game->setObjectName(QStringLiteral("xinkxBroadcastGame"));
	game->setPlaceholderText(QStringLiteral("Начните вводить название игры…"));
	form->addRow(QStringLiteral("Игра"), game);
	layout->addLayout(form);

	// All three catalogs feed one autocomplete. Platform IDs remain separate
	// hidden state because Twitch game IDs, Kick IDs and YouTube mids differ.
	auto *model = new QStringListModel(panel);
	auto *completer = new QCompleter(model, panel);
	completer->setCaseSensitivity(Qt::CaseInsensitive);
	completer->setFilterMode(Qt::MatchContains);
	completer->setCompletionMode(QCompleter::PopupCompletion);
	game->setCompleter(completer);
	auto *gameStatus = new QLabel(QStringLiteral("Поиск игр: Twitch / Kick"), panel);
	gameStatus->setWordWrap(true);
	layout->addWidget(gameStatus);

	// Provider IDs are separate from the shared game name. YouTube has a
	// public *video category* catalog, but no public catalog of game titles.
	const auto categories = std::make_shared<QHash<QString, QComboBox *>>();
	for (const auto &platform : {QStringLiteral("twitch"), QStringLiteral("kick"), QStringLiteral("youtube")}) {
		auto *combo = new QComboBox(panel);
		if (platform != QStringLiteral("youtube")) combo->hide();
		combo->setObjectName(QStringLiteral("xinkxBroadcast") +
			(platform == QStringLiteral("youtube") ? QStringLiteral("YouTube") :
			platform == QStringLiteral("twitch") ? QStringLiteral("Twitch") : QStringLiteral("Kick")) +
			QStringLiteral("Game"));
		combo->addItem(QStringLiteral("Не выбрано"), QString{});
		choose(combo, local->value(key(platform + QStringLiteral("Category"))).toString(),
			local->value(key(platform + QStringLiteral("CategoryName"))).toString());
		categories->insert(platform, combo);
		if (platform == QStringLiteral("youtube")) form->addRow(QStringLiteral("Категория YouTube"), combo);
	}
	auto *youtubeEvent = new QComboBox(panel);
	youtubeEvent->setObjectName(QStringLiteral("xinkxBroadcastYouTubeEvent"));
	youtubeEvent->addItem(QStringLiteral("Выберите эфир YouTube"), QString{});
	auto *youtubeEventRow = new QWidget(panel);
	auto *youtubeEventLayout = new QHBoxLayout(youtubeEventRow);
	youtubeEventLayout->setContentsMargins(0, 0, 0, 0);
	youtubeEventLayout->addWidget(youtubeEvent, 1);
	auto *checkYouTube = new QPushButton(QStringLiteral("Проверить"), youtubeEventRow);
	checkYouTube->setToolTip(QStringLiteral("Проверить состояние эфира и привязанного видеопотока через YouTube API"));
	youtubeEventLayout->addWidget(checkYouTube);
	form->addRow(QStringLiteral("Эфир YouTube"), youtubeEventRow);
	auto *youtubeHealth = new QLabel(QStringLiteral("YouTube: состояние эфира ещё не проверено"), panel);
	youtubeHealth->setObjectName(QStringLiteral("xinkxYouTubeBroadcastHealth"));
	youtubeHealth->setWordWrap(true);
	youtubeHealth->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(QStringLiteral("Диагностика YouTube"), youtubeHealth);

	auto *save = new QPushButton(QStringLiteral("Сохранить профиль и обновить площадки"), panel);
	save->setObjectName(QStringLiteral("xinkxBroadcastSave"));
	layout->addWidget(save);
	auto *status = new QLabel(QStringLiteral("Изменения отправляются после нажатия «Сохранить»."), panel);
	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(status);
	layout->addStretch();

	auto catalogs = std::make_shared<QHash<QString, QJsonArray>>();
	auto youtubeIds = std::make_shared<QHash<QString, QString>>();
	auto submittedTitles = std::make_shared<QHash<QString, QPair<QString, QString>>>();
	auto youtubeGaming = std::make_shared<QHash<QString, QString>>();
	auto report = std::make_shared<QHash<QString, QString>>();
	auto warnings = std::make_shared<QHash<QString, QString>>();
	auto firstTitle = std::make_shared<bool>(local->contains(key(QStringLiteral("title"))));
	auto firstDescription = std::make_shared<bool>(local->contains(key(QStringLiteral("description"))));
	auto firstGame = std::make_shared<bool>(local->contains(key(QStringLiteral("game"))));
	auto firstTags = std::make_shared<bool>(local->contains(key(QStringLiteral("tags"))));
	auto youtubeCategoryManual = std::make_shared<bool>(local->contains(key(QStringLiteral("youtubeCategory"))));

	const auto youtubeAccount = [=] {
		// The vertical RTMP destination is not another YouTube channel.
		for (const auto &entry : gateway->accounts())
			if (entry.platform == QStringLiteral("youtube") && !entry.destination.portrait) return entry;
		for (const auto &entry : gateway->accounts())
			if (entry.platform == QStringLiteral("youtube")) return entry;
		return StreamAccount{};
	};
	QObject::connect(checkYouTube, &QPushButton::clicked, panel, [=] {
		const auto account = youtubeAccount();
		const QString broadcastId = youtubeEvent->currentData().toString();
		if (account.id.isEmpty() || broadcastId.isEmpty()) {
			youtubeHealth->setText(QStringLiteral("Выбери YouTube-эфир и подключи OAuth."));
			return;
		}
		youtubeHealth->setText(QStringLiteral("YouTube: проверяем привязку и приём сигнала…"));
		gateway->inspectBroadcast(account.id, broadcastId);
	});
	const auto resolveVideo = [=](const QString &accountId) {
		const QString explicitId = youtubeEvent->currentData().toString();
		if (!explicitId.isEmpty()) return explicitId;
		return youtubeIds->value(accountId);
	};
	const auto useGamingCategory = [=] {
		const QString label = youtubeGaming->value(youtubeAccount().id);
		// Never overwrite a category explicitly chosen by the streamer.
		if (*youtubeCategoryManual || game->text().trimmed().isEmpty() || label.isEmpty() ||
		    !categories->value(QStringLiteral("youtube"))->currentData().toString().isEmpty()) return;
		choose(categories->value(QStringLiteral("youtube")), QStringLiteral("20"), label);
		local->setValue(key(QStringLiteral("youtubeCategory")), QStringLiteral("20"));
		local->setValue(key(QStringLiteral("youtubeCategoryName")), label);
		flush->start();
	};
	const auto clearMappings = [=] {
		for (const auto &provider : {QStringLiteral("twitch"), QStringLiteral("kick")}) {
			choose(categories->value(provider), {}, {});
			local->remove(key(provider + QStringLiteral("Category")));
			local->remove(key(provider + QStringLiteral("CategoryName")));
		}
	};
	QObject::connect(categories->value(QStringLiteral("youtube")), &QComboBox::currentIndexChanged,
		panel, [=] {
			*youtubeCategoryManual = true;
			auto *combo = categories->value(QStringLiteral("youtube"));
			local->setValue(key(QStringLiteral("youtubeCategory")), combo->currentData().toString());
			local->setValue(key(QStringLiteral("youtubeCategoryName")), combo->currentText());
			flush->start();
		});
	QObject::connect(youtubeEvent, &QComboBox::currentIndexChanged, panel, [=] {
		local->setValue(key(QStringLiteral("youtubeBroadcast")), youtubeEvent->currentData().toString());
		local->setValue(key(QStringLiteral("youtubeBroadcastName")), youtubeEvent->currentText());
		youtubeHealth->setText(QStringLiteral("Нажми «Проверить» для выбранного эфира."));
		flush->start();
	});
	const auto regenerateSuggestions = [=] {
		QStringList names;
		for (const auto &entries : *catalogs)
			for (const auto &value : entries) {
				const QJsonObject item = value.toObject();
				const QString name = item.value(QStringLiteral("title")).toString(
					item.value(QStringLiteral("name")).toString());
				if (name.isEmpty()) continue;
				bool exists = false;
				for (const auto &other : names)
					if (other.compare(name, Qt::CaseInsensitive) == 0) { exists = true; break; }
				if (!exists) names << name;
			}
		names.sort(Qt::CaseInsensitive);
		model->setStringList(names);
		if (game->hasFocus() && !names.isEmpty()) {
			completer->setCompletionPrefix(game->text());
			completer->complete();
		}
		QStringList sources;
		for (const auto &provider : {QStringLiteral("twitch"), QStringLiteral("kick")})
			if (!catalogs->value(provider).isEmpty()) sources << platformTitle(provider);
		QStringList mapped;
		if (!game->text().trimmed().isEmpty()) {
			for (const auto &provider : {QStringLiteral("twitch"), QStringLiteral("kick")}) {
				const auto *combo = categories->value(provider);
				mapped << platformTitle(provider) + QStringLiteral(": ") +
					(combo->currentData().toString().isEmpty() ? QStringLiteral("не сопоставлена") : combo->currentText());
			}
		}
		gameStatus->setText((sources.isEmpty()
			? QStringLiteral("Игр не найдено либо источник недоступен")
			: QStringLiteral("Найдены игры: ") + sources.join(QStringLiteral(" / "))) +
			(mapped.isEmpty() ? QString{} : QStringLiteral("\n") + mapped.join(QStringLiteral(" · "))));
	};
	const auto matchProvider = [=](const QString &provider, const QString &requested) {
		const QString name = requested.trimmed();
		if (name.isEmpty()) return;
		QString id;
		QString selectedName;
		for (const auto &value : catalogs->value(provider)) {
			const QJsonObject item = value.toObject();
			const QString label = item.value(QStringLiteral("name")).toString();
			if (label.compare(name, Qt::CaseInsensitive)) continue;
			const QString found = item.value(QStringLiteral("id")).toVariant().toString();
			if (found.isEmpty() || found == QStringLiteral("0")) continue;
			// Never guess between multiple IDs for the same visible name.
			if (!id.isEmpty() && id != found) return;
			id = found;
			selectedName = label;
		}
		if (id.isEmpty()) return;
		choose(categories->value(provider), id, selectedName);
		local->setValue(key(provider + QStringLiteral("Category")), id);
		local->setValue(key(provider + QStringLiteral("CategoryName")), selectedName);
		flush->start();
	};
	const auto assignGame = [=](const QString &value) {
		const QString name = value.trimmed();
		if (name.isEmpty()) return;
		clearMappings();
		game->setText(name);
		for (const auto &provider : {QStringLiteral("twitch"), QStringLiteral("kick")})
			matchProvider(provider, name);
		useGamingCategory();
		regenerateSuggestions();
	};
	QObject::connect(completer, qOverload<const QString &>(&QCompleter::activated), panel, assignGame);

	QObject::connect(title, &QLineEdit::textChanged, panel, [=](const QString &value) {
		*firstTitle = true;
		local->setValue(key(QStringLiteral("title")), value);
		flush->start();
	});
	QObject::connect(description, &QPlainTextEdit::textChanged, panel, [=] {
		*firstDescription = true;
		local->setValue(key(QStringLiteral("description")), description->toPlainText());
		flush->start();
	});
	tags->setOnChanged([=](const QStringList &values) {
		*firstTags = true;
		local->setValue(key(QStringLiteral("tags")), values);
		flush->start();
	});
	youtubeTags->setOnChanged([=](const QStringList &values) {
		local->setValue(key(QStringLiteral("youtubeTags")), values);
		flush->start();
	});
	QObject::connect(game, &QLineEdit::textChanged, panel, [=](const QString &value) {
		*firstGame = true;
		local->setValue(key(QStringLiteral("game")), value);
		flush->start();
		QTimer::singleShot(0, panel, regenerateSuggestions);
	});
	QObject::connect(game, &QLineEdit::textEdited, panel, [=] {
		clearMappings();
		useGamingCategory();
		flush->start();
	});

	const auto search = [=] {
		const QString query = game->text().trimmed();
		if (query.size() < 2) return;
		catalogs->clear();
		model->setStringList({});
		for (const auto &account : gateway->accounts())
			if (account.platform == QStringLiteral("twitch") || account.platform == QStringLiteral("kick"))
				gateway->searchCategories(account.id, query);
		gameStatus->setText(QStringLiteral("Поиск «%1»…").arg(query));
	};
	auto *searchDelay = new QTimer(panel);
	searchDelay->setSingleShot(true);
	searchDelay->setInterval(700);
	QObject::connect(game, &QLineEdit::textEdited, panel, [=](const QString &value) {
		if (value.trimmed().size() >= 2) searchDelay->start();
		else searchDelay->stop();
	});
	QObject::connect(searchDelay, &QTimer::timeout, panel, search);
	QObject::connect(game, &QLineEdit::returnPressed, panel, [=] {
		const QString value = game->text().trimmed();
		for (const auto &name : model->stringList())
			if (name.compare(value, Qt::CaseInsensitive) == 0) { assignGame(name); return; }
		search();
	});

	QObject::connect(gateway, &ChatGateway::categoryResults, panel, [=](const QString &id, const QJsonArray &items) {
		QString provider;
		for (const auto &account : gateway->accounts()) if (account.id == id) provider = account.platform;
		if (provider == QStringLiteral("youtube")) {
			const QString previous = categories->value(provider)->currentData().toString();
			const QString saved = local->value(key(QStringLiteral("youtubeCategory"))).toString();
			const QString intended = !previous.isEmpty() ? previous : saved;
			const QSignalBlocker blocked(categories->value(provider));
			categories->value(provider)->clear();
			categories->value(provider)->addItem(QStringLiteral("Не менять категорию"), QString{});
			for (const auto &value : items) {
				const auto item = value.toObject();
				const QString categoryId = item.value(QStringLiteral("id")).toVariant().toString();
				const QString label = item.value(QStringLiteral("name")).toString();
				if (categoryId == QStringLiteral("20")) (*youtubeGaming)[id] = label;
				if (!categoryId.isEmpty()) categories->value(provider)->addItem(label, categoryId);
			}
			choose(categories->value(provider), intended,
				local->value(key(QStringLiteral("youtubeCategoryName"))).toString());
			useGamingCategory();
			return;
		}
		if (provider != QStringLiteral("twitch") && provider != QStringLiteral("kick")) return;
		(*catalogs)[provider] = items;
		matchProvider(provider, game->text());
		regenerateSuggestions();
	});
	QObject::connect(gateway, &ChatGateway::broadcastInfo, panel, [=](const QString &id, const QJsonObject &info) {
		const QString platform = info.value(QStringLiteral("platform")).toString();
		const QString actualTitle = info.value(QStringLiteral("title")).toString();
		const QString actualDescription = info.value(QStringLiteral("description")).toString();
		const QString actualGame = info.value(QStringLiteral("categoryName")).toString();
		if (platform != QStringLiteral("twitch") && platform != QStringLiteral("kick") &&
		    platform != QStringLiteral("youtube")) return;
		if (!*firstTitle && !actualTitle.isEmpty()) title->setText(actualTitle);
		if (!*firstDescription && !actualDescription.isEmpty()) description->setPlainText(actualDescription);
		if (!*firstGame && platform == QStringLiteral("twitch") && !actualGame.isEmpty()) game->setText(actualGame);
		bool twitchConnected = false;
		for (const auto &account : gateway->accounts())
			if (account.platform == QStringLiteral("twitch")) { twitchConnected = true; break; }
		if (!*firstTags && (platform == QStringLiteral("twitch") ||
			(platform == QStringLiteral("kick") && !twitchConnected))) {
			const QJsonArray remote = info.value(QStringLiteral("tags")).toArray();
			QStringList incoming;
			for (const QJsonValue &value : remote) if (value.isString()) incoming << value.toString();
			if (!incoming.isEmpty()) tags->setTags(incoming);
		}
		if (platform != QStringLiteral("youtube")) {
			const QString actualId = info.value(QStringLiteral("categoryId")).toVariant().toString();
			if (!actualId.isEmpty() && categories->value(platform)->currentData().toString().isEmpty())
				choose(categories->value(platform), actualId, actualGame);
			return;
		}
		const QString selectedId = info.value(QStringLiteral("id")).toString();
		if (selectedId.isEmpty()) youtubeIds->remove(id);
		else (*youtubeIds)[id] = selectedId;
		const QSignalBlocker blocked(youtubeEvent);
		const QString previously = !youtubeEvent->currentData().toString().isEmpty()
			? youtubeEvent->currentData().toString()
			: local->value(key(QStringLiteral("youtubeBroadcast"))).toString();
		youtubeEvent->clear();
		youtubeEvent->addItem(QStringLiteral("Выберите эфир YouTube"), QString{});
		for (const auto &entry : info.value(QStringLiteral("broadcasts")).toArray()) {
			const auto item = entry.toObject();
			youtubeEvent->addItem(item.value(QStringLiteral("title")).toString(), item.value(QStringLiteral("id")).toString());
		}
		// A saved event can be completed or deleted. Never silently keep its
		// stale ID when a different live broadcast is now selected.
		const int previousIndex = youtubeEvent->findData(previously);
		const int activeIndex = youtubeEvent->findData(selectedId);
		if (previousIndex > 0) youtubeEvent->setCurrentIndex(previousIndex);
		else if (activeIndex > 0) youtubeEvent->setCurrentIndex(activeIndex);
		const QString currentId = youtubeEvent->currentData().toString();
		if (!currentId.isEmpty() && currentId == selectedId) {
			QStringList diagnostics;
			const QString lifecycle = info.value(QStringLiteral("lifecycle")).toString();
			diagnostics << QStringLiteral("Эфир: %1").arg(lifecycle.isEmpty() ? QStringLiteral("неизвестно") : lifecycle);
			const QString boundStreamId = info.value(QStringLiteral("boundStreamId")).toString();
			if (boundStreamId.isEmpty())
				diagnostics << QStringLiteral("Нет привязанного видеопотока. Передача RTMP не запустит этот эфир.");
			else {
				const QString ingest = info.value(QStringLiteral("ingestStatus")).toString();
				diagnostics << QStringLiteral("Приём YouTube: %1").arg(ingest.isEmpty()
					? QStringLiteral("не проверен") : ingest);
				if (info.contains(QStringLiteral("configuredStreamMatch")) &&
				    !info.value(QStringLiteral("configuredStreamMatch")).toBool())
					diagnostics << QStringLiteral("ОШИБКА: выбранный в XINKX видеопоток не привязан к эфиру.");
				if (info.contains(QStringLiteral("ingestKeyMatch")) &&
				    !info.value(QStringLiteral("ingestKeyMatch")).toBool())
					diagnostics << QStringLiteral("ОШИБКА: RTMP-ключ XINKX не совпадает с ключом эфира.");
				if (ingest == QStringLiteral("active") && lifecycle != QStringLiteral("live"))
					diagnostics << QStringLiteral("YouTube получает сигнал, но эфир ещё не Live. Проверь автозапуск/«Начать эфир».");
				const QString ingestError = info.value(QStringLiteral("ingestCheckError")).toString();
				if (!ingestError.isEmpty()) diagnostics << QStringLiteral("Проверка: ") + ingestError;
			}
			youtubeHealth->setText(diagnostics.join(QLatin1Char('\n')));
		}
		local->setValue(key(QStringLiteral("youtubeBroadcast")), youtubeEvent->currentData().toString());
		local->setValue(key(QStringLiteral("youtubeBroadcastName")), youtubeEvent->currentText());
		flush->start();
	});

	QObject::connect(gateway, &ChatGateway::broadcastResult, panel, [=](const QString &id, const QString &message, bool success) {
		writeDiagnostic(QStringLiteral("broadcast:%1:%2").arg(id, success ? QStringLiteral("ok") : QStringLiteral("error")), message);
		(*report)[id] = message + warnings->value(id);
		QStringList lines;
		for (const auto &account : gateway->accounts())
			if (report->contains(account.id)) lines << report->value(account.id);
		status->setText(lines.join(QLatin1Char('\n')));
		if (submittedTitles->contains(id)) {
			const auto saved = submittedTitles->take(id);
			if (success) {
				// Reflect the title actually submitted, even if the editor was
				// changed while the remote update was in flight.
				const int index = youtubeEvent->findData(saved.first);
				if (index > 0) {
					youtubeEvent->setItemText(index, saved.second);
					local->setValue(key(QStringLiteral("youtubeBroadcastName")), saved.second);
					flush->start();
				}
			}
		}
	});

	// Installed after GameProfilesWorkspace connects the same button; that
	// handler persists the chosen profile and activates its OBS scene first.
	QTimer::singleShot(0, panel, [=] {
		QObject::connect(save, &QPushButton::clicked, panel, [=] {
			if (save->property("xinkxProfileReady").isValid() &&
			    !save->property("xinkxProfileReady").toBool()) return;
			local->sync();
			report->clear();
			warnings->clear();
			int sent = 0;
			QSet<QString> processedYouTube;
			const QString name = game->text().trimmed();
			for (const auto &entry : gateway->accounts()) {
				if (entry.platform != QStringLiteral("twitch") && entry.platform != QStringLiteral("kick") &&
				    entry.platform != QStringLiteral("youtube")) continue;
				QString eventId;
				if (entry.platform == QStringLiteral("youtube")) {
					if (entry.destination.portrait && !entry.providerId.isEmpty()) {
						bool hasMainOutput = false;
						for (const auto &other : gateway->accounts())
							if (other.id != entry.id && other.platform == QStringLiteral("youtube") &&
							    !other.destination.portrait && other.providerId == entry.providerId)
								hasMainOutput = true;
						if (hasMainOutput) continue;
					}
					const QString channel = !entry.providerId.isEmpty() ? entry.providerId : entry.id;
					if (processedYouTube.contains(channel)) continue;
					processedYouTube.insert(channel);
					eventId = resolveVideo(entry.id);
					if (eventId.isEmpty()) {
						(*report)[entry.id] = QStringLiteral("YouTube: нет однозначного эфира — данные не изменены");
						continue;
					}
					(*youtubeIds)[entry.id] = eventId;
					(*submittedTitles)[entry.id] = {eventId, title->text().trimmed().left(100)};
				}
				QString category = categories->value(entry.platform)->currentData().toString();
				if (!name.isEmpty() && category.isEmpty() && entry.platform != QStringLiteral("youtube"))
					(*warnings)[entry.id] = QStringLiteral(" · игра не найдена на площадке, категория не менялась");
				const bool applyTags = entry.platform == QStringLiteral("youtube")
					? local->contains(key(QStringLiteral("youtubeTags")))
					: local->contains(key(QStringLiteral("tags")));
				gateway->updateBroadcast(entry.id, eventId, title->text(), description->toPlainText(), category,
					entry.platform == QStringLiteral("youtube") ? youtubeTags->tags() : tags->tags(), applyTags);
				++sent;
			}
			QStringList notes;
			for (const auto &value : *report) notes << value;
			status->setText(QStringLiteral("Обновляются площадки: %1").arg(sent) +
				(notes.isEmpty() ? QString{} : QStringLiteral("\n") + notes.join(QLatin1Char('\n'))));
		});
	});

	QSet<QString> inspectedChannels;
	for (const auto &entry : gateway->accounts()) {
		if (entry.platform == QStringLiteral("youtube")) {
			const QString channel = entry.providerId.isEmpty() ? entry.id : entry.providerId;
			if (inspectedChannels.contains(channel)) continue;
			inspectedChannels.insert(channel);
		}
		gateway->inspectBroadcast(entry.id);
		if (entry.platform == QStringLiteral("youtube")) gateway->searchCategories(entry.id, {});
	}
	// Never open an isolated Chrome instance automatically at startup.

	auto *scroll = new QScrollArea(dock);
	scroll->setObjectName(QStringLiteral("xinkxBroadcastEditor"));
	scroll->setWidgetResizable(true);
	scroll->setWidget(panel);
	studioTabs(mainWindow)->addTab(scroll, QStringLiteral("Эфир"));
}

} // namespace xinkx
