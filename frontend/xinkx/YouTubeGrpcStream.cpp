#include "YouTubeGrpcStream.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QUrl>

#include <curl/curl.h>

#include <limits>
#include <utility>

namespace xinkx {
namespace {

constexpr qsizetype maxFrame = 4 * 1024 * 1024;

void putVarint(QByteArray &out, quint64 value)
{
	while (value >= 0x80) {
		out.append(char((value & 0x7f) | 0x80));
		value >>= 7;
	}
	out.append(char(value));
}

void putString(QByteArray &out, quint32 field, const QByteArray &value)
{
	putVarint(out, (quint64(field) << 3) | 2);
	putVarint(out, quint64(value.size()));
	out.append(value);
}

QByteArray requestFrame(const QString &chatId, const QString &pageToken)
{
	// Official stream_list.proto: live_chat_id=1, page_token=99, part=100.
	QByteArray proto;
	putString(proto, 1, chatId.toUtf8());
	if (!pageToken.isEmpty()) putString(proto, 99, pageToken.toUtf8());
	for (const auto *part : {"id", "snippet", "authorDetails"}) putString(proto, 100, part);
	QByteArray frame(5, '\0'); // Uncompressed gRPC message + uint32 network-order length.
	const quint32 size = quint32(proto.size());
	frame[1] = char(size >> 24);
	frame[2] = char(size >> 16);
	frame[3] = char(size >> 8);
	frame[4] = char(size);
	return frame + proto;
}

class ProtoReader {
public:
	explicit ProtoReader(const QByteArray &data) : bytes(data) {}
	bool done() const { return cursor == bytes.size(); }
	bool next(quint32 &field, quint64 &number, QByteArray &content)
	{
		quint64 tag = 0;
		if (!varint(tag) || tag >> 3 > std::numeric_limits<quint32>::max() || tag < 8) return false;
		field = quint32(tag >> 3);
		const unsigned wire = unsigned(tag & 7);
		number = 0;
		content.clear();
		switch (wire) {
		case 0: return varint(number);
		case 1: return skip(8);
		case 5: return skip(4);
		case 2: {
			quint64 size = 0;
			if (!varint(size) || size > quint64(bytes.size() - cursor)) return false;
			content = bytes.mid(cursor, qsizetype(size));
			cursor += qsizetype(size);
			return true;
		}
		default: return false;
		}
	}

private:
	const QByteArray &bytes;
	qsizetype cursor = 0;
	bool skip(qsizetype count)
	{
		if (count > bytes.size() - cursor) return false;
		cursor += count;
		return true;
	}
	bool varint(quint64 &value)
	{
		value = 0;
		for (int i = 0; i < 10 && cursor < bytes.size(); ++i) {
			const quint8 octet = quint8(bytes.at(cursor++));
			if (i == 9 && octet > 1) return false;
			value |= quint64(octet & 0x7f) << (7 * i);
			if (!(octet & 0x80)) return true;
		}
		return false;
	}
};

bool readString(const QByteArray &raw, quint32 target, QString &out)
{
	ProtoReader fields(raw);
	while (!fields.done()) {
		quint32 field = 0;
		quint64 number = 0;
		QByteArray content;
		if (!fields.next(field, number, content)) return false;
		if (field == target) out = QString::fromUtf8(content);
	}
	return true;
}

bool parseSnippet(const QByteArray &raw, QJsonObject &snippet)
{
	ProtoReader fields(raw);
	int type = 0;
	while (!fields.done()) {
		quint32 field = 0;
		quint64 number = 0;
		QByteArray content;
		if (!fields.next(field, number, content)) return false;
		if (field == 1) type = int(number);
		else if (field == 16) snippet.insert(QStringLiteral("displayMessage"), QString::fromUtf8(content));
		else if (field == 19) {
			QString text;
			if (!readString(content, 1, text)) return false;
			snippet.insert(QStringLiteral("textMessageDetails"),
				QJsonObject{{QStringLiteral("messageText"), text}});
		} else if (field == 27 || field == 28) {
			QString amount;
			QString comment;
			if (!readString(content, 3, amount) ||
			    (field == 27 && !readString(content, 4, comment))) return false;
			snippet.insert(field == 27 ? QStringLiteral("superChatDetails") : QStringLiteral("superStickerDetails"),
				QJsonObject{{QStringLiteral("amountDisplayString"), amount},
					{QStringLiteral("userComment"), comment}});
		} else if (field == 30) {
			QString comment;
			if (!readString(content, 3, comment)) return false;
			snippet.insert(QStringLiteral("displayMessage"), comment);
		}
	}
	switch (type) {
	case 1: snippet.insert(QStringLiteral("type"), QStringLiteral("textMessageEvent")); break;
	case 15: snippet.insert(QStringLiteral("type"), QStringLiteral("superChatEvent")); break;
	case 16: snippet.insert(QStringLiteral("type"), QStringLiteral("superStickerEvent")); break;
	case 7: snippet.insert(QStringLiteral("type"), QStringLiteral("newSponsorEvent")); break;
	case 17: snippet.insert(QStringLiteral("type"), QStringLiteral("memberMilestoneChatEvent")); break;
	case 18: snippet.insert(QStringLiteral("type"), QStringLiteral("membershipGiftingEvent")); break;
	case 19: snippet.insert(QStringLiteral("type"), QStringLiteral("giftMembershipReceivedEvent")); break;
	default: break;
	}
	return true;
}

bool parseItem(const QByteArray &raw, QJsonObject &item)
{
	ProtoReader fields(raw);
	while (!fields.done()) {
		quint32 field = 0;
		quint64 number = 0;
		QByteArray content;
		if (!fields.next(field, number, content)) return false;
		if (field == 101) item.insert(QStringLiteral("id"), QString::fromUtf8(content));
		else if (field == 2) {
			QJsonObject snippet;
			if (!parseSnippet(content, snippet)) return false;
			item.insert(QStringLiteral("snippet"), snippet);
		} else if (field == 3) {
			QString name;
			if (!readString(content, 103, name)) return false;
			item.insert(QStringLiteral("authorDetails"),
				QJsonObject{{QStringLiteral("displayName"), name}});
		}
	}
	return true;
}

bool parseResponse(const QByteArray &raw, QJsonObject &page)
{
	ProtoReader fields(raw);
	QJsonArray items;
	while (!fields.done()) {
		quint32 field = 0;
		quint64 number = 0;
		QByteArray content;
		if (!fields.next(field, number, content)) return false;
		if (field == 100602) page.insert(QStringLiteral("nextPageToken"), QString::fromUtf8(content));
		else if (field == 2) page.insert(QStringLiteral("offlineAt"), QString::fromUtf8(content));
		else if (field == 1007) {
			QJsonObject item;
			if (!parseItem(content, item)) return false;
			items.append(item);
		}
	}
	page.insert(QStringLiteral("items"), items);
	return true;
}

struct Transfer {
	QObject *receiver = nullptr;
	YouTubeGrpcStream::PageHandler onPage;
	QByteArray pending;
	QString grpcMessage;
	int grpcStatus = -1;
	bool malformed = false;
};

size_t onBytes(char *data, size_t size, size_t count, void *user)
{
	const size_t length = size * count;
	auto &state = *static_cast<Transfer *>(user);
	if (length > size_t(maxFrame) || state.pending.size() + qsizetype(length) > maxFrame + 5) {
		state.malformed = true;
		return 0;
	}
	state.pending.append(data, qsizetype(length));
	while (state.pending.size() >= 5) {
		const auto *header = reinterpret_cast<const unsigned char *>(state.pending.constData());
		const quint32 length = (quint32(header[1]) << 24) | (quint32(header[2]) << 16) |
			(quint32(header[3]) << 8) | header[4];
		if (header[0] != 0 || length > quint32(maxFrame)) {
			state.malformed = true;
			return 0;
		}
		if (state.pending.size() < qsizetype(length) + 5) break;
		QJsonObject page;
		if (!parseResponse(state.pending.mid(5, length), page)) {
			state.malformed = true;
			return 0;
		}
		state.pending.remove(0, qsizetype(length) + 5);
		QMetaObject::invokeMethod(state.receiver, [handler = state.onPage, page] { handler(page); },
					  Qt::QueuedConnection);
	}
	return length;
}

size_t onHeader(char *data, size_t size, size_t count, void *user)
{
	const size_t length = size * count;
	auto &state = *static_cast<Transfer *>(user);
	const QByteArray header(data, qsizetype(length));
	if (header.startsWith("grpc-status:")) state.grpcStatus = header.mid(12).trimmed().toInt();
	else if (header.startsWith("grpc-message:"))
		state.grpcMessage = QUrl::fromPercentEncoding(header.mid(13).trimmed());
	return length;
}

} // namespace

YouTubeGrpcStream::YouTubeGrpcStream(QObject *receiver, const QString &accessToken, const QString &liveChatId,
				     const QString &pageToken, PageHandler page, CloseHandler close)
	: state(std::make_shared<State>())
{
	const auto shared = state;
	worker = QThread::create([receiver, accessToken, liveChatId, pageToken, page = std::move(page),
				  close = std::move(close), shared] {
		Transfer transfer{receiver, page};
		QString message;
		int grpcStatus = -1;
		CURL *curl = curl_easy_init();
		if (curl) {
			const QByteArray payload = requestFrame(liveChatId, pageToken);
			const QByteArray bearer = QByteArray("authorization: Bearer ") + accessToken.toUtf8();
			curl_slist *headers = nullptr;
			headers = curl_slist_append(headers, "content-type: application/grpc");
			headers = curl_slist_append(headers, "te: trailers");
			headers = curl_slist_append(headers, "grpc-accept-encoding: identity");
			headers = curl_slist_append(headers, bearer.constData());
			curl_easy_setopt(curl, CURLOPT_URL,
				"https://youtube.googleapis.com/youtube.api.v3.V3DataLiveChatMessageService/StreamList");
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.constData());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(payload.size()));
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBytes);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
				+[](void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
					return static_cast<State *>(user)->stopping.load() ? 1 : 0;
				});
			curl_easy_setopt(curl, CURLOPT_XFERINFODATA, shared.get());
			const CURLcode code = curl_easy_perform(curl);
			long http = 0;
			long httpVersion = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
			curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &httpVersion);
			grpcStatus = transfer.grpcStatus;
			if (transfer.malformed) message = QStringLiteral("Некорректный gRPC-пакет");
			else if (httpVersion != CURL_HTTP_VERSION_2_0) message = QStringLiteral("Требуется HTTP/2 (libcurl/nghttp2)");
			else if (http != 200) message = QStringLiteral("YouTube HTTP %1").arg(http);
			else if (grpcStatus > 0) message = QStringLiteral("gRPC %1: %2").arg(grpcStatus)
				.arg(transfer.grpcMessage.left(150));
			else if (code != CURLE_OK) message = QStringLiteral("gRPC transport: %1")
				.arg(QString::fromLatin1(curl_easy_strerror(code)));
			else message = QStringLiteral("Поток YouTube завершён");
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		} else message = QStringLiteral("libcurl недоступен");
		if (!shared->stopping.load())
			QMetaObject::invokeMethod(receiver, [close, grpcStatus, message] { close(grpcStatus, message); },
						  Qt::QueuedConnection);
	});
	worker->start();
}

YouTubeGrpcStream::~YouTubeGrpcStream()
{
	state->stopping.store(true);
	worker->wait();
	delete worker;
}

} // namespace xinkx
