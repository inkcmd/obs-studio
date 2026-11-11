import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const overlay = read('OverlayRuntime.cpp');
const overlayHeader = read('OverlayRuntime.hpp');
const gateway = read('ChatGateway.cpp');
const gatewayHeader = read('ChatGateway.hpp');

// Seeing a chat message does not imply OBS can display follow/sub alerts.
// Inspect the active scene (including grouped and hidden sources) and expose
// a one-click way to add the real native alerts source.
assert.match(overlayHeader, /QString alertSourceHealth\(OBSBasic \*mainWindow\) const/);
assert.match(overlay, /obs_scene_enum_items\(scene, inspectAlertSource, &presence\)/);
assert.match(overlay, /obs_sceneitem_is_group\(item\)/);
assert.match(overlay, /obs_sceneitem_visible\(item\)/);
assert.match(overlay, /std::strcmp\(id, "xinkx_alerts"\)/);
assert.match(overlay, /xinkxAlertReadiness/);
assert.match(overlay, /xinkxAttachAlerts/);
assert.match(overlay, /runtime->addToScene\(mainWindow, &message, QStringLiteral\("alerts"\)\)/);

// Provider EventSub readiness is independent of the actual on-scene display.
assert.match(gatewayHeader, /void alertTransportStatus\(/);
assert.match(gateway, /emit alertTransportStatus\(id, type, result\.ok/);
assert.match(gateway, /channel:read:subscriptions/);
assert.match(gateway, /moderator:read:followers/);
assert.match(gateway, /EventSub отключён/);
assert.match(overlay, /xinkxAlertTransportHealth/);

// Distinguish genuine provider events from local tests and warn once when
// alerts cannot be shown. Never treat Kick chat as a Kick follow webhook.
assert.match(overlay, /gateway, &ChatGateway::incoming/);
assert.match(overlay, /xinkxLastRealAlert/);
assert.match(gateway, /alert:received/);
assert.match(gateway, /alertDeliveryWarnings/);
assert.match(gateway, /overlay->publishAlert\(line\.type, line\.platform/);
assert.match(overlay, /Kick: фолловеры\/подписки требуют официального webhook/);

console.log('XINKX alert scene readiness, Twitch EventSub status, and actual delivery diagnostics OK');
