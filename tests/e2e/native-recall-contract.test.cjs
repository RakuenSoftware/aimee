const assert = require('node:assert/strict');
const test = require('node:test');
const { createScenario, promptFor, assertRecall } = require('./native-recall-contract.cjs');

const scenario = createScenario({
  label: 'BenignLabel0123456789abcdef0123',
  marker: 'CrossMarkerabcdef0123456789abcd',
});

test('the first three recall prompts retain their original difficulty', () => {
  assert.equal(promptFor(scenario, 'A1'), `Remember this short benign label for this conversation: ${scenario.label}. Reply only with that label. Do not use tools or ask questions.`);
  assert.equal(promptFor(scenario, 'A2'), 'What short label did I ask you to remember? Reply only with that label. Use existing conversation; do not use tools or ask questions.');
  assert.equal(promptFor(scenario, 'B'), `Recall the short label I asked you to remember. Reply with that label and this marker: ${scenario.marker}. Do not use tools or ask questions.`);
});

test('the return asks for user-provided facts without leaking either answer', () => {
  const prompt = promptFor(scenario, 'A-return');
  assert.match(prompt, /I supplied in my earlier user messages/);
  assert.match(prompt, /user-provided values/);
  assert(!prompt.includes('assistant response'));
  for (const value of Object.values(scenario)) assert(!prompt.includes(value));
  assert(!promptFor(scenario, 'A2').includes(scenario.label));
  assert(!promptFor(scenario, 'B').includes(scenario.label));
});

test('independent 80-bit values cannot be shortened or shared', () => {
  assert.throws(() => createScenario({ ...scenario, label: 'BenignLabel0123' }));
  assert.throws(() => createScenario({ ...scenario, marker: `CrossMarker${scenario.label.slice(11)}` }));
  assert.throws(() => promptFor(scenario, 'fallback'));
});

test('the oracle requires both exact values on return, not substrings or guesses', () => {
  const answer = `${scenario.label}\n${scenario.marker}`;
  assertRecall(scenario, 'A-return', answer);
  assertRecall(scenario, 'B', `**${scenario.label}**, \`${scenario.marker}\``);
  assertRecall(scenario, 'A2', scenario.label);
  for (const wrong of [
    scenario.label,
    scenario.marker,
    `${scenario.label}\n${scenario.marker.slice(0, -1)}`,
    `${scenario.label}0\n${scenario.marker}`,
    `prefix${scenario.label}\n${scenario.marker}`,
    `${answer}\n${scenario.label}`,
    `${answer}\nCrossMarker11111111111111111111`,
    `The answer is ${answer}`,
    '',
  ]) assert.throws(() => assertRecall(scenario, 'A-return', wrong));
});

const references = createScenario({
  mode: 'user-reference-uuid',
  projectReferenceID: '9435d83f-1f18-42af-afb1-32e638a092fd',
  releaseReferenceID: '3af92b2e-a8d5-42b9-9d72-c38648d83e15',
});

test('legacy mode stays selectable and remains the default', () => {
  assert.equal(scenario.mode, 'legacy-label-marker');
  const explicit = createScenario({ ...scenario, mode: 'legacy-label-marker' });
  for (const phase of ['A1', 'A2', 'B', 'A-return'])
    assert.equal(promptFor(explicit, phase), promptFor(scenario, phase));
  assert.throws(() => createScenario({ ...scenario, mode: 'fallback' }));
});

test('user-reference UUID prompts preserve the four-turn factual information flow', () => {
  assert.equal(promptFor(references, 'A1'), `The user-provided project reference ID for this conversation is ${references.projectReferenceID}. Remember it and reply only with that ID. Do not use tools or ask questions.`);
  assert.equal(promptFor(references, 'A2'), 'What project reference ID did I provide earlier? Reply only with that ID. Use the existing conversation; do not use tools or ask questions.');
  assert.equal(promptFor(references, 'B'), `I am providing a release reference ID: ${references.releaseReferenceID}. Reply only with my earlier project reference ID and this release reference ID. Do not use tools or ask questions.`);
  assert.equal(promptFor(references, 'A-return'), 'Recall the project reference ID and release reference ID that I supplied in my earlier user messages. Reply only with those two user-provided IDs. Use the existing conversation; do not use tools or ask questions.');
  for (const phase of ['A2', 'B', 'A-return'])
    assert(!promptFor(references, phase).includes(references.projectReferenceID));
  for (const phase of ['A1', 'A2', 'A-return'])
    assert(!promptFor(references, phase).includes(references.releaseReferenceID));
  assert.throws(() => promptFor(references, 'retry'));
});

test('reference IDs must be distinct full lowercase UUIDv4 values', () => {
  for (const projectReferenceID of [
    references.projectReferenceID.slice(0, -1),
    references.projectReferenceID.toUpperCase(),
    references.projectReferenceID.replace('-42af-', '-12af-'),
    references.projectReferenceID.replace('-afb1-', '-7fb1-'),
    '9435d83f',
  ]) assert.throws(() => createScenario({ ...references, projectReferenceID }));
  assert.throws(() => createScenario({ ...references, releaseReferenceID: references.projectReferenceID }));
});

test('UUID oracle requires each exact user value once, with no extra content', () => {
  const first = references.projectReferenceID, second = references.releaseReferenceID;
  assertRecall(references, 'A1', first);
  assertRecall(references, 'A2', `\`${first}\``);
  assertRecall(references, 'B', `${first}\n${second}`);
  assertRecall(references, 'A-return', `**${second}**, \`${first}\``);
  for (const wrong of [
    first,
    second,
    `${first}\n${second.slice(0, -1)}`,
    `${first}0\n${second}`,
    `prefix${first}\n${second}`,
    `${first}\n${second}\n${first}`,
    `${first}\n${second}\na9d8f253-1916-424e-b112-c9828d08c527`,
    `${first}\n${second.toUpperCase()}`,
    `Project reference ID: ${first}\nRelease reference ID: ${second}`,
    `${first}\n${second}\nThe IDs are remembered.`,
    `${first.slice(0, 8)}\n${second.slice(0, 8)}`,
    '',
  ]) assert.throws(() => assertRecall(references, 'A-return', wrong));
  assert.throws(() => assertRecall(references, 'A2', `${first}\n${second}`));
  assert.throws(() => assertRecall(references, 'A-return', `${first}\n3af92b2e-a8d5-42b9-9d72-c38648d83e16`));
});
