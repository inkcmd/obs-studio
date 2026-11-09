#pragma once

#include <QJsonObject>
#include <QObject>

class OBSBasic;

namespace xinkx {

// Rendering preferences only; OBS remains the only scene graph.
class PresentationSettings final : public QObject {
	Q_OBJECT
public:
	explicit PresentationSettings(QObject *parent = nullptr);
	QJsonObject snapshot() const { return document; }
	static QJsonObject defaults();
	static bool validate(const QJsonObject &value, QString *error = nullptr);
	bool save(const QJsonObject &value, QString *error = nullptr);
	QString path() const;

signals:
	void changed(const QJsonObject &document);

private:
	QJsonObject document;
};

void installPresentationWorkspace(OBSBasic *mainWindow);

} // namespace xinkx
