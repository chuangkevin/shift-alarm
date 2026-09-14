const assert = require('node:assert/strict');
const fs = require('node:fs');

const source = fs.readFileSync('firmware-next/main/main.cpp', 'utf8');
const policies = [...source.matchAll(/\/\* OTA_STATUS_POLICY_START \*\/(.*?)\/\* OTA_STATUS_POLICY_END \*\//gs)];
const policy = policies.at(-1);
assert.ok(policy, 'embedded staged OTA browser status policy is missing');
assert.match(source, /mutationPending/);
assert.match(source, /不會自動重送/);
assert.match(source, /marker-fault/);
assert.match(source, /!d\.staged&&!d\.markerFault/);
assert.match(source, /otaResetTerminalNoStaged\(\)/);
assert.equal((source.match(/async function mutate\(/g) || []).length, 1);

let acceptedRequest = 0;
let currentSession = null;
let newest = 0;
eval(policy[1]);

recordUpdateFailure(3);
assert.equal(acceptedRequest, 3);
assert.equal(acceptUpdateStatus({session: 'boot-a', sequence: 1}, 2), false);
acceptedRequest = 0;
assert.equal(acceptUpdateStatus({session: 'boot-a', sequence: 7}, 2), true);
assert.equal(acceptUpdateStatus({session: 'boot-a', sequence: 8}, 1), false);
assert.equal(acceptUpdateStatus({session: 'boot-a', sequence: 6}, 3), false);
assert.equal(acceptUpdateStatus({session: 'boot-a', sequence: 8}, 4), true);
assert.equal(acceptUpdateStatus({session: 'boot-b', sequence: 0}, 5), true);
assert.equal(newest, 0);
assert.equal(currentSession, 'boot-b');
assert.equal(acceptUpdateStatus({session: 'boot-a', sequence: 9}, 4), false);
assert.equal(acceptUpdateStatus({session: 'boot-b', sequence: 1}, 6), true);

console.log('OTA browser session reset and in-boot ordering checks passed');
