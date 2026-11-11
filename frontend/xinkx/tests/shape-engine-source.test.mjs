import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const shape = read('ShapeEngine.cpp');
const sources = read('NativeSources.cpp');
const overlay = read('OverlayRuntime.cpp');
const build = read('../cmake/feature-xinkx-studio.cmake');

assert.match(build, /xinkx\/ShapeEngine\.cpp/);
assert.match(sources, /registerShapeSource\(\)/);
assert.match(shape, /info.id = "xinkx_shape"/);
assert.match(shape, /obs_register_source\(&info\)/);
assert.match(shape, /OBS_SOURCE_VIDEO \| OBS_SOURCE_CUSTOM_DRAW/);
assert.match(shape, /QPainter painter\(&image\)/);
assert.match(shape, /QLinearGradient fade\(/);
assert.match(shape, /gs_texture_create\(/);
assert.match(shape, /obs_source_draw\(shape->texture/);
assert.match(shape, /gs_texture_destroy\(shape->texture\)/);
assert.match(shape, /obs_enter_graphics\(\)/);
assert.doesNotMatch(shape, /browser_source|cef_|QTcpServer/);
assert.match(shape, /OBSBasic::BackupScene\(scene\)/);
assert.match(shape, /CreateSceneUndoRedoAction\(/);
assert.match(overlay, /showShapeCreator\(mainWindow, dock\)/);

console.log('XINKX phase 3: native libobs GPU primitives, scene lifecycle and editing contract OK');
