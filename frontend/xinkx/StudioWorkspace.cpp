#include "StudioWorkspace.hpp"

#include "StudioController.hpp"
#include "MultiStreamManager.hpp"
#include "OverlayRuntime.hpp"
#include "ChatGateway.hpp"
#include "PresentationSettings.hpp"
#include "DonationGateway.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <QDir>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>

namespace xinkx {

void installBroadcastWorkspace(OBSBasic *mainWindow);
void installGameProfilesWorkspace(OBSBasic *mainWindow);
void installSceneJsonWorkspace(OBSBasic *mainWindow);

namespace {

struct InspectorState {
	qint64 itemId = -1;
	double ratio = 1.0;
	SelectedTransform current;
};

QDoubleSpinBox *makeDimensionField(QWidget *parent, bool sizeField)
{
	auto *spin = new QDoubleSpinBox(parent);
	spin->setDecimals(1);
	spin->setRange(sizeField ? 1.0 : -32768.0, 32768.0);
	spin->setSingleStep(1.0);
	spin->setKeyboardTracking(false);
	spin->setEnabled(false);
	return spin;
}

} // namespace

QDockWidget *studioDock(OBSBasic *mainWindow)
{
	return mainWindow->findChild<QDockWidget *>(QStringLiteral("xinkxStudioDock"));
}

QTabWidget *studioTabs(OBSBasic *mainWindow)
{
	auto *dock = studioDock(mainWindow);
	return dock ? dock->findChild<QTabWidget *>(QStringLiteral("xinkxStudioTabs")) : nullptr;
}

void installStudioWorkspace(OBSBasic *mainWindow)
{
	auto *controller = new StudioController(mainWindow);
	auto *manager = new MultiStreamManager(mainWindow);
	auto *extraStatus = new QLabel(mainWindow);
	mainWindow->statusBar()->addPermanentWidget(extraStatus);
	auto updateExtraStatus = [extraStatus, manager, mainWindow] {
		const int active = manager->activeCount();
		const int count = manager->configuredCount();
		auto text = QStringLiteral("Выходы: %1/%2").arg(active).arg(count);
		if (active) {
			text += QStringLiteral(" · %1 кбит/с · пропущено: %2")
					.arg(manager->extraBitrate()).arg(manager->totalDroppedFrames());
		}
		extraStatus->setText(text);
		extraStatus->setStyleSheet(active > 0 ? QStringLiteral("color: #58dea2;") : QString());
		extraStatus->setToolTip(manager->sessionDetails().toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
	};
	QObject::connect(manager, &MultiStreamManager::statusChanged, mainWindow, updateExtraStatus);
	updateExtraStatus();

	// Inspector edits real OBS scene items; standard source dialogs stay available.
	auto *inspector = new QDockWidget(QStringLiteral("Источник"), mainWindow);
	inspector->setObjectName(QStringLiteral("xinkxSourceInspector"));
	inspector->setAllowedAreas(Qt::RightDockWidgetArea);

	auto *scroll = new QScrollArea(inspector);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *content = new QWidget(scroll);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(14, 16, 14, 14);
	layout->setSpacing(12);

	auto *sourceTitle = new QLabel(QStringLiteral("Источник не выбран"), content);
	sourceTitle->setWordWrap(true);
	sourceTitle->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px;"));
	layout->addWidget(sourceTitle);

	auto *geometryLabel = new QLabel(QStringLiteral("Положение и размер"), content);
	geometryLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
	layout->addWidget(geometryLabel);

	auto *form = new QFormLayout();
	form->setSpacing(8);
	auto *x = makeDimensionField(content, false);
	auto *y = makeDimensionField(content, false);
	auto *width = makeDimensionField(content, true);
	auto *height = makeDimensionField(content, true);
	form->addRow(QStringLiteral("X"), x);
	form->addRow(QStringLiteral("Y"), y);
	form->addRow(QStringLiteral("Ширина"), width);
	form->addRow(QStringLiteral("Высота"), height);
	layout->addLayout(form);

	auto *keepAspect = new QCheckBox(QStringLiteral("Сохранять пропорции"), content);
	keepAspect->setChecked(true);
	layout->addWidget(keepAspect);
	// This is OBS's own preview snapping setting, not a separate XINKX scene model.
	auto *snapping = new QCheckBox(QStringLiteral("Магнитные направляющие OBS"), content);
	snapping->setToolTip(QStringLiteral("Привязка к краям холста, центру и другим источникам при перетаскивании."));
	snapping->setChecked(config_get_bool(App()->GetUserConfig(), "BasicWindow", "SnappingEnabled"));
	layout->addWidget(snapping);
	QObject::connect(snapping, &QCheckBox::toggled, mainWindow, [](bool enabled) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SnappingEnabled", enabled);
	});
	auto *snapSync = new QTimer(mainWindow);
	snapSync->setInterval(1000);
	QObject::connect(snapSync, &QTimer::timeout, mainWindow, [snapping] {
		const bool enabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SnappingEnabled");
		if (snapping->isChecked() != enabled) {
			const QSignalBlocker blocker(snapping);
			snapping->setChecked(enabled);
		}
	});
	snapSync->start();

	auto *rotate = makeDimensionField(content, false);
	rotate->setRange(-360.0, 360.0);
	rotate->setSuffix(QStringLiteral("°"));
	auto *angleForm = new QFormLayout();
	angleForm->addRow(QStringLiteral("Поворот"), rotate);
	layout->addLayout(angleForm);

	auto *cropTitle = new QLabel(QStringLiteral("Обрезка · пиксели источника"), content);
	cropTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
	layout->addWidget(cropTitle);
	auto *cropGrid = new QGridLayout();
	auto createCrop = [content] {
		auto *field = new QSpinBox(content);
		field->setRange(0, 32768);
		field->setKeyboardTracking(false);
		field->setEnabled(false);
		return field;
	};
	auto *cropLeft = createCrop();
	auto *cropTop = createCrop();
	auto *cropRight = createCrop();
	auto *cropBottom = createCrop();
	cropGrid->addWidget(new QLabel(QStringLiteral("Слева"), content), 0, 0);
	cropGrid->addWidget(cropLeft, 0, 1);
	cropGrid->addWidget(new QLabel(QStringLiteral("Сверху"), content), 1, 0);
	cropGrid->addWidget(cropTop, 1, 1);
	cropGrid->addWidget(new QLabel(QStringLiteral("Справа"), content), 2, 0);
	cropGrid->addWidget(cropRight, 2, 1);
	cropGrid->addWidget(new QLabel(QStringLiteral("Снизу"), content), 3, 0);
	cropGrid->addWidget(cropBottom, 3, 1);
	layout->addLayout(cropGrid);

	auto *fit = new QPushButton(QStringLiteral("Вписать в холст"), content);
	auto *center = new QPushButton(QStringLiteral("В центр"), content);
	auto *hCenter = new QPushButton(QStringLiteral("По горизонтали"), content);
	auto *vCenter = new QPushButton(QStringLiteral("По вертикали"), content);
	auto *alignmentGrid = new QGridLayout();
	alignmentGrid->addWidget(fit, 0, 0);
	alignmentGrid->addWidget(center, 0, 1);
	alignmentGrid->addWidget(hCenter, 1, 0);
	alignmentGrid->addWidget(vCenter, 1, 1);
	layout->addLayout(alignmentGrid);
	auto *presetTitle = new QLabel(QStringLiteral("Готовые композиции"), content);
	presetTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
	layout->addWidget(presetTitle);
	auto *preset = new QComboBox(content);
	preset->addItems({QStringLiteral("Камера · снизу справа"), QStringLiteral("Камера · снизу слева"),
			  QStringLiteral("Камера · сверху справа"), QStringLiteral("Камера · сверху слева"),
			  QStringLiteral("Половина холста · слева"), QStringLiteral("Половина холста · справа")});
	layout->addWidget(preset);
	auto *margin = new QSpinBox(content);
	margin->setRange(0, 300);
	margin->setValue(32);
	margin->setSuffix(QStringLiteral(" px"));
	margin->setKeyboardTracking(false);
	auto *presetOptions = new QFormLayout();
	presetOptions->addRow(QStringLiteral("Отступ"), margin);
	layout->addLayout(presetOptions);
	auto *applyPreset = new QPushButton(QStringLiteral("Применить композицию"), content);
	applyPreset->setEnabled(false);
	layout->addWidget(applyPreset);
	auto *copyLayout = new QPushButton(QStringLiteral("Копировать размещение"), content);
	auto *pasteLayout = new QPushButton(QStringLiteral("Вставить размещение"), content);
	copyLayout->setEnabled(false);
	pasteLayout->setEnabled(false);
	auto *clipboardRow = new QHBoxLayout();
	clipboardRow->addWidget(copyLayout);
	clipboardRow->addWidget(pasteLayout);
	layout->addLayout(clipboardRow);

	auto *lockedHint = new QLabel(content);
	lockedHint->setWordWrap(true);
	lockedHint->setStyleSheet(QStringLiteral("color: #9da9bd;"));
	layout->addWidget(lockedHint);

	auto *openProperties = new QPushButton(QStringLiteral("Свойства источника"), content);
	openProperties->setEnabled(false);
	layout->addWidget(openProperties);
	auto *overlaySource = new QPushButton(QStringLiteral("Добавить / выделить чат и эффекты"), content);
	overlaySource->setToolTip(QStringLiteral("XINKX-слой добавляется в текущую сцену как обычный источник OBS. Размер и положение редактируются в предпросмотре."));
	layout->addWidget(overlaySource);
	QObject::connect(overlaySource, &QPushButton::clicked, mainWindow, [mainWindow] {
		auto *runtime = mainWindow->findChild<OverlayRuntime *>(QStringLiteral("xinkxOverlayRuntime"));
		QString message;
		if (runtime) runtime->addToScene(mainWindow, &message);
		else message = QStringLiteral("Слой XINKX недоступен");
		mainWindow->statusBar()->showMessage(message, 7000);
	});
	scroll->setWidget(content);
	inspector->setWidget(scroll);
	mainWindow->AddDockWidget(inspector, Qt::RightDockWidgetArea);
	inspector->setFeatures(inspector->features() & ~QDockWidget::DockWidgetFloatable);

	auto editor = std::make_shared<InspectorState>();
	QObject::connect(applyPreset, &QPushButton::clicked, mainWindow,
			 [controller, editor, preset, margin, mainWindow] {
				 const auto &item = editor->current;
				 if (item.itemId < 0 || item.locked || !item.resizable || item.height <= 0) {
					 return;
				 }
				 obs_video_info video{};
				 if (!obs_get_video_info(&video) || !video.base_width || !video.base_height) {
					 return;
				 }
				 const double canvasW = video.base_width;
				 const double canvasH = video.base_height;
				 const double inset = margin->value();
				 const double ratio = std::clamp(item.width / item.height, 0.25, 4.0);
				 const int choice = preset->currentIndex();
				 const bool pip = choice < 4;
				 const double maxW = pip ? std::min(canvasW * 0.29, canvasW - 2 * inset)
							 : canvasW / 2 - 2 * inset;
				 const double maxH = pip ? std::min(canvasH * 0.35, canvasH - 2 * inset)
							 : canvasH - 2 * inset;
				 if (maxW <= 0 || maxH <= 0) {
					 return;
				 }
				 const double targetW = std::min(maxW, maxH * ratio);
				 const double targetH = targetW / ratio;
				 double targetX;
				 double targetY;
				 if (pip) {
					 const bool right = choice == 0 || choice == 2;
					 const bool bottom = choice == 0 || choice == 1;
					 targetX = right ? canvasW - inset - targetW : inset;
					 targetY = bottom ? canvasH - inset - targetH : inset;
				 } else {
					 const double halfX = choice == 5 ? canvasW / 2 : 0;
					 targetX = halfX + (canvasW / 2 - targetW) / 2;
					 targetY = (canvasH - targetH) / 2;
				 }
				 const bool applied = controller->applySelectedTransform(item.itemId, targetX, targetY,
									 targetW, targetH);
				 if (applied) {
					 mainWindow->statusBar()->showMessage(
						 QStringLiteral("Композиция применена; отмена через Ctrl+Z"), 4000);
				 }
			 });
	const QString transformPrefix = QStringLiteral("XINKX_TRANSFORM:");
	QObject::connect(copyLayout, &QPushButton::clicked, mainWindow, [editor, transformPrefix] {
		const auto &value = editor->current;
		if (value.itemId < 0) {
			return;
		}
		QJsonObject layout{{QStringLiteral("version"), 1}, {QStringLiteral("x"), value.x},
				   {QStringLiteral("y"), value.y}, {QStringLiteral("width"), value.width},
				   {QStringLiteral("height"), value.height}, {QStringLiteral("rotation"), value.rotation},
				   {QStringLiteral("cropLeft"), value.cropLeft}, {QStringLiteral("cropTop"), value.cropTop},
				   {QStringLiteral("cropRight"), value.cropRight}, {QStringLiteral("cropBottom"), value.cropBottom}};
		QGuiApplication::clipboard()->setText(transformPrefix +
						  QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact)));
	});
	QObject::connect(pasteLayout, &QPushButton::clicked, mainWindow, [editor, controller, mainWindow, transformPrefix] {
		const QString text = QGuiApplication::clipboard()->text();
		if (editor->itemId < 0 || editor->current.locked || !text.startsWith(transformPrefix) || text.size() > 8192) {
			mainWindow->statusBar()->showMessage(QStringLiteral("Буфер не содержит размещение XINKX"), 4000);
			return;
		}
		const auto doc = QJsonDocument::fromJson(text.mid(transformPrefix.size()).toUtf8());
		const auto value = doc.object();
		if (!doc.isObject() || value.value(QStringLiteral("version")).toInt() != 1 ||
		    !value.value(QStringLiteral("width")).isDouble() || !value.value(QStringLiteral("height")).isDouble()) {
			mainWindow->statusBar()->showMessage(QStringLiteral("Неверный формат размещения"), 4000);
			return;
		}
		const bool moved = controller->applySelectedTransform(
			editor->itemId, value.value(QStringLiteral("x")).toDouble(), value.value(QStringLiteral("y")).toDouble(),
			value.value(QStringLiteral("width")).toDouble(), value.value(QStringLiteral("height")).toDouble());
		const bool cropped = controller->applySelectedCropRotation(
			editor->itemId, value.value(QStringLiteral("rotation")).toDouble(),
			value.value(QStringLiteral("cropLeft")).toInt(), value.value(QStringLiteral("cropTop")).toInt(),
			value.value(QStringLiteral("cropRight")).toInt(), value.value(QStringLiteral("cropBottom")).toInt());
		mainWindow->statusBar()->showMessage(
			moved && cropped ? QStringLiteral("Размещение применено к выбранному источнику")
						 : QStringLiteral("Часть параметров не подходит для выбранного источника"), 4000);
	});
	auto apply = [controller, editor, x, y, width, height] {
		if (editor->itemId < 0) {
			return;
		}
		controller->applySelectedTransform(editor->itemId, x->value(), y->value(), width->value(),
						   height->value());
	};
	for (auto *field : {x, y}) {
		QObject::connect(field, &QDoubleSpinBox::editingFinished, mainWindow, apply);
	}
	QObject::connect(width, &QDoubleSpinBox::editingFinished, mainWindow,
			 [editor, width, height, keepAspect, apply] {
				 if (keepAspect->isChecked() && editor->ratio > 0.001) {
					 const QSignalBlocker blocker(height);
					 height->setValue(width->value() / editor->ratio);
				 }
				 apply();
			 });
	QObject::connect(height, &QDoubleSpinBox::editingFinished, mainWindow,
			 [editor, width, height, keepAspect, apply] {
				 if (keepAspect->isChecked() && editor->ratio > 0.001) {
					 const QSignalBlocker blocker(width);
					 width->setValue(height->value() * editor->ratio);
				 }
				 apply();
			 });
	auto applyCropRotation = [controller, editor, rotate, cropLeft, cropTop, cropRight, cropBottom] {
		if (editor->itemId < 0) {
			return;
		}
		controller->applySelectedCropRotation(editor->itemId, rotate->value(), cropLeft->value(),
						      cropTop->value(), cropRight->value(), cropBottom->value());
	};
	QObject::connect(rotate, &QDoubleSpinBox::editingFinished, mainWindow, applyCropRotation);
	for (auto *field : {cropLeft, cropTop, cropRight, cropBottom}) {
		QObject::connect(field, &QSpinBox::editingFinished, mainWindow, applyCropRotation);
	}
	for (const auto &action : {
		     std::pair{fit, "on_actionFitToScreen_triggered"},
		     std::pair{center, "on_actionCenterToScreen_triggered"},
		     std::pair{hCenter, "on_actionHorizontalCenter_triggered"},
		     std::pair{vCenter, "on_actionVerticalCenter_triggered"},
	     }) {
		const char *slot = action.second;
		auto *button = action.first;
		QObject::connect(button, &QPushButton::clicked, mainWindow, [mainWindow, slot] {
			QMetaObject::invokeMethod(mainWindow, slot, Qt::DirectConnection);
		});
	}

	QObject::connect(openProperties, &QPushButton::clicked, mainWindow, [mainWindow] {
		mainWindow->actionOpenSourceProperties();
	});

	QObject::connect(controller, &StudioController::stateChanged, mainWindow,
		[sourceTitle, openProperties](const QString &, const QString &selectedSource, bool, bool) {
			sourceTitle->setText(selectedSource.isEmpty() ? QStringLiteral("Источник не выбран") :
									 selectedSource);
			openProperties->setEnabled(!selectedSource.isEmpty());
		});

	QObject::connect(controller, &StudioController::selectedTransformChanged, mainWindow,
		[editor, x, y, width, height, rotate, cropLeft, cropTop, cropRight, cropBottom, fit, center,
		 hCenter, vCenter, applyPreset, copyLayout, pasteLayout, lockedHint, openProperties](const SelectedTransform &transform) {
			const bool selectedChanged = editor->itemId != transform.itemId;
			if (selectedChanged) {
				editor->itemId = -1;
			}
			editor->current = transform;
			const bool editable = transform.itemId >= 0 && !transform.locked;
			const bool resize = editable && transform.resizable;
			openProperties->setEnabled(transform.itemId >= 0);
			copyLayout->setEnabled(transform.itemId >= 0);
			applyPreset->setEnabled(resize);
			pasteLayout->setEnabled(editable);
			for (auto *field : {x, y}) {
				field->setEnabled(editable);
			}
			for (auto *field : {width, height}) {
				field->setEnabled(resize);
			}
			rotate->setEnabled(editable);
			for (auto *field : {cropLeft, cropTop, cropRight, cropBottom}) {
				field->setEnabled(editable);
			}
			for (auto *button : {fit, center, hCenter, vCenter}) {
				button->setEnabled(editable);
			}
			lockedHint->setText(transform.locked ? QStringLiteral("Источник заблокирован в OBS") :
						  QString());

			// Do not overwrite an in-progress edit during the 500 ms OBS state refresh.
			if (!selectedChanged && (x->hasFocus() || y->hasFocus() || width->hasFocus() ||
						 height->hasFocus() || rotate->hasFocus() || cropLeft->hasFocus() ||
						 cropTop->hasFocus() || cropRight->hasFocus() || cropBottom->hasFocus())) {
				return;
			}
			const QSignalBlocker bx(x), by(y), bw(width), bh(height), br(rotate);
			const QSignalBlocker bcl(cropLeft), bct(cropTop), bcr(cropRight), bcb(cropBottom);
			x->setValue(transform.x);
			y->setValue(transform.y);
			rotate->setValue(transform.rotation);
			cropLeft->setValue(transform.cropLeft);
			cropTop->setValue(transform.cropTop);
			cropRight->setValue(transform.cropRight);
			cropBottom->setValue(transform.cropBottom);
			if (transform.resizable) {
				width->setValue(transform.width);
				height->setValue(transform.height);
				editor->ratio = transform.height > 0.001 ? transform.width / transform.height : 1.0;
			} else {
				width->setValue(width->minimum());
				height->setValue(height->minimum());
			}
			editor->itemId = transform.itemId;
		});

	// XINKX-only settings are kept separately from OBS profile/scene data.
	char configPath[512];
	bool inspectorVisible = true;
	QSettings *settings = nullptr;
	const int configLength = GetAppConfigPath(configPath, sizeof(configPath), "xinkx-studio/studio.ini");
	if (configLength > 0 && static_cast<size_t>(configLength) < sizeof(configPath)) {
		const QString path = QString::fromUtf8(configPath);
		QDir().mkpath(QFileInfo(path).absolutePath());
		settings = new QSettings(path, QSettings::IniFormat, mainWindow);
		inspectorVisible = settings->value(QStringLiteral("workspace/inspectorVisible"), true).toBool();
		QObject::connect(inspector, &QDockWidget::visibilityChanged, mainWindow, [settings](bool visible) {
			settings->setValue(QStringLiteral("workspace/inspectorVisible"), visible);
		});
	}
	// OBS restores its saved window layout after constructing the frontend.
	// Re-dock the inspector once that has finished; floating windows must not cover Preview.
	QTimer::singleShot(0, mainWindow, [mainWindow, inspector, inspectorVisible] {
		inspector->setFloating(false);
		mainWindow->addDockWidget(Qt::RightDockWidgetArea, inspector);
		inspector->setVisible(inspectorVisible);
		mainWindow->resizeDocks({inspector}, {280}, Qt::Horizontal);
	});
	auto *studio = new QDockWidget(QStringLiteral("XINKX Studio"), mainWindow);
	studio->setObjectName(QStringLiteral("xinkxStudioDock"));
	studio->setAllowedAreas(Qt::RightDockWidgetArea);
	studio->setFeatures(studio->features() & ~QDockWidget::DockWidgetFloatable);
	auto *studioPages = new QTabWidget(studio);
	studioPages->setObjectName(QStringLiteral("xinkxStudioTabs"));
	studioPages->setDocumentMode(true);
	studio->setWidget(studioPages);
	mainWindow->AddDockWidget(studio, Qt::RightDockWidgetArea);
	installOverlayWorkspace(mainWindow, inspector);
	installChatWorkspace(mainWindow);
	installPresentationWorkspace(mainWindow);
	installBroadcastWorkspace(mainWindow);
	installGameProfilesWorkspace(mainWindow);
	// JSON scene export remains available in source for diagnostics. Native source and
	// scene editing in OBS is the default; no separate raw JSON dock is opened.
	installDonationWorkspace(mainWindow);
	QTimer::singleShot(0, mainWindow, [mainWindow, inspector, studio, studioPages] {
		studio->setFloating(false);
		mainWindow->tabifyDockWidget(inspector, studio);
		studio->show();
		studio->raise();
		mainWindow->resizeDocks({studio}, {450}, Qt::Horizontal);
		const int chat = studioPages->indexOf(studioPages->findChild<QWidget *>(QStringLiteral("xinkxChatPage")));
		if (chat >= 0) studioPages->setCurrentIndex(chat);
	});
}

void shutdownStudioOutputs(OBSBasic *mainWindow)
{
	if (auto *manager = mainWindow->findChild<MultiStreamManager *>()) {
		manager->shutdown();
	}
}

} // namespace xinkx
