#include "StreamAccounts.hpp"
#include "PlatformApi.hpp"

#include <OBSApp.hpp>

#include <QDateTime>
#include <QDir>
#include <QByteArray>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>

#include <array>

#ifdef _WIN32
#include <Windows.h>
#include <wincrypt.h>
#endif

namespace xinkx {

namespace {

QString protect(const QString &clear)
{
	if (clear.isEmpty()) {
		return {};
	}
#ifdef _WIN32
	const QByteArray plain = clear.toUtf8();
	DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()))};
	DATA_BLOB output{};
	if (!CryptProtectData(&input, L"XINKX Studio", nullptr, nullptr, nullptr,
			      CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		return {};
	}
	const QByteArray encrypted(reinterpret_cast<const char *>(output.pbData), static_cast<int>(output.cbData));
	LocalFree(output.pbData);
	return QString::fromLatin1(encrypted.toBase64());
#else
	// Desktop keychain integration is required before enabling token persistence on Linux.
	return {};
#endif
}

QString unprotect(const QString &encrypted)
{
	if (encrypted.isEmpty()) {
		return {};
	}
#ifdef _WIN32
	const QByteArray bytes = QByteArray::fromBase64(encrypted.toLatin1());
	DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data()))};
	DATA_BLOB output{};
	if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
		return {};
	}
	const QString clear = QString::fromUtf8(reinterpret_cast<const char *>(output.pbData),
					      static_cast<int>(output.cbData));
	SecureZeroMemory(output.pbData, output.cbData);
	LocalFree(output.pbData);
	return clear;
#else
	return {};
#endif
}

} // namespace

QString studioSettingsPath()
{
	char path[512]{};
	const int length = GetAppConfigPath(path, sizeof(path), "xinkx-studio/studio.ini");
	if (length <= 0 || static_cast<size_t>(length) >= sizeof(path)) {
		return {};
	}
	const QString result = QString::fromUtf8(path);
	QDir().mkpath(QFileInfo(result).absolutePath());
	return result;
}

QString platformTitle(const QString &platform)
{
	const auto *api = platformApi(platform);
	return api ? api->title : platform;
}

bool validIngestUrl(const QString &value)
{
	const QUrl url(value, QUrl::StrictMode);
	const auto scheme = url.scheme();
	return url.isValid() && (scheme == QStringLiteral("rtmp") || scheme == QStringLiteral("rtmps")) &&
	       !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}

bool StreamDestination::ready() const
{
	return validIngestUrl(ingestUrl) && !streamKey.trimmed().isEmpty() && width >= 320 && width <= 7680 &&
	       height >= 240 && height <= 4320 && width % 2 == 0 && height % 2 == 0 &&
	       bitrate >= 500 && bitrate <= 51000;
}

bool OAuthSession::requiresAuthorization() const
{
	return accessToken.isEmpty() || (expiresAt > 0 && expiresAt <= QDateTime::currentSecsSinceEpoch());
}

QString StreamAccount::displayName() const
{
	return providerName.isEmpty() || providerName == label ? label : label + QStringLiteral(" · ") + providerName;
}

void StreamAccount::clearAuthorization()
{
	oauth.accessToken.clear();
	oauth.refreshToken.clear();
	oauth.expiresAt = 0;
	providerId.clear();
	providerName.clear();
	if (destination.fromOAuth) {
		destination.ingestUrl.clear();
		destination.streamKey.clear();
		destination.fromOAuth = false;
	}
}

QVector<StreamAccount> loadStreamAccounts(bool withSecrets)
{
	const QString path = studioSettingsPath();
	if (path.isEmpty()) {
		return {};
	}
	QSettings settings(path, QSettings::IniFormat);
	QVector<StreamAccount> accounts;
	const int count = settings.beginReadArray(QStringLiteral("accounts"));
	for (int i = 0; i < count; ++i) {
		settings.setArrayIndex(i);
		StreamAccount account;
		account.id = settings.value(QStringLiteral("id")).toString();
		account.platform = settings.value(QStringLiteral("platform")).toString();
		if (account.id.isEmpty() || (!platformApi(account.platform) && account.platform != QStringLiteral("custom"))) {
			continue;
		}
		account.label = settings.value(QStringLiteral("label"), platformTitle(account.platform)).toString();
		account.providerId = settings.value(QStringLiteral("providerId")).toString();
		account.providerName = settings.value(QStringLiteral("providerName")).toString();
		account.youtubeStreamId = settings.value(QStringLiteral("youtubeStreamId")).toString();
		account.primary = settings.value(QStringLiteral("primary"), false).toBool();
		account.enabled = settings.value(QStringLiteral("enabled"), true).toBool();
		if (withSecrets) {
			account.oauth.clientId = settings.value(QStringLiteral("clientId")).toString();
			account.oauth.clientSecret = unprotect(settings.value(QStringLiteral("clientSecret")).toString());
			account.oauth.accessToken = unprotect(settings.value(QStringLiteral("accessToken")).toString());
			account.oauth.refreshToken = unprotect(settings.value(QStringLiteral("refreshToken")).toString());
			account.oauth.expiresAt = settings.value(QStringLiteral("expiresAt")).toLongLong();
			account.destination.ingestUrl = settings.contains(QStringLiteral("ingestUrlDpapi"))
								? unprotect(settings.value(QStringLiteral("ingestUrlDpapi")).toString())
								: settings.value(QStringLiteral("ingestUrl")).toString();
			account.destination.streamKey = unprotect(settings.value(QStringLiteral("streamKey")).toString());
		}
		account.destination.encoder = settings.value(QStringLiteral("encoder"), QStringLiteral("auto")).toString();
		account.destination.bitrate = settings.value(QStringLiteral("bitrate"), 6000).toInt();
		account.destination.width = settings.value(QStringLiteral("width"), 1920).toInt();
		account.destination.height = settings.value(QStringLiteral("height"), 1080).toInt();
		account.destination.fromOAuth = settings.value(QStringLiteral("ingestFromOAuth"),
								       !account.oauth.accessToken.isEmpty()).toBool();
		accounts.push_back(account);
	}
	settings.endArray();
	return accounts;
}

bool saveStreamAccounts(const QVector<StreamAccount> &accounts)
{
	const QString path = studioSettingsPath();
	if (path.isEmpty()) {
		return false;
	}
	QVector<std::array<QString, 5>> encrypted;
	encrypted.reserve(accounts.size());
	for (const auto &account : accounts) {
		const std::array secrets{account.oauth.clientSecret, account.oauth.accessToken,
					 account.oauth.refreshToken, account.destination.streamKey, account.destination.ingestUrl};
		std::array<QString, 5> values;
		for (size_t i = 0; i < secrets.size(); ++i) {
			values[i] = protect(secrets[i]);
			if (!secrets[i].isEmpty() && values[i].isEmpty()) {
				return false;
			}
		}
		encrypted.push_back(values);
	}
	QSettings settings(path, QSettings::IniFormat);
	settings.remove(QStringLiteral("accounts"));
	settings.beginWriteArray(QStringLiteral("accounts"));
	for (int i = 0; i < accounts.size(); ++i) {
		settings.setArrayIndex(i);
		const auto &a = accounts[i];
		settings.setValue(QStringLiteral("id"), a.id);
		settings.setValue(QStringLiteral("platform"), a.platform);
		settings.setValue(QStringLiteral("label"), a.label);
		settings.setValue(QStringLiteral("enabled"), a.enabled);
		settings.setValue(QStringLiteral("primary"), a.primary);
		settings.setValue(QStringLiteral("providerId"), a.providerId);
		settings.setValue(QStringLiteral("providerName"), a.providerName);
		settings.setValue(QStringLiteral("youtubeStreamId"), a.youtubeStreamId);
		settings.setValue(QStringLiteral("ingestFromOAuth"), a.destination.fromOAuth);
		settings.setValue(QStringLiteral("clientId"), a.oauth.clientId);
		settings.setValue(QStringLiteral("clientSecret"), encrypted[i][0]);
		settings.setValue(QStringLiteral("accessToken"), encrypted[i][1]);
		settings.setValue(QStringLiteral("refreshToken"), encrypted[i][2]);
		settings.setValue(QStringLiteral("expiresAt"), a.oauth.expiresAt);
		settings.setValue(QStringLiteral("ingestUrlDpapi"), encrypted[i][4]);
		settings.setValue(QStringLiteral("streamKey"), encrypted[i][3]);
		settings.setValue(QStringLiteral("encoder"), a.destination.encoder);
		settings.setValue(QStringLiteral("bitrate"), a.destination.bitrate);
		settings.setValue(QStringLiteral("width"), a.destination.width);
		settings.setValue(QStringLiteral("height"), a.destination.height);
	}
	settings.endArray();
	settings.sync();
	return settings.status() == QSettings::NoError;
}

} // namespace xinkx
