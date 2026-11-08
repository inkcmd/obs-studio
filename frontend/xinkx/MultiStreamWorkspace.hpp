#pragma once

class QWidget;

namespace xinkx {

// Extensions live in OBS's existing Settings -> Stream page. The main stream
// remains fully managed by OBS; this list defines only extra destinations.
void installMultiStreamSettings(QWidget *streamPage);

} // namespace xinkx
