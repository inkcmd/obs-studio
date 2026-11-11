// Source-contract coverage; a real Windows/Qt and OAuth runtime test is still required.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const source = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const editor = source('BroadcastTagsEditor.hpp');
const ui = source('BroadcastWorkspace.cpp');
const api = source('ChatGateway.cpp');
const profiles = source('GameProfilesWorkspace.cpp');

// Twitch and Kick share the strict, selectable tags; YouTube has its own list.
assert.match(ui, /new BroadcastTagsEditor\(true, panel\)/);
assert.match(ui, /new BroadcastTagsEditor\(false, panel\)/);
assert.match(ui, /xinkxBroadcastTags/);
assert.match(ui, /xinkxBroadcastYouTubeTags/);
assert.match(ui, /local->contains\(key\(QStringLiteral\("youtubeTags"\)\)\)/);
assert.match(editor, /existing\.size\(\) < 10 && tag\.size\(\) <= 25/);
assert.match(editor, /QRegularExpression permitted/);
assert.match(editor, /return length <= 500/);
assert.match(editor, /values\.removeAll\(tag\)/);

// Both APIs use the documented writable property, including [] to clear.
assert.match(api, /body\.insert\(QStringLiteral\("tags"\), QJsonArray::fromStringList\(tags\)\)/);
assert.match(api, /body\.insert\(QStringLiteral\("custom_tags"\), QJsonArray::fromStringList\(tags\)\)/);
assert.match(api, /live\.value\(QStringLiteral\("custom_tags"\)\)/);
assert.match(api, /item\.value\(QStringLiteral\("tags"\)\)/);
assert.match(api, /bool tagsMatch\(const QJsonArray &remote, const QStringList &expected\)/);
assert.match(api, /Kick не вернул список тегов для проверки/);

// A YouTube tag change is a video update, never part of liveBroadcasts.update.
assert.match(api, /www\.googleapis\.com\/youtube\/v3\/videos\?part=snippet/);
assert.match(api, /if \(!verified \|\| !applyTags\)/);
assert.match(api, /if \(current == tags\)/);
assert.match(api, /snippet\.insert\(field, existing\.value\(field\)\)/);
assert.match(api, /if \(metadataUnchanged\)/);

// Tags are stored in each game profile and restored on switching profiles.
assert.match(profiles, /profile\.contains\(QStringLiteral\("tags"\)\)/);
assert.match(profiles, /profile\.contains\(QStringLiteral\("youtubeTags"\)\)/);
assert.match(profiles, /QJsonArray::fromStringList\(tags->tags\(\)\)/);
assert.match(profiles, /QJsonArray::fromStringList\(youtubeTags->tags\(\)\)/);

console.log('XINKX broadcast tags: native chips, profile persistence, Twitch/Kick/YouTube provider contracts OK');
