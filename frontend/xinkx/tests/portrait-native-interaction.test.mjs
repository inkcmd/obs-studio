import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const canvas = read('PortraitCanvas.cpp');
const accounts = read('MultiStreamWorkspace.cpp');
const studio = read('StudioWorkspace.cpp');

for (const term of [
  'obs_canvas_scene_create(canvas',
  'obs_sceneitem_get_info2(layers.at(row).item, initialTransform.get())',
  'const QPointF delta = event->position() - start',
  'obs_sceneitem_set_info2(layers.at(row).item, &info)',
  'snapAxis((*initialBounds)[0] + dx',
  'const int selectedHandle = handleAt(selectedRow, px, py)',
  'if (selectedHandle > 0)',
  'matrix4_inv(&inverse, &box)',
  'contextRequested(float(point.x()), float(point.y()), event->globalPosition().toPoint())',
  'obs_frontend_open_source_filters(source)',
  'obs_sceneitem_set_order(item, upwards ? OBS_ORDER_MOVE_UP : OBS_ORDER_MOVE_DOWN)',
  'setZoomFactor(zoomFactor() *',
  'PortraitCanvas::setViewMode(int mode)',
  'QStringLiteral("order"), index',
  'gs_draw_quadf(nullptr',
  'gs_set_viewport(0, 0, cx, cy)',
  'xinkxPortraitSourcesWorkspace',
  'PortraitCanvas::portraitPreviewRegion',
  'nativePreview->installEventFilter(this)',
  'previewState.visible.store(true',
  'PortraitCanvas::renderPreview',
  'xinkxPortraitSourceList',
  'xinkxSourcesSplitter',
  'xinkxPortraitNativeSourceTree',
  'xinkxPortraitSourcesToolbar',
  'new OBSBasicSourceSelect(main, main->undo_s',
  'splitter->addWidget(nativeFrame)',
  'splitter->addWidget(portraitSources)',
  'const auto selectedLayer = [=]',
  'entry->setData(Qt::UserRole + 1, layer.key)',
  'layer.key != key',
  'obs_source_create("game_capture", name.toUtf8().constData()',
  'Добавить выделенный источник 16:9 → 9:16',
  'OBS_SOURCE_DO_NOT_DUPLICATE',
  'std::vector<Captured> mainSources',
  'if (!store.contains(extraKey(name)))',
  'obs_source_duplicate(original',
  'Использовать источник OBS · общие свойства',
  'Создать независимую копию источника',
  'saveDelay->setSingleShot(true)',
]) assert.ok(canvas.includes(term), `missing portrait editor contract ${term}`);

// Standard OBS Sources stays bound to horizontal editing; the portrait list
// manages only portrait scene items, even if they share a source UUID.
assert.doesNotMatch(canvas, /xinkxSourceCanvasTabs/);
assert.doesNotMatch(canvas, /xinkxUnifiedSourcesWorkspace/);
assert.doesNotMatch(canvas, /xinkxLandscapeSourceMode/);
assert.doesNotMatch(canvas, /xinkxPortraitSourceMode/);
assert.doesNotMatch(canvas, /xinkxCanvasSplitter/);
assert.match(canvas, /guardedFrame->setParent\(guardedContents\)/);
assert.match(canvas, /obs_sceneitem_remove\(layer\.item\)/);
assert.doesNotMatch(canvas, /on_actionRemoveSource_triggered/);
assert.match(canvas, /obs_frontend_open_source_properties\(source\)/);

assert.doesNotMatch(studio, /xinkxCanvasMode/);
assert.match(studio, /PortraitCanvas::instance\(\)\.setViewMode\(2\)/);
assert.match(accounts, /PortraitCanvas::instance\(\)\.setViewMode\(2\)/);
assert.doesNotMatch(canvas, /setVisible\(mode !=/);

console.log('XINKX dual persistent canvases, independent scene-item lists, context actions and snapping OK');
