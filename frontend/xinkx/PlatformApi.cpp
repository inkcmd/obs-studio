#include "PlatformApi.hpp"

#include <QList>

namespace xinkx {

const QList<PlatformApi> &platformApis()
{
	using namespace Qt::StringLiterals;
	static const QList<PlatformApi> platforms{
		{u"twitch"_s, u"Twitch"_s, QUrl(u"https://id.twitch.tv/oauth2/authorize"_s),
		 QUrl(u"https://id.twitch.tv/oauth2/token"_s), QUrl(u"https://id.twitch.tv/oauth2/revoke"_s),
		 u"channel:read:stream_key channel:manage:broadcast channel:read:subscriptions "
		 u"moderator:read:followers bits:read user:read:chat user:write:chat channel:bot"_s,
		 QUrl(u"https://api.twitch.tv/helix/users"_s), false, true},
		{u"youtube"_s, u"YouTube"_s, QUrl(u"https://accounts.google.com/o/oauth2/v2/auth"_s),
		 QUrl(u"https://oauth2.googleapis.com/token"_s), QUrl(u"https://oauth2.googleapis.com/revoke"_s),
		 u"https://www.googleapis.com/auth/youtube"_s,
		 QUrl(u"https://www.googleapis.com/youtube/v3/channels?part=id,snippet&mine=true"_s), true, false},
		{u"kick"_s, u"Kick"_s, QUrl(u"https://id.kick.com/oauth/authorize"_s),
		 QUrl(u"https://id.kick.com/oauth/token"_s), QUrl(u"https://id.kick.com/oauth/revoke"_s),
		 u"user:read channel:read streamkey:read channel:write chat:write events:subscribe kicks:read"_s,
		 QUrl(u"https://api.kick.com/public/v1/users"_s), true, true},
	};
	return platforms;
}

const PlatformApi *platformApi(const QString &id)
{
	for (const auto &platform : platformApis()) {
		if (platform.id == id) {
			return &platform;
		}
	}
	return nullptr;
}

} // namespace xinkx
