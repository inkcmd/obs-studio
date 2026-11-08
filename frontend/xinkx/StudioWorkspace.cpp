#include "StudioWorkspace.hpp"

#include "StudioController.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <QDir>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <memory>

namespace xinkx {

namespace {

struct InspectorState {
	qint64 itemId = -1;
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

	// Compact native header with live OBS state. No duplicate stream controls.
	auto *header = new QToolBar(mainWindow);
	header->setObjectName(QStringLiteral("xinkxStudioHeader"));
	header->setMovable(false);
	header->setFloatable(false);
	header->setMinimumHeight(44);
	header->setStyleSheet(QStringLiteral(
		"QToolBar#xinkxStudioHeader { background: #171d28; border: 0; spacing: 14px; padding: 5px 12px; }"
		"QToolBar#xinkxStudioHeader QLabel { color: #c4cddc; background: transparent; }"
		"QLabel#xinkxStudioBrand { color: #f4f7fc; font-size: 14px; font-weight: 700; }"));

	auto *brand = new QLabel(QStringLiteral("XINKX  /  STUDIO"), header);
	brand->setObjectName(QStringLiteral("xinkxStudioBrand"));
	header->addWidget(brand);
	header->addSeparator();

	auto *scene = new QLabel(QStringLiteral("Сцена: —"), header);
	scene->setMinimumWidth(150);
	header->addWidget(scene);

	auto *spacer = new QWidget(header);
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	header->addWidget(spacer);

	auto *streamStatus = new QLabel(QStringLiteral("STREAM  OFFLINE"), header);
	auto *recordStatus = new QLabel(QStringLiteral("REC  OFF"), header);
	header->addWidget(streamStatus);
	auto *statusSpacer = new QWidget(header);
	statusSpacer->setFixedWidth(22);
	header->addWidget(statusSpacer);
	header->addWidget(recordStatus);
	mainWindow->addToolBar(Qt::TopToolBarArea, header);

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
	for (auto *field : {x, y, width, height}) {
		QObject::connect(field, &QDoubleSpinBox::editingFinished, mainWindow, apply);
	}

	QObject::connect(openProperties, &QPushButton::clicked, mainWindow, [mainWindow] {
		mainWindow->actionOpenSourceProperties();
	});

	QObject::connect(controller, &StudioController::stateChanged, mainWindow,
		[scene, streamStatus, recordStatus, sourceTitle, openProperties](
			const QString &sceneName, const QString &selectedSource, bool streaming, bool recording) {
			scene->setText(QStringLiteral("Сцена: %1")
					       .arg(sceneName.isEmpty() ? QStringLiteral("—") : sceneName));
			streamStatus->setText(streaming ? QStringLiteral("STREAM  LIVE") :
						       QStringLiteral("STREAM  OFFLINE"));
			recordStatus->setText(recording ? QStringLiteral("REC  ON") : QStringLiteral("REC  OFF"));
			streamStatus->setStyleSheet(streaming ? QStringLiteral("color: #58dea2;") :
							       QStringLiteral("color: #94a0b5;"));
			recordStatus->setStyleSheet(recording ? QStringLiteral("color: #ff818f;") :
							       QStringLiteral("color: #94a0b5;"));
			sourceTitle->setText(selectedSource.isEmpty() ? QStringLiteral("Источник не выбран") :
									 selectedSource);
			openProperties->setEnabled(!selectedSource.isEmpty());
		});

	QObject::connect(controller, &StudioController::selectedTransformChanged, mainWindow,
		[editor, x, y, width, height, lockedHint, openProperties](const SelectedTransform &transform) {
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
			lockedHint->setText(transform.locked ? QStringLiteral("Источник заблокирован в OBS") :
						  QString());

			// Do not overwrite an in-progress edit during the 500 ms OBS state refresh.
			if (!selectedChanged && (x->hasFocus() || y->hasFocus() || width->hasFocus() ||
					 height->hasFocus())) {
				return;
			}
			const QSignalBlocker bx(x), by(y), bw(width), bh(height);
			x->setValue(transform.x);
			y->setValue(transform.y);
			if (transform.resizable) {
				width->setValue(transform.width);
				height->setValue(transform.height);
			} else {
				width->setValue(width->minimum());
				height->setValue(height->minimum());
			}
			editor->itemId = transform.itemId;
		});

	// XINKX-only settings are kept separately from OBS profile/scene data.
	char configPath[512];
	bool inspectorVisible = true;
	const int configLength = GetAppConfigPath(configPath, sizeof(configPath), "xinkx-studio/studio.ini");
	if (configLength > 0 && static_cast<size_t>(configLength) < sizeof(configPath)) {
		const QString path = QString::fromUtf8(configPath);
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto *settings = new QSettings(path, QSettings::IniFormat, mainWindow);
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

} // namespace xinkx
