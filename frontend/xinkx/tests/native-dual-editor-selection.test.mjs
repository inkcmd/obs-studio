import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const read = rel => readFileSync(new URL(rel, import.meta.url), 'utf8');
const canvas = read('../PortraitCanvas.cpp');
const preview = read('../../widgets/OBSBasicPreview.cpp');
const main = read('../../widgets/OBSBasic_Preview.cpp');
const sources = read('../../components/SourceTree.cpp');

// Both scenes use stock OBS hit testing, crop/rotate/drag, and handles.
assert.match(preview, /OBSScene OBSBasicPreview::EditingScene\(\) const/);
assert.match(preview, /OBSScene scene = Get\(\)->EditingScene\(\)/);
assert.match(preview, /GetStretchHandleData\(startPos, false\)/);
assert.match(preview, /DrawSelectedItem/);
assert.match(canvas, /native->DrawSceneEditing\(\)/);
assert.doesNotMatch(canvas, /preview->pressed =/);
assert.doesNotMatch(canvas, /preview->resized =/);
assert.doesNotMatch(canvas, /previewState\.hasSelection/);
assert.match(preview, /portraitEditing\.exchange\(portrait\)/);
assert.match(preview, /tree->selectionModel\(\)->clearSelection\(\)/);
assert.match(sources, /SourceTree::mousePressEvent/);
assert.match(sources, /SetPortraitEditing\(Scene\(\) == portrait\)/);
assert.match(canvas, /QItemSelectionModel::ClearAndSelect/);
assert.match(preview, /EditingSelectionChanged\(\)/);

// Editor-only portrait scaling is not a change to encoder dimensions.
assert.match(canvas, /video\.base_width = portraitWidth/);
assert.match(canvas, /video\.base_height = portraitHeight/);
assert.match(canvas, /return landscapeHeight;/);
assert.match(canvas, /portraitDisplayFactor\(int\(video\.base_height\)\)/);
assert.match(main, /if \(!window->ui->preview->IsPortraitEditing\(\)\)/);
console.log('XINKX one stock editor, exclusive scene selection, equal display height');
