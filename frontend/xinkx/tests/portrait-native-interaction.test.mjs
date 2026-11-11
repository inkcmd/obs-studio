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
  'xinkxUnifiedSourcesWorkspace',
  'xinkxCanvasSplitter',
  'splitter->addWidget(landscapePane)',
  'splitter->addWidget(side)',
  'guardedSplitter->deleteLater()',
  'xinkxUnifiedSourceList',
  'xinkxLandscapeSourceMode',
  'xinkxPortraitSourceMode',
  'xinkxAdvancedSourceTree',
  'sourceStack->addWidget(nativeFrame)',
  'const auto selectedLayer = [=]',
  'const auto setActiveCanvas = [=](int target, bool selectFromList)',
  'new LandscapeCanvasFocus([=] { setActiveCanvas(0, false); }, side)',
  'setActiveCanvas(1, false)',
  'obs_sceneitem_select(item, source && !uuid->isEmpty()',
  'obs_source_create("game_capture", name.toUtf8().constData()',
  'OBS_SOURCE_DO_NOT_DUPLICATE',
  'std::vector<Captured> mainSources',
  'if (!store.contains(extraKey(name)))',
  'obs_source_duplicate(original',
  'Использовать источник OBS · общие свойства',
  'Создать независимую копию источника',
  'saveDelay->setSingleShot(true)',
]) assert.ok(canvas.includes(term), `missing portrait editor contract ${term}`);

// A single source list now represents both render compositions. The stock
// OBS source tree is preserved under Advanced rather than duplicated in tabs.
assert.doesNotMatch(canvas, /xinkxSourceCanvasTabs/);
assert.doesNotMatch(canvas, /LandscapeTabFocus/);
assert.match(canvas, /guardedStack->removeWidget\(guardedFrame\)/);
assert.match(canvas, /guardedFrame->setParent\(guardedContents\)/);
assert.match(canvas, /horizontalVisibility\.contains\(uuid\)/);
assert.match(canvas, /portraitVisibility\.contains\(uuid\)/);
assert.match(canvas, /obs_enum_scenes\(/);
assert.match(canvas, /Изменение настроек захвата затронет все/);
assert.match(canvas, /QStringLiteral\("\[?16:9"\)/);
assert.match(canvas, /QMetaObject::invokeMethod\(mainWindow, "on_actionRemoveSource_triggered"/);
assert.match(canvas, /obs_frontend_open_source_properties\(source\)/);

assert.match(studio, /canvasMode->addItems\(/);
assert.match(studio, /PortraitCanvas::instance\(\)\.setViewMode\(mode\)/);
assert.match(accounts, /enabledPortrait/);
assert.match(accounts, /PortraitCanvas::instance\(\)\.setViewMode\(mode\)/);

console.log('XINKX portrait source isolation, shared scene selection, preview modes, context actions and transform snapping OK');
