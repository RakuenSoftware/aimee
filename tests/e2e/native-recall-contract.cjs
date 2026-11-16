const assert = require('node:assert/strict');
const crypto = require('node:crypto');

function createScenario({ mode = 'legacy-label-marker', label, marker, projectReferenceID, releaseReferenceID }) {
  assert(['legacy-label-marker', 'user-reference-uuid'].includes(mode), 'Unknown recall scenario mode');
  if (mode === 'user-reference-uuid') {
    const uuidV4 = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
    assert.match(projectReferenceID, uuidV4);
    assert.match(releaseReferenceID, uuidV4);
    assert.notEqual(projectReferenceID, releaseReferenceID);
    return Object.freeze({ mode, projectReferenceID, releaseReferenceID });
  }
  assert.match(label, /^BenignLabel[0-9a-f]{20}$/);
  assert.match(marker, /^CrossMarker[0-9a-f]{20}$/);
  assert.notEqual(label.slice('BenignLabel'.length), marker.slice('CrossMarker'.length));
  return Object.freeze({ mode, label, marker });
}

function promptFor(scenario, phase) {
  const current = createScenario(scenario);
  const { label, marker, projectReferenceID, releaseReferenceID } = current;
  const prompts = current.mode === 'user-reference-uuid' ? {
    A1: `The user-provided project reference ID for this conversation is ${projectReferenceID}. Remember it and reply only with that ID. Do not use tools or ask questions.`,
    A2: 'What project reference ID did I provide earlier? Reply only with that ID. Use the existing conversation; do not use tools or ask questions.',
    B: `I am providing a release reference ID: ${releaseReferenceID}. Reply only with my earlier project reference ID and this release reference ID. Do not use tools or ask questions.`,
    'A-return': 'Recall the project reference ID and release reference ID that I supplied in my earlier user messages. Reply only with those two user-provided IDs. Use the existing conversation; do not use tools or ask questions.',
  } : {
    A1: `Remember this short benign label for this conversation: ${label}. Reply only with that label. Do not use tools or ask questions.`,
    A2: 'What short label did I ask you to remember? Reply only with that label. Use existing conversation; do not use tools or ask questions.',
    B: `Recall the short label I asked you to remember. Reply with that label and this marker: ${marker}. Do not use tools or ask questions.`,
    // Both facts originate in user messages. The marker is supplied in B's
    // user prompt, not introduced by the assistant's subsequent response.
    'A-return': 'Recall the original label and the marker I supplied in my earlier user messages. Reply only with those two user-provided values. Use the existing conversation; do not use tools or ask questions.',
  };
  assert(Object.hasOwn(prompts, phase), 'Unknown recall phase');
  const prompt = prompts[phase];
  const first = current.mode === 'user-reference-uuid' ? projectReferenceID : label;
  const second = current.mode === 'user-reference-uuid' ? releaseReferenceID : marker;
  if (phase !== 'A1') assert(!prompt.includes(first), 'Repeated the first answer');
  if (phase !== 'B') assert(!prompt.includes(second), 'Repeated the second answer');
  return prompt;
}

function assertRecall(scenario, phase, text) {
  const current = createScenario(scenario);
  const first = current.mode === 'user-reference-uuid' ? current.projectReferenceID : current.label;
  const second = current.mode === 'user-reference-uuid' ? current.releaseReferenceID : current.marker;
  assert(['A1', 'A2', 'B', 'A-return'].includes(phase), 'Unknown recall phase');
  assert.equal(typeof text, 'string');
  const expected = phase === 'B' || phase === 'A-return' ? [first, second] : [first];
  const pattern = current.mode === 'user-reference-uuid'
    ? /(?<![A-Za-z0-9_-])[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}(?![A-Za-z0-9_-])/g
    : /(?<![A-Za-z0-9_])(?:BenignLabel|CrossMarker)[0-9a-f]+(?![A-Za-z0-9_])/g;
  const values = text.match(pattern) || [];
  assert.deepEqual([...values].sort(), [...expected].sort(), 'Actual assistant did not return the exact recall values once each');
  let remainder = text;
  for (const value of expected) remainder = remainder.replace(value, '');
  assert.match(remainder, /^[\s`*|,;:.\-]*$/, 'Expected only the requested values and formatting');
  return { phase, values, canonical_text_sha256: crypto.createHash('sha256').update(text).digest('hex') };
}

module.exports = { createScenario, promptFor, assertRecall };
