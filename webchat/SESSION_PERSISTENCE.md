# Per-user persistent chat sessions

Webchat now persists, **per logged-in user**, the list of chat sessions (one per
browser tab) so a user can reopen their browser after a crash and restore every
tab. Conversation history itself already lives on aimee-server keyed by the
session id; webchat stores only *ownership* and lightweight metadata (title,
cwd, timestamps) in its SQLite DB (`webchat.db`, table `chat_sessions`).

This document is the **backend contract** the frontend SPA
(`RakuenSoftware/smoothgui`) integrates against. The Go backend is complete; the
SPA changes described under "Frontend integration" live in that separate repo.

## Identity

- A **session** == one tab's conversation, identified by the aimee-server
  conversation id, the same value the SPA already sends as `aimee_session_id`
  on `POST /api/chat/send` and that aimee-server echoes back in the `session`
  stream event.
- All endpoints are gated by the existing login session cookie (`requireAuth`)
  and scoped to the authenticated username. A user only ever sees, restores, or
  deletes their own sessions.

## Endpoints

### `GET /api/chat/sessions`

Returns the user's sessions, most-recently-active first (bare JSON array).
(Distinct from `/api/chat/threads`, which is the unrelated, not-yet-implemented
in-tab conversation-branch surface.)

```json
[
  {
    "id": "web-ab12cd34",
    "title": "fix the login redirect bug",
    "cwd": "/home/me/proj",
    "created_at": "2026-06-04T10:00:00Z",
    "last_active": "2026-06-04T11:30:00Z"
  }
]
```

Empty list is `[]`. Use this on app load to render the restorable tab list.

### `GET /api/chat/session?sid=<id>`

Returns one session's metadata, plus back-compat bootstrap fields the SPA
already consumes:

```json
{
  "session_id": "web-ab12cd34",
  "csrf": "",
  "prompt_tier": "standard",
  "exists": true,
  "title": "fix the login redirect bug",
  "cwd": "/home/me/proj",
  "created_at": "...",
  "last_active": "..."
}
```

For an unknown / not-yet-persisted `sid`, `exists` is `false` and the metadata
fields are omitted (the other fields still return so existing bootstrap logic is
unaffected).

### `POST /api/chat/session`

Create or update (rename) a session. Body:

```json
{ "id": "web-ab12cd34", "title": "optional explicit title", "cwd": "/optional" }
```

`id` may also be sent as `sid` or `aimee_session_id`. An explicitly supplied
non-empty `title` **overwrites** the stored title (this is the rename path); an
empty/absent title leaves any existing title untouched. Returns the stored
record (same shape as a `threads` element). `401` if unauthenticated, `400` if
no id.

### `DELETE /api/chat/session?sid=<id>`

Forget a session (close a tab). Returns `{"status":"ok","deleted":true}`.
Scoped to the user, deleting another user's id is a no-op.

## Automatic recording

The SPA does **not** have to explicitly register a session for it to persist:
`POST /api/chat/send` records the session automatically on every turn. It bumps
`last_active`, fills `cwd` from the turn, and derives the `title` from the first
user message (first line, truncated). The id used is the one aimee-server minted
for a brand-new tab (delivered in the `session` stream event) when the SPA did
not supply one. So even a tab that crashed mid-first-message is restorable.

`POST /api/chat/session` is therefore only needed for **explicit** actions:
rename, or pre-registering a tab before its first turn.

## Frontend integration (smoothgui repo)

1. **On app load**, call `GET /api/chat/sessions`. Render each entry as a
   restorable tab/thread in the sidebar. Clicking one re-opens that
   conversation by reusing its `id` as the `aimee_session_id` on subsequent
   `POST /api/chat/send` calls (aimee-server replays history for that id).
2. **Stop relying on `localStorage`/`sessionStorage`** as the source of truth
   for the tab list, the server is now authoritative, so the list survives a
   device crash or a switch to another machine. (You may still cache the
   *currently focused* session id client-side for fast reload.)
3. **Per-tab id**: keep generating a stable per-tab id (e.g. `web-<rand>`) and
   send it as `aimee_session_id`; it becomes the session's `id`. Each tab gets
   its own id so each persists independently.
4. **Rename**: `POST /api/chat/session` with `{id, title}`.
5. **Close/forget a tab**: `DELETE /api/chat/session?sid=<id>`.
6. The thread endpoints (`/api/chat/threads`, `/branch`, `/switch-thread`,
   `/rewind`) remain stubs; in-tab conversation-branch management is a separate
   feature, out of scope for per-user tab restore.

## Frontend status (aimee/frontend)

The aimee web SPA (`frontend/src/pages/Chat.tsx`) integrates this as of the
session-frontend change: on mount it fetches `GET /api/chat/sessions` and merges
any server-side sessions the local `localStorage` cache is missing (restoring
tabs after a crash or on a fresh device), and `closeTab` issues
`DELETE /api/chat/session?sid=` so a closed tab does not reappear. `localStorage`
is retained as a fast local cache (it also holds per-tab message history); the
server is authoritative for *which* tabs exist.
