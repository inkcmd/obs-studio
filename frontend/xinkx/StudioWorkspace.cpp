#include "StudioWorkspace.hpp"

#include "StudioController.hpp"
#include "MultiStreamManager.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <QDir>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <utility>

namespace xinkx {

namespace {

struct InspectorState {
	qint64 itemId = -1;
	double ratio = 1.0;
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

void installStudioWorkspace(OBSBasic *mainWindow)
{
	auto *controller = new StudioController(mainWindow);
	auto *manager = new MultiStreamManager(mainWindow);
	QObject::connect(mainWindow, &OBSBasic::StreamingStarted, manager,
			 [manager](bool delayStarting) {
				 if (!delayStarting) {
					 manager->startConfigured();
				 }
			 });
	QObject::connect(mainWindow, &OBSBasic::StreamingStopping, manager, [manager] { manager->stopAll(); });
	QObject::connect(mainWindow, &OBSBasic::StreamingStopped, manager, [manager] { manager->stopAll(); });
	auto *extraStatus = new QLabel(mainWindow);
	mainWindow->statusBar()->addPermanentWidget(extraStatus);
	auto updateExtraStatus = [extraStatus, manager] {
		const int active = manager->activeCount();
		const int count = manager->configuredCount();
		extraStatus->setText(QStringLiteral("Доп. потоки: %1/%2").arg(active).arg(count));
		extraStatus->setStyleSheet(active > 0 ? QStringLiteral("color: #58dea2;") : QString());
	};
	QObject::connect(manager, &MultiStreamManager::destinationChanged, mainWindow,
			 [updateExtraStatus](int) { updateExtraStatus(); });
	QObject::connect(manager, &MultiStreamManager::streamError, mainWindow,
			 [mainWindow](const QString &message) { mainWindow->statusBar()->showMessage(message, 10000); });
	updateExtraStatus();

	// Inspector edits real OBS scene items; standard source dialogs stay available.
	auto *inspector = new QDockWidget(QStringLiteral("Источник"), mainWindow);
	inspector->setObjectName(QStringLiteral("xinkxSourceInspector"));
	inspector->setAllowedAreas(Qt::RightDockWidgetArea);

	auto *content = new QWidget(inspector);
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

	auto *lockedHint = new QLabel(content);
	lockedHint->setWordWrap(true);
	lockedHint->setStyleSheet(QStringLiteral("color: #9da9bd;"));
	layout->addWidget(lockedHint);

	auto *openProperties = new QPushButton(QStringLiteral("Свойства источника"), content);
	openProperties->setEnabled(false);
	layout->addWidget(openProperties);
	layout->addStretch();
	inspector->setWidget(content);
	mainWindow->AddDockWidget(inspector, Qt::RightDockWidgetArea);
	inspector->setFeatures(inspector->features() & ~QDockWidget::DockWidgetFloatable);

	auto editor = std::make_shared<InspectorState>();
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
		 hCenter, vCenter, lockedHint, openProperties](const SelectedTransform &transform) {
			const bool selectedChanged = editor->itemId != transform.itemId;
			if (selectedChanged) {
				editor->itemId = -1;
			}
			const bool editable = transform.itemId >= 0 && !transform.locked;
			const bool resize = editable && transform.resizable;
			openProperties->setEnabled(transform.itemId >= 0);
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
}

void shutdownStudioOutputs(OBSBasic *mainWindow)
{
	if (auto *manager = mainWindow->findChild<MultiStreamManager *>()) {
		manager->shutdown();
	}
}

} // namespace xinkx
