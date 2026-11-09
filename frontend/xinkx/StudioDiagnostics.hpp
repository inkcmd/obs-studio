#pragma once

#include "StreamAccounts.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QTextStream>

namespace xinkx {

inline QString diagnosticsPath()
{
	return QFileInfo(studioSettingsPath()).absoluteDir().filePath(QStringLiteral("diagnostics.log"));
}

inline void writeDiagnostic(const QString &area, const QString &message)
{
	static QMutex mutex;
	const QMutexLocker guard(&mutex);
	QFile file(diagnosticsPath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
	QTextStream stream(&file);
	stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " [" << area << "] "
	       << message.left(1000).replace(QLatin1Char('\n'), QLatin1Char(' ')) << '\n';
}

} // namespace xinkx
