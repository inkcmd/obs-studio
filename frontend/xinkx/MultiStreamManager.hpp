#pragma once

#include <QObject>
#include <QString>

#include <array>

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

	bool start(int index, const StreamDestination &destination, QString *error = nullptr);
	void stop(int index);
	bool active(int index) const;
	QString description(int index) const;
	bool anythingActive() const;

signals:
	void destinationChanged(int index);

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

	std::array<Session, 3> sessions{};
	void release(Session &session);
	void updateStatus();
};

} // namespace xinkx
