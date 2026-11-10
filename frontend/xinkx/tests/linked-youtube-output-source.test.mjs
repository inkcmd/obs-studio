// Static contract checks for one YouTube identity with multiple RTMP outputs.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const source = name => readFileSync(new URL(`../${name}`, import.meta.url), 'utf8');
const model = source('StreamAccounts.hpp');
const store = source('StreamAccounts.cpp');
const settings = source('MultiStreamWorkspace.cpp');
const gateway = source('ChatGateway.cpp');

assert.match(model, /QString authorizationSourceId;/);
assert.match(store, /account\.authorizationSourceId = settings\.value/);
assert.match(store, /account\.oauth = source->oauth/);
assert.match(store, /alias \? QString\{\} : account\.oauth\.accessToken/);
assert.match(settings, /xinkxSharedAuthorization/);
assert.match(settings, /authorization->currentData\(\)\.toString\(\)/);
assert.match(settings, /account\.authorizationSourceId = linked/);
assert.match(settings, /authorizationSourceId == id/);
assert.match(settings, /entry\.id == id \|\| entry\.authorizationSourceId == id/);
assert.match(gateway, /credentialOwner = account\.authorizationSourceId/);
assert.match(gateway, /it\.value\(\)\.authorizationSourceId == credentialOwner/);

console.log('XINKX linked YouTube outputs: shared OAuth identity, independent destination and refresh-token owner OK');
