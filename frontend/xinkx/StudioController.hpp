#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

namespace xinkx {

struct SelectedTransform {
	qint64 itemId = -1;
	double x = 0.0;
	double y = 0.0;
	double width = 0.0;
	double height = 0.0;
	bool locked = false;
	bool resizable = false;

	bool operator==(const SelectedTransform &other) const
	{
		return itemId == other.itemId && x == other.x && y == other.y && width == other.width &&
		       height == other.height && locked == other.locked && resizable == other.resizable;
	}
};

// OBS scenes are authoritative; the inspector only reads/writes scene item transforms.
class StudioController final : public QObject {
	Q_OBJECT

public:
	explicit StudioController(QObject *parent = nullptr);
	bool applySelectedTransform(qint64 itemId, double x, double y, double width, double height);

signals:
	void stateChanged(const QString &sceneName, const QString &selectedSource, bool streaming, bool recording);
	void selectedTransformChanged(const SelectedTransform &transform);

private:
	void refresh();

	QString previousScene;
	QString previousSource;
	SelectedTransform previousTransform;
	bool previousStreaming = false;
	bool previousRecording = false;
	bool firstRefresh = true;
};

} // namespace xinkx

Q_DECLARE_METATYPE(xinkx::SelectedTransform)
