#include "NativeWebSocket.hpp"

#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <string>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#include <winhttp.h>
#endif

namespace xinkx {

NativeWebSocket::NativeWebSocket(QObject *receiver, const QUrl &url, MessageHandler onMessage, CloseHandler onClose,
				 OpenHandler onOpen, const QByteArray &origin)
	: state(std::make_shared<State>())
{
	const auto shared = state;
	worker = QThread::create([receiver, url, onMessage = std::move(onMessage), onClose = std::move(onClose),
				 onOpen = std::move(onOpen), origin, shared] {
		QString result = QStringLiteral("Соединение завершено");
#ifdef _WIN32
		HINTERNET session = nullptr;
		HINTERNET connection = nullptr;
		HINTERNET request = nullptr;
		const auto failure = [&result](const QString &where) {
			result = QStringLiteral("%1: WinHTTP %2").arg(where).arg(GetLastError());
		};
		do {
			const bool localCdp = url.scheme() == QStringLiteral("ws") &&
				(url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("localhost"));
			if ((!localCdp && url.scheme() != QStringLiteral("wss")) || url.host().isEmpty()) {
				result = QStringLiteral("Разрешён WSS либо локальный CDP WebSocket");
				break;
			}
			session = WinHttpOpen(L"XINKX Studio/0.15", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
					      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!session) { failure(QStringLiteral("WinHttpOpen")); break; }
			WinHttpSetTimeouts(session, 5000, 5000, 5000, 65000);
			const std::wstring host = url.host().toStdWString();
			connection = WinHttpConnect(session, host.c_str(),
						      url.port(localCdp ? 80 : 443), 0);
			if (!connection) { failure(QStringLiteral("WinHttpConnect")); break; }
			QString path = url.path(QUrl::FullyEncoded);
			if (path.isEmpty()) path = QStringLiteral("/");
			const QString query = url.query(QUrl::FullyEncoded);
			if (!query.isEmpty()) path += QStringLiteral("?") + query;
			const std::wstring object = path.toStdWString();
			request = WinHttpOpenRequest(connection, L"GET", object.c_str(), nullptr,
						 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
						 localCdp ? 0 : WINHTTP_FLAG_SECURE);
			if (!request) { failure(QStringLiteral("WinHttpOpenRequest")); break; }
			if (!origin.isEmpty()) {
				const std::wstring header = (QStringLiteral("Origin: ") + QString::fromLatin1(origin) +
					QStringLiteral("\r\n")).toStdWString();
				if (!WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(-1),
							 WINHTTP_ADDREQ_FLAG_ADD)) {
					failure(QStringLiteral("WebSocket Origin")); break;
				}
			}
			if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
			    !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
						  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
			    !WinHttpReceiveResponse(request, nullptr)) {
				failure(QStringLiteral("WebSocket handshake")); break;
			}
			DWORD status = 0, length = sizeof(status);
			if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
						 nullptr, &status, &length, nullptr) || status != 101) {
				result = QStringLiteral("WSS: HTTP %1").arg(status); break;
			}
			HINTERNET websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
			if (!websocket) { failure(QStringLiteral("WebSocket upgrade")); break; }
			shared->socket.store(websocket);
			WinHttpCloseHandle(request);
			request = nullptr;
			if (onOpen) QMetaObject::invokeMethod(receiver, [onOpen] { onOpen(); }, Qt::QueuedConnection);
			QByteArray message;
			char buffer[8192];
			while (!shared->stopping.load()) {
				DWORD read = 0;
				WINHTTP_WEB_SOCKET_BUFFER_TYPE kind{};
				const DWORD error = WinHttpWebSocketReceive(websocket, buffer, sizeof(buffer), &read, &kind);
				if (error != NO_ERROR) {
					result = QStringLiteral("WSS: WinHTTP %1").arg(error);
					break;
				}
				if (kind == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
				if (kind != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
				    kind != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
					message.clear();
					continue;
				}
				if (message.size() + static_cast<int>(read) > 2 * 1024 * 1024) {
					result = QStringLiteral("Слишком большой WSS-пакет");
					break;
				}
				message.append(buffer, static_cast<int>(read));
				if (kind == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
					const QByteArray complete = message;
					message.clear();
					QMetaObject::invokeMethod(receiver, [onMessage, complete] { onMessage(complete); },
								  Qt::QueuedConnection);
				}
			}
			if (shared->socket.exchange(nullptr) == websocket) WinHttpCloseHandle(websocket);
		} while (false);
		if (request) WinHttpCloseHandle(request);
		if (connection) WinHttpCloseHandle(connection);
		if (session) WinHttpCloseHandle(session);
#else
		result = QStringLiteral("Нативный WSS сейчас доступен только в Windows");
#endif
		if (!shared->stopping.load()) {
			QMetaObject::invokeMethod(receiver, [onClose, result] { onClose(result); }, Qt::QueuedConnection);
		}
	});
	worker->start();
}

bool NativeWebSocket::send(const QByteArray &message)
{
#ifdef _WIN32
	if (message.isEmpty() || message.size() > 65536 || state->stopping.load()) return false;
	const auto handle = state->socket.load();
	if (!handle) return false;
	return WinHttpWebSocketSend(static_cast<HINTERNET>(handle),
				    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
				    const_cast<char *>(message.constData()), static_cast<DWORD>(message.size())) == NO_ERROR;
#else
	Q_UNUSED(message);
	return false;
#endif
}

NativeWebSocket::~NativeWebSocket()
{
	state->stopping.store(true);
#ifdef _WIN32
	if (const auto handle = state->socket.exchange(nullptr)) {
		WinHttpCloseHandle(static_cast<HINTERNET>(handle));
	}
#endif
	worker->wait();
	delete worker;
}

} // namespace xinkx
