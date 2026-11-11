import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const gateway = read('ChatGateway.cpp');
const chat = read('ChatWorkspace.cpp');
const broadcast = read('BroadcastWorkspace.cpp');
const streams = read('MultiStreamManager.cpp');
const grpc = read('YouTubeGrpcStream.cpp');

// A manual refresh must retry offline connections without discarding a
// working Twitch/Kick socket or bypassing Google's real daily quota pause.
assert.match(chat, /gateway->retryDisconnectedChats\(\)/);
assert.match(gateway, /void ChatGateway::retryDisconnectedChats\(\)/);
assert.match(gateway, /!twitch\.contains\(account\.id\)/);
assert.match(gateway, /!youtubeStreams\.contains\(account\.id\)/);
assert.match(gateway, /!kickSockets\.contains\(account\.id\)/);
assert.match(gateway, /now < youtubeQuotaResumeAt/);

// A transport outage is not an OAuth revocation. Retry with a bounded delay.
assert.match(gateway, /result\.status == 0 \|\| result\.status >= 500 \|\| result\.status == 429/);
assert.match(gateway, /twitchRetryAttempts/);
assert.match(gateway, /channel:read:subscriptions/);
assert.match(gateway, /moderator:read:followers/);
assert.match(chat, /view->warnings\[id\]\.append\(message\)/);

// YouTube's healthy ingest and the chosen live broadcast are different things.
// Verify remote binding and key equality, but never echo the stream secret.
assert.match(gateway, /part"\), QStringLiteral\("id,status,cdn"\)/);
assert.match(gateway, /ingestStatus/);
assert.match(gateway, /ingestKeyMatch/);
assert.match(gateway, /configuredStreamMatch/);
assert.match(broadcast, /xinkxYouTubeBroadcastHealth/);
assert.match(broadcast, /gateway->inspectBroadcast\(account\.id, broadcastId\)/);
assert.doesNotMatch(broadcast, /ingestionInfo/);

// Around an OBS hang, diagnostics identify the synchronous libobs call.
assert.match(streams, /Starting output %1, serial=%2/);
assert.match(streams, /Restarting output %1, serial=%2/);
assert.match(streams, /Stop requested %1, serial=%2/);

// A quiet long-lived gRPC stream must still be cancellable. The old
// curl_easy_perform + unbounded QThread::wait froze the OBS GUI on teardown.
assert.doesNotMatch(grpc, /curl_easy_perform\(curl\)/);
assert.match(grpc, /curl_multi_poll\(multi, nullptr, 0, 250, &socketsReady\)/);
assert.match(grpc, /!shared->stopping\.load\(\)/);
assert.match(grpc, /worker->wait\(\)/);

console.log('XINKX offline recovery, alert permission warnings and YouTube ingest diagnostics OK');
