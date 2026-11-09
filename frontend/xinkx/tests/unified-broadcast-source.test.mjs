// Static contract checks for native UI wiring where Qt/MSVC is unavailable.
// These are source-level guards, NOT a native build or provider integration test.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const source = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const broadcast = source('BroadcastWorkspace.cpp');
const profiles = source('GameProfilesWorkspace.cpp');
const api = source('ChatGateway.cpp');
const studio = source('YouTubeStudioBridge.cpp');

assert.match(broadcast, /setObjectName\(QStringLiteral\("xinkxBroadcastSave"\)\)/);
assert.equal((broadcast.match(/new QPushButton\(QStringLiteral\("Сохранить/g) || []).length, 1);
assert.doesNotMatch(profiles, /new QPushButton\(QStringLiteral\("Применить/);
assert.doesNotMatch(profiles, /new QPushButton\(QStringLiteral\("Сохранить/);
assert.match(profiles, /setProperty\("xinkxProfileReady", false\)/);
assert.match(broadcast, /new QCompleter\(model, panel\)/);
assert.match(broadcast, /combo->hide\(\)/);
assert.match(broadcast, /youtubeEvent->hide\(\)/);
assert.match(broadcast, /setProperty\("xinkxYoutubeMid", id\)/);
assert.match(broadcast, /pendingThumbnail->take\(id\)/);
assert.match(api, /videoCategories.*\n.*QStringLiteral\("20"\)/);
assert.match(api, /uploadType"\), QStringLiteral\("media"\)/);
assert.match(api, /thumbnails\/set/);
assert.match(studio, /bool YouTubeStudioBridge::hasProfile\(\) const/);

console.log('XINKX unified broadcast source contract: one Save, hidden mappings, YouTube category and thumbnails OK');
