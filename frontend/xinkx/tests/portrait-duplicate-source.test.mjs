// OBS scene duplication must copy its saved independent portrait scene layout.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = path => readFileSync(new URL(path, import.meta.url), 'utf8');
const scenes = read('../../widgets/OBSBasic_Scenes.cpp');
const portrait = read('../PortraitCanvas.cpp');

assert.match(scenes, /obs_scene_duplicate\(curScene, name\.c_str\(\), OBS_SCENE_DUP_REFS\)/);
assert.match(scenes, /PortraitCanvas::instance\(\)\.duplicateLayout\(/);
assert.match(scenes, /duplicateLayout\([\s\S]*?SetCurrentScene\(source, true\)/);
assert.match(portrait, /void PortraitCanvas::duplicateLayout\(/);
assert.match(portrait, /settings\.contains\(storeKey\(toScene\)\)/);
assert.match(portrait, /settings\.setValue\(extraKey\(toScene\), settings\.value\(sourceExtras\)\)/);

console.log('XINKX portrait duplication contract: cloned layout and layers before switching scenes');
