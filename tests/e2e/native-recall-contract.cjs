const assert = require('node:assert/strict');
const crypto = require('node:crypto');

function createScenario({ label, marker }) {
  assert.match(label, /^BenignLabel[0-9a-f]{20}$/);
  assert.match(marker, /^CrossMarker[0-9a-f]{20}$/);
  assert.notEqual(label.slice('BenignLabel'.length), marker.slice('CrossMarker'.length));
  return Object.freeze({ label, marker });
}

function promptFor(scenario, phase) {
  const { label, marker } = createScenario(scenario);
  const prompts = {
    A1: `Remember this short benign label for this conversation: ${label}. Reply only with that label. Do not use tools or ask questions.`,
    A2: 'What short label did I ask you to remember? Reply only with that label. Use existing conversation; do not use tools or ask questions.',
    B: `Recall the short label I asked you to remember. Reply with that label and this marker: ${marker}. Do not use tools or ask questions.`,
    // Both facts originate in user messages. The marker is supplied in B's
    // user prompt, not introduced by the assistant's subsequent response.
    'A-return': 'Recall the original label and the marker I supplied in my earlier user messages. Reply only with those two user-provided values. Use the existing conversation; do not use tools or ask questions.',
  };
  assert(Object.hasOwn(prompts, phase), 'Unknown recall phase');
  const prompt = prompts[phase];
  if (phase !== 'A1') assert(!prompt.includes(label), 'Repeated the label answer');
  if (phase !== 'B') assert(!prompt.includes(marker), 'Repeated the marker answer');
  return prompt;
}

function assertRecall(scenario, phase, text) {
  const { label, marker } = createScenario(scenario);
  assert(['A1', 'A2', 'B', 'A-return'].includes(phase), 'Unknown recall phase');
  assert.equal(typeof text, 'string');
  const expected = phase === 'B' || phase === 'A-return' ? [label, marker] : [label];
  const values = text.match(/(?<![A-Za-z0-9_])(?:BenignLabel|CrossMarker)[0-9a-f]+(?![A-Za-z0-9_])/g) || [];
  assert.deepEqual([...values].sort(), [...expected].sort(), 'Actual assistant did not return the exact recall values once each');
  let remainder = text;
  for (const value of expected) remainder = remainder.replace(value, '');
  assert.match(remainder, /^[\s`*|,;:.\-]*$/, 'Expected only the requested values and formatting');
  return { phase, values, canonical_text_sha256: crypto.createHash('sha256').update(text).digest('hex') };
}

module.exports = { createScenario, promptFor, assertRecall };
