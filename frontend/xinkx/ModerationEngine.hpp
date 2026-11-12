#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QStringList>

namespace xinkx {

// Local incoming-chat filter. This does not ban users or delete messages on
// Twitch, YouTube or Kick; provider moderation requires separate OAuth scopes.
class ModerationEngine final : public QObject {
	Q_OBJECT
public:
	struct Configuration {
		bool enabled = false; // Opt-in: do not change existing streams on upgrade.
		bool blockLinks = false;
		bool blockRepeats = true;
		bool blockFlood = true;
		int maxMessages = 5;
		int windowSeconds = 10;
		QStringList blockedPhrases;
	};

	struct Counters {
		quint64 passed = 0;
		quint64 phrases = 0;
		quint64 links = 0;
		quint64 repeats = 0;
		quint64 flood = 0;
		quint64 suppressed() const { return phrases + links + repeats + flood; }
	};

	explicit ModerationEngine(QObject *parent = nullptr);
	Configuration configuration() const { return config; }
	Counters counters() const { return stats; }
	bool updateConfiguration(const Configuration &value, QString *error = nullptr);
	bool allowChat(const QString &platform, const QString &accountId, const QString &viewer,
		       const QString &message, qint64 nowMs = -1);

signals:
	void configurationChanged();
	void countersChanged();

private:
	struct ViewerState {
		QQueue<qint64> messages;
		QString previous;
		qint64 previousAt = 0;
		qint64 lastSeen = 0;
	};

	QString path;
	Configuration config;
	Counters stats;
	QHash<QString, ViewerState> viewers;
	bool notifyPending = false;

	static bool validate(const Configuration &value, QString *error);
	void load();
	void notifyCounters();
	void pruneViewers(qint64 now);
};

} // namespace xinkx
