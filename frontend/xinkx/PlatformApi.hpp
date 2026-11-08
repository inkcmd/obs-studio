#pragma once

#include <QString>
#include <QUrl>

namespace xinkx {

struct PlatformApi {
	QString id;
	QString title;
	QUrl authorize;
	QUrl token;
	QUrl revoke;
	QString scopes;
	QUrl identity;
	bool pkce;
	bool requiresSecret;
};

const PlatformApi *platformApi(const QString &id);
const QList<PlatformApi> &platformApis();

} // namespace xinkx
