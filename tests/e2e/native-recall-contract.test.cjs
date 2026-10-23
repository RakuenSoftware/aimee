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
