const assert = require('node:assert/strict');
const test = require('node:test');
const {validateAddedTurns} = require('./native-recall-history.cjs');

const turn = 'a'.repeat(32), proofTurn = 'b'.repeat(32);
const session = 'fixture-session', actor = 'fixture-actor';
function fixture() {
  const envelope = (id, owner, kind, payload) => ({
    session_id: session, actor, turn_id: owner, event_id: id,
    event: {kind, payload}
  });
  // The replay owner deliberately uses its own random turn. These inert hashes
  // represent the source-owned proof shape, not model text or signing material.
  return [
    envelope('running', turn, 'execution', {status: 'running'}),
    envelope(`${proofTurn}:proof:1`, proofTurn, 'replay_proof', {
      scope: '1'.repeat(64), block_digest: '2'.repeat(64), wire: 'anthropic',
      thinking_binding: `ap1:${'3'.repeat(64)}:${'4'.repeat(64)}:${'5'.repeat(64)}`
    }),
    envelope('projection', turn, 'projection', {
      model: 'fixture-model', boundary: 'response',
      omitted_reasoning_blocks: 1, omitted_opaque_state_blocks: 1
    }),
    envelope('completed', turn, 'execution', {status: 'completed'})
  ];
}
const check = rows => validateAddedTurns(rows, turn, session, actor);

test('owned typed replay proof can use its separate proof turn', () => {
  check(fixture());
  const rows = fixture();
  rows[1].event.payload.wire = 'responses';
  delete rows[1].event.payload.thinking_binding;
  check(rows);
});

for (const kind of ['message', 'delta', 'execution', 'control_request', 'control_response', 'projection', 'tool_use']) {
  test(`a foreign ${kind} cannot acquire the replay-proof exception`, () => {
    const rows = fixture();
    rows[1].event = {kind, payload: {}};
    assert.throws(() => check(rows), /Foreign execution event/);
  });
}

for (const [name, mutate] of Object.entries({
  actor: row => row.actor = 'other-actor',
  session: row => row.session_id = 'other-session',
  scope: row => row.event.payload.scope = 'bad',
  digest: row => row.event.payload.block_digest = 'bad',
  wire: row => row.event.payload.wire = 'unknown',
  missingBinding: row => delete row.event.payload.thinking_binding,
  malformedBinding: row => row.event.payload.thinking_binding = 'bad',
  payloadAction: row => row.event.payload.action = 'allow',
  eventAction: row => row.event.action = 'allow',
  foreignEventID: row => row.event_id = `${turn}:proof:1`,
  zeroCounter: row => row.event_id = `${proofTurn}:proof:0`,
  malformedTurn: row => row.turn_id = 'bad'
})) {
  test(`malformed replay proof is rejected: ${name}`, () => {
    const rows = fixture();
    mutate(rows[1]);
    assert.throws(() => check(rows));
  });
}

test('Responses proof cannot carry an Anthropic binding', () => {
  const rows = fixture();
  rows[1].event.payload.wire = 'responses';
  assert.throws(() => check(rows));
});

test('existing primary-history message/commit grouping stays supported', () => {
  const rows = fixture();
  rows.push(
    {session_id: session, actor, turn_id: 'c'.repeat(32), event: {kind: 'history', payload: {scope: 'primary', phase: 'message', index: 0}}},
    {session_id: session, actor, turn_id: 'c'.repeat(32), event: {kind: 'history', payload: {scope: 'primary', phase: 'commit', count: 1}}}
  );
  check(rows);
  rows.at(-1).event.payload.count = 2;
  assert.throws(() => check(rows));
});
