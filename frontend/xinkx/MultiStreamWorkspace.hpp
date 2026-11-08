#pragma once

class QDockWidget;
class QLabel;
class QSettings;
class OBSBasic;

namespace xinkx {

// Integrated right-side tab: each remote output is encoded and sent by libobs.
QDockWidget *installMultiStreamWorkspace(OBSBasic *mainWindow, QDockWidget *inspector,
					QSettings *settings, QLabel *multiStatus);

} // namespace xinkx
