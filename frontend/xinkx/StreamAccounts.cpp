#include "StreamAccounts.hpp"

#include <OBSApp.hpp>

#include <QDir>
#include <QByteArray>
#include <QFileInfo>
#include <QSettings>

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
	if (platform == QStringLiteral("twitch")) {
		return QStringLiteral("Twitch");
	}
	if (platform == QStringLiteral("youtube")) {
		return QStringLiteral("YouTube");
	}
	if (platform == QStringLiteral("kick")) {
		return QStringLiteral("Kick");
	}
	return platform;
}

QVector<StreamAccount> loadStreamAccounts()
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
		if (account.id.isEmpty() || (account.platform != QStringLiteral("twitch") &&
					 account.platform != QStringLiteral("youtube") &&
					 account.platform != QStringLiteral("kick"))) {
			continue;
		}
		account.label = settings.value(QStringLiteral("label"), platformTitle(account.platform)).toString();
		account.enabled = settings.value(QStringLiteral("enabled"), true).toBool();
		account.clientId = settings.value(QStringLiteral("clientId")).toString();
		account.clientSecret = unprotect(settings.value(QStringLiteral("clientSecret")).toString());
		account.accessToken = unprotect(settings.value(QStringLiteral("accessToken")).toString());
		account.refreshToken = unprotect(settings.value(QStringLiteral("refreshToken")).toString());
		account.expiresAt = settings.value(QStringLiteral("expiresAt")).toLongLong();
		account.ingestUrl = settings.value(QStringLiteral("ingestUrl")).toString();
		account.streamKey = unprotect(settings.value(QStringLiteral("streamKey")).toString());
		account.encoder = settings.value(QStringLiteral("encoder"), QStringLiteral("auto")).toString();
		account.bitrate = settings.value(QStringLiteral("bitrate"), 6000).toInt();
		account.width = settings.value(QStringLiteral("width"), 1920).toInt();
		account.height = settings.value(QStringLiteral("height"), 1080).toInt();
		accounts.push_back(account);
	}
	settings.endArray();
	return accounts;
}

void saveStreamAccounts(const QVector<StreamAccount> &accounts)
{
	const QString path = studioSettingsPath();
	if (path.isEmpty()) {
		return;
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
		settings.setValue(QStringLiteral("clientId"), a.clientId);
		settings.setValue(QStringLiteral("clientSecret"), protect(a.clientSecret));
		settings.setValue(QStringLiteral("accessToken"), protect(a.accessToken));
		settings.setValue(QStringLiteral("refreshToken"), protect(a.refreshToken));
		settings.setValue(QStringLiteral("expiresAt"), a.expiresAt);
		settings.setValue(QStringLiteral("ingestUrl"), a.ingestUrl);
		settings.setValue(QStringLiteral("streamKey"), protect(a.streamKey));
		settings.setValue(QStringLiteral("encoder"), a.encoder);
		settings.setValue(QStringLiteral("bitrate"), a.bitrate);
		settings.setValue(QStringLiteral("width"), a.width);
		settings.setValue(QStringLiteral("height"), a.height);
	}
	settings.endArray();
	settings.sync();
}

} // namespace xinkx
