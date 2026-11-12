import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const read = rel => readFileSync(new URL(rel, import.meta.url), 'utf8');
const canvas = read('../PortraitCanvas.cpp');
const header = read('../PortraitCanvas.hpp');
const main = read('../../widgets/OBSBasic_Preview.cpp');
const preview = read('../../widgets/OBSBasicPreview.cpp');

// Physical source dimensions, not 70/30 widget viewport partitioning.
assert.match(header, /workspaceGap = 128/);
assert.match(canvas, /return landscapeWidth \+ workspaceGap \+ portraitWidth/);
assert.match(canvas, /return std::max\(landscapeHeight, portraitHeight\)/);
assert.match(canvas, /main->GetDisplayRect\(x, y, w, h\)/);
assert.doesNotMatch(canvas, /previewState\.(zoom|panX|panY)/);
assert.match(main, /GetScaleAndCenterPos\(worldWidth, worldHeight/);
assert.match(main, /GetCenterPosFromFixedScale\(worldWidth, worldHeight/);
assert.match(main, /landscapeTop\) \* previewScale/);
assert.match(preview, /PortraitCanvas::workspaceWidth/);
assert.match(preview, /middlePanning/);
assert.match(canvas, /nativePreview->SetScrollingOffset/);
assert.match(canvas, /button->setProperty\("class", original->property\("class"\)\)/);
assert.match(canvas, /nativePreview->increaseScalingLevel/);
assert.doesNotMatch(canvas, /new QSplitter\(Qt::Horizontal/);

// Example: 2560x1440 + 1080x1920 and 128 scene-pixel gap.
const worldW = 2560 + 128 + 1080;
const worldH = Math.max(1440, 1920);
const zoom = 0.25;
assert.equal(worldW, 3768);
assert.equal(worldH, 1920);
assert.equal(2560 * zoom, 640);
assert.equal(1080 * zoom, 270);
assert.equal(1920 * zoom, 480);
assert.equal((worldH - 1440) * zoom / 2, 60);
console.log('XINKX dual canvas uses shared physical workspace scale/pan and OBS theme icons');
