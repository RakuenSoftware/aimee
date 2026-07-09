import { describe, it, expect } from 'vitest';
import { TAB_TUTORIALS, tutorialFor } from './tutorials';
import { parseSeen, withSeen, SEEN_KEY } from './tutorialState';
import { NAV_ITEMS } from '../nav';

/* The routes App exposes, read straight from the nav registry (not a hand-copied
 * list) so the "every nav route has a tutorial" test below actually guards drift:
 * a new tab added to NAV_ITEMS without a tutorial entry fails here. */
const NAV_ROUTES = NAV_ITEMS.map(it => it.route);

describe('TAB_TUTORIALS content contract', () => {
  it('has a tutorial for every NAV_ITEMS route', () => {
    for (const route of NAV_ROUTES) {
      expect(TAB_TUTORIALS, `missing tutorial for ${route}`).toHaveProperty(route);
    }
  });

  it('every tutorial has a non-empty title and 1–5 body lines', () => {
    for (const [route, t] of Object.entries(TAB_TUTORIALS)) {
      expect(t.title.trim().length, `${route} title`).toBeGreaterThan(0);
      expect(t.body.length, `${route} body count`).toBeGreaterThan(0);
      expect(t.body.length, `${route} body count`).toBeLessThanOrEqual(5);
      for (const line of t.body) expect(line.trim().length, `${route} body line`).toBeGreaterThan(0);
    }
  });

  it('every seeAlso points at a real tutorial route', () => {
    for (const [route, t] of Object.entries(TAB_TUTORIALS)) {
      if (t.seeAlso) {
        expect(TAB_TUTORIALS, `${route} seeAlso -> ${t.seeAlso}`).toHaveProperty(t.seeAlso);
      }
    }
  });

  it('tutorialFor returns undefined for an unknown route', () => {
    expect(tutorialFor('/nope')).toBeUndefined();
    expect(tutorialFor('/chat')).toBe(TAB_TUTORIALS['/chat']);
  });
});

describe('tutorialState seen-list logic (pure)', () => {
  it('parseSeen tolerates null, malformed, and non-string junk', () => {
    expect(parseSeen(null)).toEqual([]);
    expect(parseSeen('not json')).toEqual([]);
    expect(parseSeen('{"a":1}')).toEqual([]); // object, not array
    expect(parseSeen('[1,"/chat",true,"/logs"]')).toEqual(['/chat', '/logs']);
  });

  it('withSeen adds idempotently and preserves order', () => {
    let list: string[] = [];
    list = withSeen(list, '/chat');
    list = withSeen(list, '/logs');
    list = withSeen(list, '/chat'); // dup
    expect(list).toEqual(['/chat', '/logs']);
  });

  it('a seen round-trip through parse/serialize is stable', () => {
    const list = withSeen(withSeen([], '/chat'), '/settings');
    const roundTripped = parseSeen(JSON.stringify(list));
    expect(roundTripped).toEqual(list);
    expect(SEEN_KEY).toBe('aimee_tutorial_seen');
  });
});
