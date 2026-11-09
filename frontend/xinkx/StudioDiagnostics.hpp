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

inline void writeDiagnostic(const QString &area, const QString &message, int maxChars = 1000)
{
	static QMutex mutex;
	const QMutexLocker guard(&mutex);
	QFile file(diagnosticsPath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
	QTextStream stream(&file);
	QString line = maxChars < 0 ? message : message.left(maxChars);
	line.replace(QLatin1Char('\n'), QLatin1Char(' '));
	stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " [" << area << "] "
	       << line << '\n';
}

} // namespace xinkx
