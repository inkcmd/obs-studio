#pragma once

#include <cstdint>

#include <QString>
#include <QVector>

#include <obs.hpp>

class QWidget;

namespace xinkx {

// A separate libobs video mix and scene. Sources are shared with the main scene,
// but every portrait scene item has its own independent transform and bounds.
class PortraitCanvas final {
public:
	static PortraitCanvas &instance();
	video_t *video();
	void sync();
	// Two viewports on OBS's existing display; the portrait video mix remains separate.
	struct PreviewRegion {
		int x, y, width, height;
		bool contains(float px, float py) const
		{
			return px >= x && px < x + width && py >= y && py < y + height;
		}
	};
	static int landscapePreviewWidth(int displayWidth);
	static PreviewRegion portraitPreviewRegion(int displayWidth, int displayHeight);
	bool previewVisible() const;
	void renderPreview(uint32_t displayWidth, uint32_t displayHeight) const;
	// OBS duplicates the horizontal scene separately; clone the portrait
	// layout at the same time, retaining independent transform storage.
	void duplicateLayout(const QString &fromScene, const QString &toScene);
	void edit(QWidget *parent);
	// The active OBS scene drives both compositions. The 16:9 and 9:16
	// previews remain visible together in the normal OBS preview container.
	void setViewMode(int mode);
	void closeEditor();
	void shutdown();

private:
	PortraitCanvas() = default;
	~PortraitCanvas() = default;
	PortraitCanvas(const PortraitCanvas &) = delete;
	PortraitCanvas &operator=(const PortraitCanvas &) = delete;

	struct Layer {
		QString key;
		QString name;
		obs_sceneitem_t *item = nullptr; // Owned by scene; never persisted across sync().
	};

	obs_canvas_t *canvas = nullptr;
	obs_scene_t *scene = nullptr;
	QString sceneName;
	QString signature;
	bool editing = false;
	QVector<Layer> layers;

	bool ensure();
	void rebuild(obs_scene_t *mainScene, const QString &name, const QString &newSignature);
	void updateLayer(const QString &key, float x, float y, float width, float height, bool fill);
	void save();
};

} // namespace xinkx
