import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = path => readFileSync(new URL(path, import.meta.url), 'utf8');
const portrait = read('../PortraitCanvas.cpp');
const tree = read('../../components/SourceTree.cpp');
const model = read('../../components/SourceTreeModel.cpp');
const item = read('../../components/SourceTreeItem.cpp');
const dialog = read('../../dialogs/OBSBasicSourceSelect.cpp');
const youtube = read('../ChatGateway.cpp');
const broadcast = read('../BroadcastWorkspace.cpp');

// One OBSQTDisplay presents both GPU viewports; no secondary render surface.
assert.match(portrait, /new PortraitDisplay\(nativePreview, side\)/);
assert.match(portrait, /PortraitCanvas::renderPreview/);
assert.doesNotMatch(portrait, /new QSplitter\(Qt::Horizontal/);
assert.doesNotMatch(portrait, /class PortraitDisplay : public OBSQTDisplay/);
assert.doesNotMatch(portrait, /Вертикальный холст · 9:16/);
assert.doesNotMatch(portrait, /0\.38f, 0\.51f, 0\.65f/);

// Real OBS SourceTree (icons, visibility, locking, drag/drop, rename) and its
// original source-type selector are retargeted to the independent scene.
assert.match(portrait, /new SourceTree\(portraitSources, \[this\] \{ return OBSScene\(scene\); \}\)/);
assert.match(portrait, /xinkxPortraitSourcesToolbar/);
assert.match(portrait, /new OBSBasicSourceSelect\(main, main->undo_s/);
assert.match(dialog, /if \(targetSceneAdd\)/);
assert.match(tree, /sceneProvider \? sceneProvider\(\) : OBSBasic::Get\(\)->GetCurrentScene\(\)/);
assert.match(model, /OBSScene scene = st->Scene\(\)/);
assert.match(item, /OBSScene scene = tree->Scene\(\)/);
assert.match(portrait, /obs_scene_enum_items\(scene, collectItemRefs, &actualItems\)/);
assert.match(portrait, /Snapshot even shared inputs/);

// Archive items cannot crowd out active/upcoming events or be restored as
// selectable pseudo-events; Check can fetch a new selection without a saved ID.
assert.match(youtube, /QStringLiteral\("broadcastStatus"\), status/);
assert.match(youtube, /QStringLiteral\("active"\), QStringLiteral\("upcoming"\)/);
assert.doesNotMatch(youtube, /QStringLiteral\("mine"\), QStringLiteral\("true"\)/);
assert.doesNotMatch(broadcast, /youtubeEvent->addItem\(QStringLiteral\("Недоступен \/ завершён/);
assert.match(broadcast, /gateway->inspectBroadcast\(account.id, broadcastId\)/);

console.log('XINKX embedded two-canvas native source list and live/upcoming YouTube selection OK');
