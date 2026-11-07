import { describe, expect, it } from 'vitest';
import { mergeChannelMessages } from './channelMessages';

describe('channel delivery', () => {
  it('merges a late snapshot, overlapping live delivery and reconnect without losing distinct identical messages', () => {
    const first = { id: '9007199254740992', sender: 'user', message: { role: 'user', blocks: [{ type: 'text', text: 'same' }] } };
    const second = { ...first, id: '9007199254740993' };
    const live = mergeChannelMessages([], [second]);
    const caughtUp = mergeChannelMessages([first, second], live);
    expect(mergeChannelMessages(caughtUp, [second])).toEqual([first, second]);
  });
});
