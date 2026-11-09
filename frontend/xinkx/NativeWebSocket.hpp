#pragma once

#include <QByteArray>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

class QObject;
class QThread;

namespace xinkx {

// Windows WinHTTP provides TLS WebSockets without adding QtWebSockets or Node.
// The worker never touches Qt widgets and is joined before its owner exits.
class NativeWebSocket final {
public:
	using MessageHandler = std::function<void(const QByteArray &)>;
	using CloseHandler = std::function<void(const QString &)>;
	using OpenHandler = std::function<void()>;
	NativeWebSocket(QObject *receiver, const QUrl &url, MessageHandler onMessage, CloseHandler onClose,
			OpenHandler onOpen = {}, const QByteArray &origin = {});
	~NativeWebSocket();
	bool send(const QByteArray &message);
	NativeWebSocket(const NativeWebSocket &) = delete;
	NativeWebSocket &operator=(const NativeWebSocket &) = delete;

private:
	struct State {
		std::atomic<bool> stopping{false};
		std::atomic<void *> socket{nullptr};
	};
	std::shared_ptr<State> state;
	QThread *worker = nullptr;
};

} // namespace xinkx
