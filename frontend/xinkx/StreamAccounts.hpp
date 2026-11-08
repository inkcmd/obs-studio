#pragma once

#include <QString>
#include <QVector>

namespace xinkx {

struct OAuthSession {
	QString clientId;
	QString clientSecret;
	QString accessToken;
	QString refreshToken;
	qint64 expiresAt = 0;
	bool requiresAuthorization() const;
};

struct StreamDestination {
	QString ingestUrl;
	QString streamKey;
	QString encoder = QStringLiteral("auto");
	int bitrate = 6000;
	int width = 1920;
	int height = 1080;
	bool fromOAuth = false;
	bool ready() const;
};

struct StreamAccount {
	QString id;
	QString platform;
	QString label;
	QString providerId;
	QString providerName;
	QString youtubeStreamId;
	bool enabled = true;
	bool primary = false;
	OAuthSession oauth;
	StreamDestination destination;
	QString displayName() const;
	void clearAuthorization();
};

QString studioSettingsPath();
QVector<StreamAccount> loadStreamAccounts(bool withSecrets = true);
bool saveStreamAccounts(const QVector<StreamAccount> &accounts);
QString platformTitle(const QString &platform);
bool validIngestUrl(const QString &url);

} // namespace xinkx
