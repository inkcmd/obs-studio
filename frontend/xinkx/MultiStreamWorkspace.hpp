#pragma once

#include <functional>

class QWidget;

namespace xinkx {

std::function<bool()> installMultiStreamSettings(QWidget *streamPage, QWidget *nativeService, QWidget *nativeStack,
						      const std::function<void()> &changed,
						      const std::function<void(bool)> &busyChanged);

} // namespace xinkx
