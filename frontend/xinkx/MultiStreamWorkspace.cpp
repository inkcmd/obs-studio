#include "MultiStreamWorkspace.hpp"

#include "MultiStreamManager.hpp"

#include <widgets/OBSBasic.hpp>

#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <memory>
#include <utility>

namespace xinkx {

namespace {

constexpr const char *platformIds[] = {"twitch", "youtube", "kick"};
constexpr const char *platformLabels[] = {"Twitch", "YouTube", "Kick"};

struct DestinationControls {
	QLineEdit *url = nullptr;
	QLineEdit *key = nullptr;
	QSpinBox *bitrate = nullptr;
	QSpinBox *width = nullptr;
	QSpinBox *height = nullptr;
	QComboBox *encoder = nullptr;
	QLabel *status = nullptr;
	QPushButton *toggle = nullptr;
};

using Destinations = std::array<DestinationControls, 3>;

QSpinBox *makeSpin(QWidget *parent, int min, int max, int step, int value)
{
	auto *spin = new QSpinBox(parent);
	spin->setRange(min, max);
	spin->setSingleStep(step);
	spin->setValue(value);
	spin->setKeyboardTracking(false);
	return spin;
}

StreamDestination currentDestination(const DestinationControls &controls)
{
	StreamDestination result;
	result.url = controls.url->text();
	result.key = controls.key->text();
	result.bitrateKbps = controls.bitrate->value();
	result.width = controls.width->value();
	result.height = controls.height->value();
	result.encoder = controls.encoder->currentData().toString();
	return result;
}

} // namespace

QDockWidget *installMultiStreamWorkspace(OBSBasic *mainWindow, QDockWidget *inspector, QSettings *settings,
						 QLabel *multiStatus)
{
	auto *manager = new MultiStreamManager(mainWindow);
	auto updateGlobalStatus = [manager, multiStatus] {
		int count = 0;
		for (int i = 0; i < 3; ++i) {
			count += manager->active(i) ? 1 : 0;
		}
		multiStatus->setText(QStringLiteral("MULTI  %1/3").arg(count));
		multiStatus->setStyleSheet(count > 0 ? QStringLiteral("color: #58dea2;") :
					       QStringLiteral("color: #94a0b5;"));
	};
	QObject::connect(manager, &MultiStreamManager::destinationChanged, mainWindow,
			 [updateGlobalStatus](int) { updateGlobalStatus(); });
	updateGlobalStatus();
	auto *dock = new QDockWidget(QStringLiteral("Мультистрим"), mainWindow);
	dock->setObjectName(QStringLiteral("xinkxMultiStreamDock"));
	dock->setAllowedAreas(Qt::RightDockWidgetArea);
	dock->setFeatures(dock->features() & ~QDockWidget::DockWidgetFloatable);

	auto *content = new QWidget(dock);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);
	auto *intro = new QLabel(QStringLiteral("Прямые выходы в интернет"), content);
	intro->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px;"));
	layout->addWidget(intro);
	auto *note = new QLabel(QStringLiteral("Каждая платформа имеет собственный H.264/AAC энкодер, "
						  "разрешение и битрейт. Без локального RTMP-сервера."), content);
	note->setWordWrap(true);
	note->setStyleSheet(QStringLiteral("color: #9da9bd;"));
	layout->addWidget(note);

	auto *tabs = new QTabWidget(content);
	layout->addWidget(tabs, 1);
	auto controls = std::make_shared<Destinations>();
	const QString defaults[] = {QStringLiteral("rtmps://live.twitch.tv/app"),
				    QStringLiteral("rtmps://a.rtmps.youtube.com/live2"), QString()};
	const int defaultWidths[] = {1920, 2560, 1280};
	const int defaultHeights[] = {1080, 1440, 720};
	const int defaultBitrates[] = {6000, 9000, 4500};

	for (int i = 0; i < 3; ++i) {
		const QString configPrefix = QStringLiteral("multistream/%1/").arg(QString::fromLatin1(platformIds[i]));
		const auto restore = [settings, configPrefix](const char *key, const QVariant &fallback) -> QVariant {
			return settings ? settings->value(configPrefix + QString::fromLatin1(key), fallback) : fallback;
		};
		auto *page = new QWidget(tabs);
		auto *pageLayout = new QVBoxLayout(page);
		pageLayout->setContentsMargins(5, 12, 5, 5);
		pageLayout->setSpacing(12);
		auto *form = new QFormLayout();
		form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
		form->setSpacing(8);
		DestinationControls &ui = (*controls)[i];
		ui.url = new QLineEdit(page);
		ui.url->setPlaceholderText(QStringLiteral("rtmps://..."));
		ui.url->setText(restore("url", defaults[i]).toString());
		form->addRow(QStringLiteral("RTMP(S) URL"), ui.url);
		ui.key = new QLineEdit(page);
		ui.key->setEchoMode(QLineEdit::Password);
		ui.key->setPlaceholderText(QStringLiteral("Ключ трансляции"));
		form->addRow(QStringLiteral("Ключ"), ui.key);
		ui.bitrate = makeSpin(page, 500, 51000, 500,
					  restore("bitrate", defaultBitrates[i]).toInt());
		ui.bitrate->setSuffix(QStringLiteral(" kbps"));
		form->addRow(QStringLiteral("Видео"), ui.bitrate);
		ui.width = makeSpin(page, 320, 7680, 2, restore("width", defaultWidths[i]).toInt());
		ui.height = makeSpin(page, 240, 4320, 2, restore("height", defaultHeights[i]).toInt());
		form->addRow(QStringLiteral("Ширина"), ui.width);
		form->addRow(QStringLiteral("Высота"), ui.height);
		ui.encoder = new QComboBox(page);
		ui.encoder->addItem(QStringLiteral("Авто (NVENC → x264)"), QStringLiteral("auto"));
		ui.encoder->addItem(QStringLiteral("NVIDIA NVENC H.264"), QStringLiteral("nvenc"));
		ui.encoder->addItem(QStringLiteral("x264 (CPU)"), QStringLiteral("x264"));
		const int encoderIndex = ui.encoder->findData(restore("encoder", QStringLiteral("auto")));
		if (encoderIndex >= 0) {
			ui.encoder->setCurrentIndex(encoderIndex);
		}
		form->addRow(QStringLiteral("Кодер"), ui.encoder);
		pageLayout->addLayout(form);
		ui.toggle = new QPushButton(QStringLiteral("Запустить"), page);
		pageLayout->addWidget(ui.toggle);
		ui.status = new QLabel(QStringLiteral("Остановлен"), page);
		ui.status->setWordWrap(true);
		ui.status->setStyleSheet(QStringLiteral("color: #94a0b5;"));
		pageLayout->addWidget(ui.status);
		pageLayout->addStretch();
		tabs->addTab(page, QString::fromLatin1(platformLabels[i]));
		// Keys are deliberately never saved. Settings only hold public endpoints and encode parameters.
		if (settings) {
			QObject::connect(ui.url, &QLineEdit::editingFinished, mainWindow, [settings, configPrefix, line = ui.url] {
				settings->setValue(configPrefix + QStringLiteral("url"), line->text());
			});
			for (const auto &field : {std::pair{ui.bitrate, "bitrate"}, std::pair{ui.width, "width"},
						       std::pair{ui.height, "height"}}) {
				auto *spin = field.first;
				const char *key = field.second;
				QObject::connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), mainWindow,
						 [settings, configPrefix, key](int value) {
							 settings->setValue(configPrefix + QString::fromLatin1(key), value);
						 });
			}
			QObject::connect(ui.encoder, QOverload<int>::of(&QComboBox::currentIndexChanged), mainWindow,
					 [settings, configPrefix, combo = ui.encoder](int) {
						 settings->setValue(configPrefix + QStringLiteral("encoder"), combo->currentData());
					 });
		}
	}

	auto *keysNote = new QLabel(QStringLiteral("Ключи не записываются на диск: их нужно ввести "
						   "снова после перезапуска."), content);
	keysNote->setWordWrap(true);
	keysNote->setStyleSheet(QStringLiteral("color: #9da9bd;"));
	layout->addWidget(keysNote);

	auto *globalButtons = new QHBoxLayout();
	auto *startAll = new QPushButton(QStringLiteral("Запустить все"), content);
	auto *stopAll = new QPushButton(QStringLiteral("Остановить все"), content);
	globalButtons->addWidget(startAll);
	globalButtons->addWidget(stopAll);
	layout->addLayout(globalButtons);
	dock->setWidget(content);
	mainWindow->AddDockWidget(dock, Qt::RightDockWidgetArea);

	auto update = [manager, controls](int i) {
		auto &ui = (*controls)[i];
		const bool live = manager->active(i);
		ui.toggle->setText(live ? QStringLiteral("Остановить") : QStringLiteral("Запустить"));
		for (QWidget *field : {static_cast<QWidget *>(ui.url), static_cast<QWidget *>(ui.key),
					     static_cast<QWidget *>(ui.bitrate), static_cast<QWidget *>(ui.width),
					     static_cast<QWidget *>(ui.height), static_cast<QWidget *>(ui.encoder)}) {
			field->setEnabled(!live);
		}
		ui.status->setText(manager->description(i));
		ui.status->setStyleSheet(live ? QStringLiteral("color: #58dea2;") :
					      QStringLiteral("color: #94a0b5;"));
	};

	auto start = [manager, controls, update](int i) {
		QString error;
		const bool started = manager->start(i, currentDestination((*controls)[i]), &error);
		update(i);
		if (!started && !error.isEmpty()) {
			(*controls)[i].status->setText(error);
		}
	};
	for (int i = 0; i < 3; ++i) {
		QObject::connect((*controls)[i].toggle, &QPushButton::clicked, mainWindow,
				 [manager, start, i] {
					 if (manager->active(i)) {
						 manager->stop(i);
					 } else {
						 start(i);
					 }
				 });
		QObject::connect(manager, &MultiStreamManager::destinationChanged, mainWindow, [update, i](int changed) {
			if (changed == i) {
				update(i);
			}
		});
	}
	QObject::connect(startAll, &QPushButton::clicked, mainWindow, [manager, start] {
		for (int i = 0; i < 3; ++i) {
			if (!manager->active(i)) {
				start(i);
			}
		}
	});
	QObject::connect(stopAll, &QPushButton::clicked, mainWindow, [manager] {
		for (int i = 0; i < 3; ++i) {
			manager->stop(i);
		}
	});
	// Group both XINKX controls in one native sidebar rather than adding a third column.
	QTimer::singleShot(0, mainWindow, [mainWindow, inspector, dock] {
		mainWindow->tabifyDockWidget(inspector, dock);
		if (inspector->isVisible()) {
			inspector->raise();
		} else {
			dock->raise();
		}
	});
	return dock;
}

} // namespace xinkx
