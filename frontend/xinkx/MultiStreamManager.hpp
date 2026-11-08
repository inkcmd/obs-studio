#pragma once

#include "StreamAccounts.hpp"

#include <QObject>
#include <QHash>
#include <QElapsedTimer>

#include <obs.hpp>

#include <memory>
#include <vector>

class QMenu;

namespace xinkx {

OBSServiceAutoRelease createStreamService(const StreamDestination &destination, const char *name);
OBSEncoderAutoRelease createStreamEncoder(const StreamDestination &destination, const char *name);

class MultiStreamManager final : public QObject {
	Q_OBJECT

public:
	explicit MultiStreamManager(QObject *parent = nullptr);
	~MultiStreamManager() override;

	bool hasConfiguredAccounts() const;
	int startConfigured();
	void stopAll(bool force = false);
	void shutdown();
	bool anythingActive() const;
	int activeCount() const;
	int configuredCount() const;
	uint64_t totalBytes() const;
	uint64_t extraBitrate() const;
	float worstCongestion() const;
	int totalDroppedFrames() const;
	int totalVideoFrames() const;
	QString sessionDetails() const;
	void populateMenu(QMenu *menu);

signals:
	void statusChanged();
	void activityChanged(bool active);

private:
	struct OutputStats {
		QElapsedTimer timer;
		uint64_t bytes = 0;
		qint64 sampleTime = 0;
		uint64_t kbps = 0;
		void sample(obs_output_t *output, qint64 now);
		QString liveDescription(obs_output_t *output) const;
	};

	struct Session {
		MultiStreamManager *manager;
		quint64 serial;
		StreamAccount account;
		OBSServiceAutoRelease service;
		OBSOutputAutoRelease output;
		OBSSignal stopped;
		OutputStats stats;
		bool running = false;
		bool stopping = false;
		bool failed = false;
		QString description() const;
	};

	QHash<QString, bool> selection;
	QVector<StreamAccount> configured;
	QElapsedTimer clock;
	std::vector<std::unique_ptr<Session>> sessions;
	bool lastActive = false;
	quint64 nextSerial = 0;
	bool enabled(const StreamAccount &account) const;
	void start(const StreamAccount &account);
	void stop(Session &session, bool force);
	void updateStatus();
	static void outputStopped(void *data, calldata_t *params);
};

} // namespace xinkx
