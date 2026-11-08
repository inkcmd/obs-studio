#include "StudioWorkspace.hpp"

#include "StudioController.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QToolBar>
#include <QVBoxLayout>

namespace xinkx {

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

	auto *scene = new QLabel(QStringLiteral("Scene: —"), header);
	scene->setMinimumWidth(150);
	header->addWidget(scene);

	auto *spacer = new QWidget(header);
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	header->addWidget(spacer);

	auto *streamStatus = new QLabel(QStringLiteral("STREAM  OFFLINE"), header);
	auto *recordStatus = new QLabel(QStringLiteral("REC  OFF"), header);
	header->addWidget(streamStatus);
	header->addWidget(recordStatus);
	mainWindow->addToolBar(Qt::TopToolBarArea, header);

	// Read-only inspector: selection comes directly from OBS scene items.
	// Its editor will be introduced in Stage 2; the existing OBS property editor remains available.
	auto *inspector = new QDockWidget(QStringLiteral("Source inspector"), mainWindow);
	inspector->setObjectName(QStringLiteral("xinkxSourceInspector"));
	inspector->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

	auto *content = new QWidget(inspector);
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(14, 16, 14, 14);
	layout->setSpacing(12);

	auto *sourceTitle = new QLabel(QStringLiteral("No source selected"), content);
	sourceTitle->setWordWrap(true);
	sourceTitle->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px;"));
	layout->addWidget(sourceTitle);

	auto *sourceDetails = new QLabel(content);
	sourceDetails->setWordWrap(true);
	sourceDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(sourceDetails);

	auto *openProperties = new QPushButton(QStringLiteral("Source properties"), content);
	openProperties->setEnabled(false);
	layout->addWidget(openProperties);
	layout->addStretch();
	inspector->setWidget(content);
	mainWindow->AddDockWidget(inspector, Qt::RightDockWidgetArea);

	connect(openProperties, &QPushButton::clicked, mainWindow, [mainWindow] {
		mainWindow->actionOpenSourceProperties();
	});

	connect(controller, &StudioController::stateChanged, mainWindow,
		[scene, streamStatus, recordStatus, sourceTitle, sourceDetails, openProperties](
			const QString &sceneName, const QString &selectedSource, const QString &details, bool streaming,
			bool recording) {
			scene->setText(QStringLiteral("Scene: %1")
					       .arg(sceneName.isEmpty() ? QStringLiteral("—") : sceneName));
			streamStatus->setText(streaming ? QStringLiteral("STREAM  LIVE") :
						       QStringLiteral("STREAM  OFFLINE"));
			recordStatus->setText(recording ? QStringLiteral("REC  ON") : QStringLiteral("REC  OFF"));
			streamStatus->setStyleSheet(streaming ? QStringLiteral("color: #58dea2;") :
							       QStringLiteral("color: #94a0b5;"));
			recordStatus->setStyleSheet(recording ? QStringLiteral("color: #ff818f;") :
							       QStringLiteral("color: #94a0b5;"));
			sourceTitle->setText(selectedSource.isEmpty() ? QStringLiteral("No source selected") :
									 selectedSource);
			sourceDetails->setText(details);
			openProperties->setEnabled(!selectedSource.isEmpty());
		});

	// XINKX-only settings are kept separately from OBS profile/scene data.
	char configPath[512];
	const int configLength = GetAppConfigPath(configPath, sizeof(configPath), "xinkx-studio/studio.ini");
	if (configLength > 0 && static_cast<size_t>(configLength) < sizeof(configPath)) {
		const QString path = QString::fromUtf8(configPath);
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto *settings = new QSettings(path, QSettings::IniFormat, mainWindow);
		inspector->setVisible(settings->value(QStringLiteral("workspace/inspectorVisible"), true).toBool());
		connect(inspector, &QDockWidget::visibilityChanged, mainWindow, [settings](bool visible) {
			settings->setValue(QStringLiteral("workspace/inspectorVisible"), visible);
		});
	}
}

} // namespace xinkx
