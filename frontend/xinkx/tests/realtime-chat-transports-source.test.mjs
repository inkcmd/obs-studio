import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const gateway = read('ChatGateway.cpp');
const stream = read('YouTubeGrpcStream.cpp');
const build = read('../cmake/feature-xinkx-studio.cmake');

// YouTube's documented server-streaming RPC replaces repeated REST chat polling.
for (const term of [
  'youtube.googleapis.com/youtube.api.v3.V3DataLiveChatMessageService/StreamList',
  'CURL_HTTP_VERSION_2_0',
  'application/grpc',
  'putString(proto, 1, chatId.toUtf8())',
  'putString(proto, 99, pageToken.toUtf8())',
  'putString(proto, 100, part)',
  'field == 100602', // continuation token
  'field == 1007', // chat items
  'field == 101', // YouTube's stable message IDs
  'field == 16', // message display text
]) {
  assert.ok(stream.includes(term), `missing YouTube streaming protocol field: ${term}`);
}
assert.match(stream, /readString\(content, 103, name\)/);
assert.match(build, /xinkx\/YouTubeGrpcStream\.cpp/);
assert.doesNotMatch(gateway, /pollYouTubeMessages\(/);
assert.doesNotMatch(gateway, /pollingIntervalMillis/);
assert.match(gateway, /startYouTubeStream\(id\)/);
assert.match(gateway, /state\.nextPage = page\.value\(QStringLiteral\("nextPageToken"\)\)/);
assert.match(gateway, /state\.forceTokenRefresh = true/);
assert.match(gateway, /youtubeStreams\.value\(id\) != \*identity/);

// Twitch EventSub reconnect retains its subscriptions by handing off the socket.
assert.match(gateway, /session_reconnect/);
assert.match(gateway, /reconnectTwitchSocket\(id, target\)/);
assert.match(gateway, /delete twitch\.take\(id\);\s*twitch\[id\] = replacement/);
assert.match(gateway, /EventSub перенесён без повторной подписки/);

// Kick stays on a Centrifugo WebSocket, with reconnect backoff and stale-socket guards.
assert.match(gateway, /realtime\/connection/);
assert.match(gateway, /chatrooms\.%1\.v2/);
assert.match(gateway, /kickSockets\.value\(id\) != \*identity/);
assert.match(gateway, /state\.attempts = std::min\(6, state\.attempts \+ 1\)/);

// One event ID reaches all presentation consumers only once, including on reconnect.
assert.match(gateway, /if \(seenEvents\.contains\(key\)\) return/);
assert.match(gateway, /line\.accountId = canonicalChatId\(line\.accountId\)/);

console.log('XINKX realtime chat: gRPC/HTTP2 YouTube, EventSub handoff Twitch, WSS Kick and dedup guards OK');
