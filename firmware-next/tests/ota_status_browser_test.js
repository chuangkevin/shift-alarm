const assert = require('node:assert/strict');
const fs = require('node:fs');

const source = fs.readFileSync('firmware-next/main/main.cpp', 'utf8');
const policy = source.match(/\/\* OTA_STATUS_POLICY_START \*\/(.*?)\/\* OTA_STATUS_POLICY_END \*\//s);
assert.ok(policy, 'embedded OTA browser status policy is missing');

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
