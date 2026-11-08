#pragma once

#include <QString>
#include <QVector>

namespace xinkx {

struct StreamAccount {
	QString id;
	QString platform;
	QString label;
	bool enabled = true;
	QString clientId;
	QString clientSecret;
	QString accessToken;
	QString refreshToken;
	qint64 expiresAt = 0;
	QString ingestUrl;
	QString streamKey;
	QString encoder = QStringLiteral("auto");
	int bitrate = 6000;
	int width = 1920;
	int height = 1080;
};

QString studioSettingsPath();
QVector<StreamAccount> loadStreamAccounts();
void saveStreamAccounts(const QVector<StreamAccount> &accounts);
QString platformTitle(const QString &platform);

} // namespace xinkx
