/* session_worktree_key.h: THE derivation of a session's worktree/branch key.
 *
 * One session id must map to exactly one worktree directory and exactly one
 * branch, and two different session ids must never map to the same one. This is
 * the single implementation of that mapping — the thin client (which links no
 * workspace.o) and aimee-server both call it, so a worktree created by one is
 * found by the other.
 *
 * History, because the shape of this function is load-bearing:
 *
 * It used to be two separate implementations that disagreed. The server took
 * the first 16 sanitized characters of the session id; the client hashed the
 * whole id. Both were wrong in their own way:
 *
 *   - Truncation COLLIDES. Ids minted on a shared prefix ("aimee-task-…" eats
 *     11 of the 16 characters) collapsed onto one key, which means one worktree
 *     and one branch — so concurrent writers silently overwrote each other.
 *   - Disagreement DUPLICATES. hooks_ensure_cwd_worktree creates its expected
 *     worktree when it does not find one, so a session the client had already
 *     placed could be handed a second worktree on a second branch.
 *
 * So the key hashes the FULL id (64-bit FNV-1a) and prefixes it with a short
 * alnum slice of the id for human readability:
 *
 *   "4e2f8b9e-4d46-4744-b08b-1cdc7623f121" -> "4e2f8b9e-752dfcbbee8ce090"
 *
 * The prefix is cosmetic; the hash carries the identity. Two ids sharing any
 * prefix still differ in the hash.
 *
 * Path safety is structural rather than defensive: the output alphabet is
 * [A-Za-z0-9] plus one '-' and lowercase hex, so a hostile session id (these
 * can arrive in a request body from the webchat git panel or an editor) cannot
 * produce a "../.." that escapes the worktrees directory. Callers splice the
 * result into paths and branch names directly.
 */
#ifndef DEC_SESSION_WORKTREE_KEY_H
#define DEC_SESSION_WORKTREE_KEY_H 1

#include <stddef.h>

/* Length of the key's hash half (16 lowercase hex chars) plus the separator,
 * plus the readability prefix (up to 8 chars): 8 + 1 + 16 = 25, + NUL. */
#define SESSION_WORKTREE_KEY_MAX 26

/* Derive the worktree/branch key for `sid` into out[cap]. Writes "" when sid is
 * NULL/empty or cap is too small. Pure and stable across processes, builds and
 * platforms — a session resuming later must land on the same worktree. */
void session_worktree_key(const char *sid, char *out, size_t cap);

/* The PREVIOUS server-side derivation: the first 16 sanitized characters of the
 * session id. Retained solely so a session that already owns a worktree under
 * the old key can find it and clean it up. Never use it to place new work — it
 * is the colliding derivation described above. */
void session_worktree_key_legacy(const char *sid, char *out, size_t cap);

/* True when `name` is shaped like a key this module produced (a legacy 16-char
 * key or a current "<prefix>-<16 hex>"), i.e. it is already a key and must not
 * be re-derived. Guards the callers that recover a key from a worktree PATH:
 * re-keying an already-keyed value used to be harmless only because truncating
 * 16 chars to 16 chars is a no-op, which stopped being true once the key
 * carried a hash. */
int session_worktree_key_is_key(const char *name);

#endif /* DEC_SESSION_WORKTREE_KEY_H */
