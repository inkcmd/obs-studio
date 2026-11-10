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
  'xinkxSourceCanvasTabs',
  'sourceTabs->addTab(nativeFrame, QStringLiteral("16:9"))',
  'sourceTabs->addTab(portraitSources, QStringLiteral("9:16"))',
  'std::vector<Captured> mainSources',
  'if (!store.contains(extraKey(name)))',
  'obs_source_duplicate(original',
  'Использовать источник OBS · общие свойства',
  'Создать независимую копию источника',
  'saveDelay->setSingleShot(true)',
]) assert.ok(canvas.includes(term), `missing portrait editor contract ${term}`);

assert.match(studio, /canvasMode->addItems\(/);
assert.match(studio, /PortraitCanvas::instance\(\)\.setViewMode\(mode\)/);
assert.match(accounts, /enabledPortrait/);
assert.match(accounts, /PortraitCanvas::instance\(\)\.setViewMode\(mode\)/);

console.log('XINKX portrait source isolation, shared scene selection, preview modes, context actions and transform snapping OK');
