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
	// A named service and server from OBS's built-in RTMP service catalog.
	// Empty serviceName means the explicit ingestUrl is used instead.
	QString serviceName;
	QString serviceServer;
	QString encoder = QStringLiteral("auto");
	int bitrate = 6000;
	int width = 1920;
	int height = 1080;
	// Routes this destination to an independent 1080x1920 libobs canvas.
	bool portrait = false;
	bool fromOAuth = false;
	bool ready() const;
};

struct StreamAccount {
	QString id;
	QString platform;
	QString label;
	QString providerId;
	QString providerName;
	// Another output can use the same OAuth identity without storing a second
	// set of credentials. RTMP key, stream ID and composition remain independent.
	QString authorizationSourceId;
	QString youtubeStreamId;
	bool enabled = true;
	bool primary = false;
	OAuthSession oauth;
	StreamDestination destination;
	QString displayName() const;
	void clearAuthorization();
};

QString studioSettingsPath();
QString protectStudioSecret(const QString &clear);
QString unprotectStudioSecret(const QString &encrypted);
QVector<StreamAccount> loadStreamAccounts(bool withSecrets = true);
bool saveStreamAccounts(const QVector<StreamAccount> &accounts);
QString platformTitle(const QString &platform);
bool validIngestUrl(const QString &url);

} // namespace xinkx
