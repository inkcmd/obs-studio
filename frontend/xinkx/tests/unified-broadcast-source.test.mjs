// Static contract checks for native UI wiring where Qt/MSVC is unavailable.
// These are source-level guards, NOT a native build or provider integration test.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const source = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8').replace(/\r\n/g, '\n');
const broadcast = source('BroadcastWorkspace.cpp');
const profiles = source('GameProfilesWorkspace.cpp');
const api = source('ChatGateway.cpp');

assert.match(broadcast, /setObjectName\(QStringLiteral\("xinkxBroadcastSave"\)\)/);
assert.equal((broadcast.match(/new QPushButton\(QStringLiteral\("Сохранить/g) || []).length, 1);
assert.doesNotMatch(profiles, /new QPushButton\(QStringLiteral\("Применить/);
assert.doesNotMatch(profiles, /new QPushButton\(QStringLiteral\("Сохранить/);
assert.match(profiles, /setProperty\("xinkxProfileReady", false\)/);
assert.match(broadcast, /new QCompleter\(model, panel\)/);
assert.match(broadcast, /Категория YouTube/);
assert.match(broadcast, /Эфир YouTube/);
assert.doesNotMatch(broadcast, /youtubeEvent->hide\(\)/);
assert.doesNotMatch(broadcast, /YouTubeStudioBridge|Открыть YouTube Studio/);
assert.doesNotMatch(broadcast, /xinkxBroadcastThumbnail|pendingThumbnail|pendingGame/);
assert.doesNotMatch(profiles, /xinkxBroadcastThumbnail|thumbnail->text\(\)/);
assert.match(api, /videoCategories.*\n.*regionCode/);
assert.doesNotMatch(api, /thumbnails\/set|youtubeMetadataUpdated|thumbnailUpdated/);
assert.match(api, /liveBroadcasts\?part=snippet/);
assert.match(api, /snippet\.insert\(QStringLiteral\("description"\), description\.left\(5000\)\)/);
assert.match(api, /snippet\.insert\(QStringLiteral\("title"\), heading\)/);
assert.match(api, /"contentDetails"\), QJsonObject\{\{QStringLiteral\("monitorStream"\), monitor\}\}/);
assert.match(api, /nextYoutubeQuotaReset/);
assert.match(api, /quotaExceeded/);
assert.doesNotMatch(broadcast, /if \(success\) gateway->inspectBroadcast/);
assert.match(broadcast, /youtubeEvent->setItemText\(index, saved\.second\)/);
assert.match(profiles, /profile\.insert\(QStringLiteral\("youtubeBroadcastName"\), youtubeEvent->currentText\(\)\)/);

console.log('XINKX broadcast source contract: one Save, YouTube metadata update and quota protection, no Studio/thumbnail');
