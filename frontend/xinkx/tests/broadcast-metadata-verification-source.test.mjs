// Static regression guards. OAuth/API behavior must be checked in a real Windows build.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const api = read('ChatGateway.cpp');
const ui = read('BroadcastWorkspace.cpp');
const profiles = read('GameProfilesWorkspace.cpp');

// YouTube unscheduled Studio broadcasts cannot be gated on scheduledStartTime.
assert.match(api, /updateYouTubeVideoDetails\(id, broadcastId, heading, description, category, tags, applyTags\)/);
assert.match(api, /const QString wantedTitle = heading\.isEmpty\(\)/);
assert.match(api, /const QString wantedCategory = category\.isEmpty\(\)/);
assert.match(api, /const QStringList wantedTags = applyTags \? tags : current/);
assert.match(api, /if \(matches\(existing\)\)/);
assert.match(api, /QJsonArray::fromStringList\(wantedTags\)/);
assert.match(api, /authorized\(id, videoUrl, \{\}, \[this, id, matches\]/);
assert.doesNotMatch(api, /scheduledStartTime\"\)\.toString\(\)\.isEmpty\(\)/);
// Twitch GET /helix/channels must confirm tags following PATCH 204.
assert.match(api, /if \(!remote\.isArray\(\) \|\| !tagsMatch\(remote\.toArray\(\), tags\)\)/);
assert.match(api, /Twitch: эфир отправлен, но теги канала не совпадают с профилем/);
// Only a selected YouTube broadcast may be updated on profile Save.
assert.match(ui, /eventId = youtubeEvent->currentData\(\)\.toString\(\)/);
assert.match(profiles, /xinkxProfileBroadcastSelection/);
assert.match(ui, /const QString previously = profileSelection \? pending/);
assert.doesNotMatch(ui, /else if \(activeIndex > 0\)/);
assert.match(ui, /Автоматически другой эфир не изменяем/);
console.log('XINKX broadcast metadata: unscheduled YouTube, Twitch readback, explicit event selection OK');
