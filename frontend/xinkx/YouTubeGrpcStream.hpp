#pragma once

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QObject;
class QThread;

namespace xinkx {

// Official YouTube liveChatMessages.streamList server-streaming RPC over
// libcurl HTTP/2. The framing/protobuf subset is private to this class;
// callbacks are delivered on the ChatGateway QObject thread.
class YouTubeGrpcStream final {
public:
	using PageHandler = std::function<void(const QJsonObject &)>;
	using CloseHandler = std::function<void(int, const QString &)>;
	YouTubeGrpcStream(QObject *receiver, const QString &accessToken, const QString &liveChatId,
			  const QString &pageToken, PageHandler page, CloseHandler close);
	~YouTubeGrpcStream();
	YouTubeGrpcStream(const YouTubeGrpcStream &) = delete;
	YouTubeGrpcStream &operator=(const YouTubeGrpcStream &) = delete;

private:
	struct State {
		std::atomic<bool> stopping{false};
	};
	std::shared_ptr<State> state;
	QThread *worker = nullptr;
};

} // namespace xinkx
