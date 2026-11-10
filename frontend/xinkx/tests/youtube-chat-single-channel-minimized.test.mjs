import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const gateway = read('ChatGateway.cpp');
const workspace = read('ChatWorkspace.cpp');
const overlay = read('PresentationWorkspace.cpp');

for (const term of [
  'QString ChatGateway::canonicalChatId(',
  'account.authorizationSourceId.isEmpty() ? accountId : account.authorizationSourceId',
  'QVector<StreamAccount> ChatGateway::chatAccounts()',
  'canonicalChatId(account.id) != account.id',
  'line.accountId = canonicalChatId(line.accountId)',
  'const QString id = canonicalChatId(requestedId)',
  'if (sentTo.contains(id)) continue',
  'videoIds.join(QLatin1Char',
  'report(countedVideos == expected ? total : -1)',
]) assert.ok(gateway.includes(term), `missing channel-scoped chat contract: ${term}`);

assert.match(workspace, /gateway->chatAccounts\(\)/);
assert.match(overlay, /auto \*streamer = new StreamerOverlay\(\)/);
assert.match(overlay, /QObject::connect\(mainWindow, &QObject::destroyed, \[guardedStreamer\]/);
assert.match(overlay, /count < 0 \? QStringLiteral\("—"\)/);
assert.doesNotMatch(overlay, /new StreamerOverlay\(mainWindow\)/);

console.log('XINKX single YouTube chat identity, multi-broadcast viewer totals and minimized overlay lifetime OK');
