#pragma once

#include "EventCore.hpp"

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QThreadPool>

#include <atomic>

class OBSBasic;
class QNetworkReply;

namespace xinkx {

// The endpoint is deliberately fixed to loopback. No cloud account/API key is
// required, and the model cannot invoke OBS actions without a user click.
class LocalLLM final : public QObject {
	Q_OBJECT
public:
	explicit LocalLLM(OBSBasic *window);
	void ask(const QString &model, const QString &prompt, bool allowLive);
	void reset();
	bool pending() const { return active != nullptr; }

signals:
	void answer(const QString &content);
	void failed(const QString &reason);
	void busyChanged(bool busy);

private:
	OBSBasic *window;
	QNetworkAccessManager network;
	QNetworkReply *active = nullptr;
	QJsonArray memory;
};

// Windows SAPI on a serialized worker; never blocks the OBS render/UI thread.
class SpeechEngine final : public QObject {
	Q_OBJECT
public:
	explicit SpeechEngine(OBSBasic *window);
	void say(const QString &text);
	void setAnnouncePaid(bool enabled);
	bool announcePaid() const { return paidEnabled; }

private:
	bool paidEnabled = false;
	QThreadPool workers;
	std::atomic<int> pendingSpeeches{0};
};

void installAssistantWorkspace(OBSBasic *window);

} // namespace xinkx
