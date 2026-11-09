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
assert.match(broadcast, /Категория YouTube/);
assert.match(broadcast, /Эфир YouTube/);
assert.doesNotMatch(broadcast, /youtubeEvent->hide\(\)/);
assert.doesNotMatch(broadcast, /studio->connectChannel\(youtubeAccount\(\)\.providerId\)/);
assert.match(broadcast, /setProperty\("xinkxYoutubeMid", id\)/);
assert.match(broadcast, /pendingThumbnail->take\(id\)/);
assert.match(api, /videoCategories.*\n.*regionCode/);
assert.match(api, /youtube\/v3\/videos\?part=snippet/);
assert.match(api, /uploadType"\), QStringLiteral\("media"\)/);
assert.match(api, /thumbnails\/set/);
assert.match(studio, /bool YouTubeStudioBridge::hasProfile\(\) const/);

console.log('XINKX broadcast source contract: one Save, explicit YouTube event/category and official thumbnail/video update');
