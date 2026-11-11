#pragma once

#include <QList>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>

namespace xinkx {

// Provider-independent event. No OAuth token, stream key, HTTP headers or
// arbitrary provider payload is accepted by the journal.
struct StudioEvent {
	QString id;              // Locally assigned occurrence ID.
	QString externalId;      // Stable provider event ID, if supplied.
	QString sourceId;        // Stable adapter/connection identity.
	QString platform;        // twitch, youtube, kick, donationalerts, ...
	QString accountId;       // Local account ID; may be empty for donation feeds.
	QString type;            // chat, follow, sub, donation, raid, ...
	QString name;
	QString message;
	QString amount;
	qint64 receivedAt = 0;
};

struct EventMetrics {
	quint64 accepted = 0;
	quint64 duplicates = 0;
	quint64 overflow = 0;
	int queued = 0;
	int stored = 0;
};

// One event ingress point for real provider notifications. Runs on the OBS
// UI thread; consumers receive normalized events through the same signal.
// Journal replay is explicit: previously seen donations/subscriptions MUST
// NOT trigger live overlays again on application startup.
class EventCore final : public QObject {
	Q_OBJECT
public:
	explicit EventCore(const QString &journalPath, QObject *parent = nullptr);
	~EventCore() override;
	bool publish(StudioEvent event);
	EventMetrics metrics() const;
	QList<StudioEvent> recent(int limit = 100) const;
	QString journalFile() const { return path; }
	void flush();

signals:
	void delivered(const xinkx::StudioEvent &event);
	void metricsChanged();
	void storageWarning(const QString &reason);

private:
	QString path;
	QQueue<StudioEvent> pending;
	QList<StudioEvent> history;
	QSet<QString> seen;
	QQueue<QString> seenOrder;
	QTimer saveTimer;
	quint64 acceptedCount = 0;
	quint64 duplicateCount = 0;
	quint64 overflowCount = 0;
	bool draining = false;
	bool dirty = false;

	static QString dedupKey(const StudioEvent &event);
	static bool sanitize(StudioEvent &event);
	void remember(const StudioEvent &event);
	void load();
	void drain();
};

} // namespace xinkx
