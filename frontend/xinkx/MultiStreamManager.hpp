#pragma once

#include <QObject>
#include <QString>

#include <vector>

#include <obs.h>

namespace xinkx {

// The platform is a destination, not an OBS profile. Each destination owns its
// own RTMP service, output, H.264 video encoder and AAC audio encoder.
struct StreamDestination {
	QString url;
	QString key;
	QString encoder = QStringLiteral("auto");
	int bitrateKbps = 6000;
	int width = 1920;
	int height = 1080;
};

class MultiStreamManager final : public QObject {
	Q_OBJECT

public:
	explicit MultiStreamManager(QObject *parent = nullptr);
	~MultiStreamManager() override;
	void shutdown();
	void startConfigured();
	void stopAll();
	int activeCount() const;
	int configuredCount() const;
	uint64_t totalBytes() const;
	int totalDroppedFrames() const;
	int totalVideoFrames() const;

	bool start(int index, const StreamDestination &destination, QString *error = nullptr);
	void stop(int index);
	bool active(int index) const;
	QString description(int index) const;
	bool anythingActive() const;

signals:
	void destinationChanged(int index);
	void streamError(const QString &message);

private:
	struct Session {
		obs_output_t *output = nullptr;
		obs_service_t *service = nullptr;
		obs_encoder_t *video = nullptr;
		obs_encoder_t *audio = nullptr;
		bool connecting = false;
		bool stopRequested = false;
		bool started = false;
		QString message;
		QString lastDisplay;
	};

	std::vector<Session> sessions;
	void release(Session &session);
	void updateStatus();
};

} // namespace xinkx
