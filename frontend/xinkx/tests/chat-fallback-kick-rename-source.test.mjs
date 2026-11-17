import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {test} from 'node:test';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const chat = read('ChatGateway.cpp');
const header = read('ChatGateway.hpp');
const grpc = read('YouTubeGrpcStream.cpp');
const ui = read('ChatWorkspace.cpp');

test('YouTube prefers streaming but switches to REST only when HTTP/2 is unavailable', () => {
  assert.match(grpc, /curl_version_info\(CURLVERSION_NOW\)->features & CURL_VERSION_HTTP2/);
  assert.match(grpc, /grpcStatus = -2/);
  assert.match(chat, /if \(grpcStatus == -2\) \{/);
  assert.match(chat, /state\.restFallback = true/);
  assert.match(header, /bool restFallback = false/);
  assert.match(chat, /if \(state.restFallback\) pollYouTubeMessages\(id\)/);
});

test('REST fallback observes YouTube polling interval, pagination, baseline and quotas', () => {
  assert.match(chat, /https:\/\/www\.googleapis\.com\/youtube\/v3\/liveChat\/messages/);
  for (const prop of ['pollingIntervalMillis', 'nextPageToken', 'pageToken', 'maxResults', 'offlineAt'])
    assert.ok(chat.includes(`QStringLiteral("${prop}")`), `missing ${prop}`);
  assert.match(chat, /std::max\(serverInterval, qint64\(10000\)\)/);
  assert.match(chat, /state\.baseline\) handleYouTubePage\(id, result\.json\)/);
  assert.match(chat, /std::max\(youtubeQuotaResumeAt, now \+/);
  assert.match(chat, /if \(state.restFallback\) \{/);
});

test('Kick follows stable OAuth account identity across display-name and slug changes', () => {
  assert.match(chat, /updated\[it\.key\(\)\]\.providerId != known\.value\(it\.key\(\)\)\.providerId/);
  assert.match(chat, /it->slug = newSlug/);
  assert.match(chat, /chat\/kickRoomForUser-%1/);
  assert.match(chat, /chat\/kickRoomOwner-%1/);
  assert.match(chat, /broadcaster_user_id/);
  assert.match(chat, /state\.slug = slug\.toLower\(\)\.trimmed\(\)/);
  assert.match(chat, /kick\[id\]\.slug != targetSlug/);
  assert.match(chat, /kick\[id\]\.lookupVersion != lookupVersion/);
  assert.match(chat, /https:\/\/kick\.com\/api\/v2\/channels\/%1/);
});

test('Kick private API block has a manual chatroom ID UI (not broadcaster user ID)', () => {
  assert.match(ui, /Kick ID/);
  assert.match(ui, /QInputDialog::getText/);
  assert.match(ui, /gateway->setKickRoomId\(id, room\)/);
  assert.match(ui, /gateway->retryDisconnectedChats\(\)/);
  assert.match(ui, /не broadcaster_user_id/);
});
