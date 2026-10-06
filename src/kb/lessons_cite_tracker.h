/* lessons_cite_tracker.h: the auto-`useful` proxy for the retrieval-learning loop
 * (graph-feedback proposal §3). A source node counts as "useful" when the agent
 * cites it AGAIN within N turns of first surfacing it — a concrete, non-circular
 * signal ("it came back to this source"), not "the agent acted on it".
 *
 * Pure + in-memory: a per-session ring of recently-cited node ids and the last
 * turn each was seen. Deterministic and unit-testable — the KB_STORE write and the
 * session-aware wiring live elsewhere (kb_store/lessons.c, the capture call-site). */
#ifndef LESSONS_CITE_TRACKER_H
#define LESSONS_CITE_TRACKER_H

/* Node-key buffer, sized like the graph node keys the tracker observes. */
#define LESSONS_NODE_MAX 512
/* Recent distinct nodes tracked per session (LRU by last-seen turn; oldest is
 * evicted when full). Bounds the per-session memory. */
#define LESSONS_TRACKER_CAP 256
/* Default cite-again window (AIMEE_TRUST_AUTO_USEFUL_TURNS). */
#define LESSONS_AUTO_USEFUL_TURNS 3

typedef struct
{
   char node[LESSONS_NODE_MAX];
   int last_turn; /* most recent turn this node was cited */
} lessons_cite_entry_t;

typedef struct
{
   lessons_cite_entry_t entries[LESSONS_TRACKER_CAP];
   int count;
   long auto_useful_count; /* running count of cite-again (auto-useful) triggers */
} lessons_cite_tracker_t;

/* Reset a tracker to empty (equivalently: memset 0). */
void lessons_cite_tracker_init(lessons_cite_tracker_t *t);

/* Observe that `node` was cited at `turn` (turns are monotonically increasing per
 * session). Returns 1 if this is an auto-`useful` trigger — the node was cited
 * before AND within `within_turns` prior turns (0 < turn - last_turn <=
 * within_turns) — else 0. Always updates the node's last-seen turn (inserting it,
 * evicting the least-recently-seen entry when full). Increments
 * t->auto_useful_count on a trigger. A NULL/empty node or NULL tracker returns 0. */
int lessons_cite_observe(lessons_cite_tracker_t *t, const char *node, int turn, int within_turns);

#endif /* LESSONS_CITE_TRACKER_H */
