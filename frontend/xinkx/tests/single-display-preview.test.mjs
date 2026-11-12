import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = path => readFileSync(new URL(path, import.meta.url), 'utf8');
const portrait = read('../PortraitCanvas.cpp');
const main = read('../../widgets/OBSBasic_Preview.cpp');
const header = read('../PortraitCanvas.hpp');

assert.match(main, /PortraitCanvas::instance\(\)\.renderPreview\(width, height\)/);
assert.match(main, /PortraitCanvas::instance\(\)\.previewVisible\(\)/);
assert.match(main, /workspaceWidth\(int\(cx\)\)/);
assert.match(main, /workspaceHeight\(int\(cy\)\)/);
assert.match(portrait, /PortraitCanvas::renderPreview\(/);
assert.match(portrait, /obs_source_video_render\(source\)/);
assert.match(portrait, /target->installEventFilter\(this\)/);
assert.match(portrait, /obs_canvas_create_private\("XINKX Portrait"/);
assert.match(header, /static PreviewRegion portraitPreviewRegion/);
assert.doesNotMatch(portrait, /obs_display_add_draw_callback/);
assert.doesNotMatch(portrait, /class PortraitDisplay : public OBSQTDisplay/);
assert.doesNotMatch(portrait, /new QSplitter\(Qt::Horizontal/);
assert.doesNotMatch(portrait, /previewGrid->removeWidget/);
console.log('XINKX: both GPU viewports share the OBS preview display');
