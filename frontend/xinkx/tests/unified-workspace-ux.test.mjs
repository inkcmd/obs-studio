import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const workspace = read('StudioWorkspace.cpp');
const portrait = read('PortraitCanvas.cpp');
const registry = read('EngineRegistry.cpp');
const chatSettings = read('StudioSettingsPages.cpp');
const streamer = read('PresentationWorkspace.cpp');
const profiles = read('GameProfilesWorkspace.cpp');
const broadcast = read('BroadcastWorkspace.cpp');
const catalog = read('OverlayRuntime.cpp');
const sources = read('NativeSources.cpp');
const overlay = read('assets/overlay.html');

assert.match(workspace, /context->setSizePolicy\(QSizePolicy::Expanding, QSizePolicy::Maximum\)/);
assert.match(workspace, /vertical->setStretchFactor\(canvas, 1\)/);
assert.match(workspace, /PortraitCanvas::instance\(\)\.setViewMode\(2\)/);
assert.doesNotMatch(workspace, /canvasMode->setCurrentIndex/);
assert.doesNotMatch(registry, /auto \*open = new QHBoxLayout/);

assert.match(portrait, /xinkxPortraitSourcesWorkspace/);
assert.match(portrait, /splitter->addWidget\(nativeFrame\)/);
assert.match(portrait, /splitter->addWidget\(portraitSources\)/);
assert.match(portrait, /entry->setData\(Qt::UserRole \+ 1, layer.key\)/);
assert.match(portrait, /obs_sceneitem_remove\(layer.item\)/);
assert.match(portrait, /Добавить выделенный источник 16:9 → 9:16/);
assert.doesNotMatch(portrait, /setVisible\(mode !=/);

assert.match(chatSettings, /chatWorkspace->addWidget\(previewPanel, 2\)/);
assert.match(chatSettings, /Чат поверх игры · только для меня/);
assert.match(chatSettings, /Чат на трансляции · для зрителей/);
assert.match(streamer, /enable->setChecked\(true\)/);

assert.match(profiles, /lastSelectedProfile/);
assert.match(profiles, /storeProfiles\(\*root\)/);
assert.match(broadcast, /Недоступен \/ завершён/);
assert.match(broadcast, /if \(activeIndex > 0\) youtubeEvent->setCurrentIndex\(activeIndex\)/);

assert.match(catalog, /Готовые примеры XINKX/);
assert.match(catalog, /Мои сохранённые пресеты/);
assert.match(catalog, /selectedIndex =/);
assert.match(sources, /renderQuality/);
assert.match(sources, /gs_matrix_scale3f\(scale, scale, 1\.f\)/);
assert.match(sources, /source->width \* source->renderScale/);
assert.match(overlay, /renderPixelRatio/);

console.log('XINKX unified preview UX, chat defaults, broadcast profile persistence, source presets and HiDPI source rendering OK');
