#pragma once

class OBSBasic;
class QDockWidget;
class QTabWidget;

namespace xinkx {

void installStudioWorkspace(OBSBasic *mainWindow);
void shutdownStudioOutputs(OBSBasic *mainWindow);
QDockWidget *studioDock(OBSBasic *mainWindow);
QTabWidget *studioTabs(OBSBasic *mainWindow);

} // namespace xinkx
