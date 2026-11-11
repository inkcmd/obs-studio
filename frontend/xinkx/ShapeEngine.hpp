#pragma once

class OBSBasic;
class QWidget;

namespace xinkx {

// GPU-backed OBS input source; layout, filters, undo and persistence are
// owned by real OBS scene items. No CEF/browser source is involved.
void registerShapeSource();
void showShapeCreator(OBSBasic *window, QWidget *parent);

} // namespace xinkx
