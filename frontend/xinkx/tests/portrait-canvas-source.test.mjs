// Native code integration contract; physical video/encoder verification remains a Windows test.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const portrait = read('PortraitCanvas.cpp');
const outputs = read('MultiStreamManager.cpp');
const settings = read('MultiStreamWorkspace.cpp');
const storage = read('StreamAccounts.cpp');

assert.match(portrait, /obs_canvas_create_private\("XINKX Portrait", &video/);
assert.match(portrait, /video\.base_width = portraitWidth/);
assert.match(portrait, /video\.base_height = portraitHeight/);
assert.match(portrait, /obs_canvas_scene_create/);
assert.match(portrait, /obs_canvas_set_channel\(canvas, 0, obs_scene_get_source\(scene\)\)/);
assert.match(portrait, /obs_scene_add\(scene, source\.source\)/);
assert.match(portrait, /obs_sceneitem_set_info2\(item, &info\)/);
assert.match(portrait, /store\.setValue\(storeKey\(sceneName\)/);
assert.match(portrait, /class PortraitDisplay : public QWidget/);
assert.doesNotMatch(portrait, /class PortraitDisplay : public OBSQTDisplay/);
assert.match(portrait, /PortraitCanvas::renderPreview/);
assert.match(outputs, /account\.destination\.portrait \? PortraitCanvas::instance\(\)\.video\(\) : obs_get_video\(\)/);
assert.match(outputs, /obs_encoder_set_video\(encoder, video \? video : obs_get_video\(\)\)/);
assert.match(settings, /Редактировать вертикальную композицию/);
assert.match(storage, /account\.destination\.portrait = settings\.value/);
assert.match(storage, /settings\.setValue\(QStringLiteral\("portrait"\)/);

console.log('XINKX portrait: independent libobs video mix, source transforms, persistent scene editor and output routing OK');
