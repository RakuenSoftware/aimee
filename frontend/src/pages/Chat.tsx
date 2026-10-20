import { parseJSON, stringifyJSON } from '../ir/json';
import './chat.css';
import { mergeChannelMessages, type ChannelMessage } from './chat/channelMessages';
import { useSessionPreference } from './chat/useSessionPreference';
import { useEffect, useRef, useState, useCallback, memo } from 'react';
import { AutoGrowTextarea, Button, Drawer, Tabs, TypingIndicator, tokens } from '@rakuensoftware/smoothgui';
import { BootstrapBanner, DiffBlock, Message, RewindMarker, ThinkingBlock, ToolBlock, TurnSummaryCard } from './chat/ChatPrimitives';
import { renderWithMentions } from './chat/markdown';
import ProjectPicker from '../components/ProjectPicker';
import { useSessions } from '../SessionContext';
import IRTranscript from '../ir/Transcript';
import { emptySession, reduceSession } from '../ir/session';
import type { Envelope, SessionState } from '../ir/session';
import type { ControlReply } from '../ir/ControlCard';
import { reconcileSessionMessages } from '../sessionPersistence';

/* ---- Types ---- */

interface TabMessage {
  role: 'user' | 'assistant' | 'narration';
  text: string;
}

interface ThreadInfo {
  id: number;
  parent_id: number;
  branch_msg_index: number;
  label: string;
  msg_count: number;
  active: boolean;
}

interface TabData {
  /* The app-level session this conversation belongs to (top tab). Tabs mirror
   * the SessionContext 1:1 by this id, so each session keeps its own history. */
  sessionId?: string;
  title: string;
  messages: TabMessage[];
  sid: string;
  aimeeSid: string;
  /* Unified-presence attachment id for this tab's session (minted by
   * POST /api/chat/attach on first send; forwarded on each turn so aimee-server
   * serializes turns across surfaces). */
  attachId?: string;
  workflowChannel?: string;
  threads?: ThreadInfo[];
  activeThreadId?: number;
}

interface BootstrapStack {
  name: string;
}

interface PersonaInfo {
  name: string;
  description?: string;
}


interface WorkflowSessionInfo {
  id: number;
  template: string;
  channel: string;
  status: string;
  current_phase: number;
  current_turn: number;
  phase_count: number;
  turns_in_phase: number;
  phase_name: string;
  expected_agent: string;
  expected_role: string;
  paused_reason: string;
  created_at: string;
  updated_at: string;
  next_prompt?: string;
  recent_context?: string;
}

interface ChannelInfo {
  name: string;
  created_at?: string;
}

interface PluginInfo {
  name: string;
  version: string;
  kind: string;
  enabled: boolean;
  hook_count: number;
  tool_count: number;
  source_path: string;
}

interface MetricRow {
  role: string;
  total: number;
  tokens: number;
  cache_write_tokens: number;
  cache_read_tokens: number;
  estimated_cost_usd: number;
}

interface ModelInfo {
  name: string;
  family: string;
  slot: number;
  state: string;
  role: string;
  registered_at: number;
  last_heartbeat: number;
}

interface MentionQuery {
  start: number;
  end: number;
  query: string;
}

/* ---- Persistence ---- */

const TABS_KEY = 'aimee_chat_tabs';
const ACTIVE_TAB_KEY = 'aimee_active_chat_tab';
const CHANNEL_KEY = 'aimee_active_channel';
const PROJECT_ROOT_KEY = 'aimee_chat_project_root';
const RULES_BANNER_DISMISSED_KEY = 'aimee_rules_banner_dismissed';
const STREAM_PERSIST_DEBOUNCE_MS = 1000;
// How many of a tab's most-recent messages the transcript renders by default.
// Full history is kept in memory (and persisted), but only this window is mounted
// to the DOM / reconciled per stream flush, so render cost is bounded by the
// window, not the whole conversation. Scrolling/"show earlier" reveals the rest.
const TRANSCRIPT_WINDOW = 150;
const TRANSCRIPT_WINDOW_STEP = 150;

function loadActiveChannel(): string | null {
  try { return localStorage.getItem(CHANNEL_KEY); } catch { return null; }
}

function saveActiveChannel(name: string | null): void {
  try {
    if (name) localStorage.setItem(CHANNEL_KEY, name);
    else localStorage.removeItem(CHANNEL_KEY);
  } catch { /* ignore */ }
}

function saveProjectRoot(root: string): void {
  try {
    if (root) localStorage.setItem(PROJECT_ROOT_KEY, root);
    else localStorage.removeItem(PROJECT_ROOT_KEY);
  } catch { /* ignore */ }
}

function loadActiveTabIndex(): number {
  try {
    const raw = localStorage.getItem(ACTIVE_TAB_KEY);
    if (!raw) return 0;
    const parsed = Number.parseInt(raw, 10);
    return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
  } catch { return 0; }
}

function saveActiveTabIndex(index: number): void {
  try { localStorage.setItem(ACTIVE_TAB_KEY, String(Math.max(0, index))); } catch { /* ignore */ }
}

function newAimeeSessionId(): string {
  try {
    const cryptoApi = globalThis.crypto;
    if (cryptoApi?.randomUUID) return `web-${cryptoApi.randomUUID()}`;
  } catch { /* ignore */ }
  return `web-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

function normalizeTab(tab: Partial<TabData> | null | undefined, index: number): TabData {
  const messages = Array.isArray(tab?.messages) ? tab.messages : [];
  return {
    sessionId: typeof tab?.sessionId === 'string' ? tab.sessionId : undefined,
    title: typeof tab?.title === 'string' && tab.title ? tab.title : (index === 0 ? 'Chat' : `Chat ${index + 1}`),
    messages,
    sid: typeof tab?.sid === 'string' ? tab.sid : '',
    aimeeSid: typeof tab?.aimeeSid === 'string' && tab.aimeeSid ? tab.aimeeSid : newAimeeSessionId(),
    attachId: typeof tab?.attachId === 'string' ? tab.attachId : undefined,
    workflowChannel: tab?.workflowChannel,
    threads: tab?.threads,
    activeThreadId: tab?.activeThreadId,
  };
}

function loadTabs(): TabData[] {
  try {
    const raw = localStorage.getItem(TABS_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as Partial<TabData>[];
      if (Array.isArray(parsed) && parsed.length > 0) return parsed.map(normalizeTab);
    }
  } catch { /* ignore */ }
  return [normalizeTab({ title: 'Chat', messages: [], sid: '' }, 0)];
}

function loadInitialChatState(): { tabs: TabData[]; activeIdx: number } {
  const tabs = loadTabs();
  const activeIdx = Math.min(loadActiveTabIndex(), Math.max(0, tabs.length - 1));
  return { tabs, activeIdx };
}

/* Persist the tab list to localStorage, but (1) bound each tab's stored history
 * full history (it's the browser-side restore source and must stay recoverable),
 * coalesce writes onto an idle callback so the synchronous JSON.stringify +
 * setItem never runs on the React commit path during a streaming burst. Rapid
 * tabs changes collapse into one write; a trailing timeout guarantees it still
 * runs if the main thread stays busy. */
let pendingTabsToPersist: TabData[] | null = null;
let tabsWriteScheduled = false;

function writePendingTabs(): void {
  tabsWriteScheduled = false;
  const tabs = pendingTabsToPersist;
  pendingTabsToPersist = null;
  if (!tabs) return;
  try { localStorage.setItem(TABS_KEY, JSON.stringify(tabs)); } catch { /* ignore */ }
}

function saveTabs(tabs: TabData[]): void {
  pendingTabsToPersist = tabs;
  if (tabsWriteScheduled) return;
  tabsWriteScheduled = true;
  const ric = (globalThis as { requestIdleCallback?: (cb: () => void, opts?: { timeout: number }) => void }).requestIdleCallback;
  if (ric) ric(writePendingTabs, { timeout: 1000 });
  else window.setTimeout(writePendingTabs, 200);
}

/* Flush any coalesced tabs write synchronously — call before the page unloads so
 * a pending in-flight write isn't dropped. */
function flushPendingTabs(): void {
  if (pendingTabsToPersist) writePendingTabs();
}

/* localStorage is a fast cache. SessionContext reconciles it with the
 * authenticated user's server-owned session list and transcript. */

function rulesBannerDismissedKey(root: string): string {
  return root ? `${RULES_BANNER_DISMISSED_KEY}:${root}` : RULES_BANNER_DISMISSED_KEY;
}

function loadRulesBannerDismissed(root = ''): boolean {
  try { return localStorage.getItem(rulesBannerDismissedKey(root)) === '1'; } catch { return false; }
}

function saveRulesBannerDismissed(dismissed: boolean, root = ''): void {
  try {
    const key = rulesBannerDismissedKey(root);
    if (dismissed) localStorage.setItem(key, '1');
    else localStorage.removeItem(key);
  } catch { /* ignore */ }
}

function streamToTabMessages(msgs: StreamMsg[]): TabMessage[] {
  return msgs
    .filter(m => m.type === 'user' || m.type === 'assistant' || m.type === 'narration')
    // Never persist an empty assistant/narration bubble (it reloads as an empty
    // box); user turns always carry text.
    .filter(m => m.type === 'user' || m.text.trim() !== '')
    .map(m => ({ role: m.type as 'user' | 'assistant' | 'narration', text: m.text }));
}

function sameTabMessages(a: TabMessage[], b: TabMessage[]): boolean {
  if (a.length !== b.length) return false;
  return a.every((msg, i) => msg.role === b[i].role && msg.text === b[i].text);
}

function isAbortError(err: unknown): boolean {
  return err instanceof Error && err.name === 'AbortError';
}

/* ---- Markdown renderer ---- */

interface BootstrapStatus {
  has_rules: boolean;
  stacks?: BootstrapStack[];
}

/* ---- Message item (rendered from saved tab or streaming) ---- */

interface StreamMsg {
  id: number;
  type: 'user' | 'assistant' | 'thinking' | 'tool' | 'narration' | 'diff' | 'checkpoint';
  text: string;       // user/assistant text
  thinkText?: string; // for thinking
  toolName?: string;
  toolArgs?: string;
  toolResult?: string;
  diffPath?: string;
  diffContent?: string;
  snapshotId?: number; // for checkpoint
}

interface ActiveStreamRefs {
  assistantId: number | null;
  thinkId: number | null;
  toolId: number | null;
  /* aimeeSid of the tab that owns this stream. Captured at send time so SSE
   * events (notably the provider `session` id) are applied to the originating
   * tab — never to whatever tab happens to be active when the event arrives.
   * Without this, switching tabs mid-stream cross-writes provider metadata onto
   * the wrong tab and corrupts the browser's restored-session cache. */
  originSid: string;
}

interface QueuedChatSend {
  projectRoot: string;
  text: string;
  version: number;
  /* aimeeSid of the tab this send was enqueued from. Captured at enqueue time so
   * the turn — and its busy indicator — stays bound to the originating tab even
   * if the user switches tabs before the queue drains. */
  originSid: string;
}

/* Per-session work state. The chat renders only the active tab's conversation, so
 * a single component-wide "working" flag lit up the indicator on whatever tab the
 * user viewed, not the one actually running a turn. Keying work by aimeeSid lets
 * the render derive busy/iteration state for the active tab alone. */
interface SidWork {
  pending: number;  // in-flight send controllers for this sid
  queued: number;   // queued (not yet dispatched) sends for this sid
  iterCur: number;  // current agent iteration (0 = none)
  iterMax: number;
}

let msgIdCounter = 0;
function nextId() { return ++msgIdCounter; }

/* ---- Thread Bar (conversation branching) ---- */

interface ThreadBarProps {
  threads: ThreadInfo[];
  activeThreadId: number;
  onSwitch: (id: number) => void;
  onBranch: () => void;
}

function ThreadBar({ threads, activeThreadId, onSwitch, onBranch }: ThreadBarProps) {
  if (threads.length <= 1) {
    return (
      <div style={{
        display: 'flex', alignItems: 'center', padding: '4px 16px',
        background: tokens.surfaceAlt, borderBottom: `1px solid ${tokens.borderLight}`,
        fontSize: '12px', color: tokens.textPale, gap: '8px',
      }}>
        <Button
          variant="default"
          size="sm"
          onClick={onBranch}
          onMouseOver={e => (e.currentTarget.style.borderColor = tokens.primary)}
          onMouseOut={e => (e.currentTarget.style.borderColor = tokens.borderMedium)}
          title="Branch conversation to explore an alternative approach"
        >
          Branch
        </Button>
        <span>No branches — branch to explore alternatives</span>
      </div>
    );
  }

  return (
    <div style={{
      display: 'flex', alignItems: 'center', padding: '4px 16px',
      background: tokens.surfaceAlt, borderBottom: `1px solid ${tokens.borderLight}`,
      fontSize: '12px', gap: '6px', overflowX: 'auto',
    }}>
      <span style={{ color: tokens.textPale, whiteSpace: 'nowrap', marginRight: '4px' }}>Threads:</span>
      {threads.map(t => (
        <button
          key={t.id}
          onClick={() => onSwitch(t.id)}
          style={{
            padding: '2px 10px', fontSize: '11px',
            color: t.id === activeThreadId ? tokens.primary : tokens.textSecondary,
            background: t.id === activeThreadId ? tokens.surface : 'transparent',
            border: t.id === activeThreadId
              ? `1px solid ${tokens.primary}`
              : `1px solid ${tokens.borderMedium}`,
            borderRadius: '4px', cursor: 'pointer', whiteSpace: 'nowrap',
          }}
          title={`${t.label} (${t.msg_count} msgs, branched from ${t.parent_id} at msg ${t.branch_msg_index})`}
        >
          {t.label || (t.parent_id < 0 ? 'root' : `thread-${t.id}`)}
          <span style={{ color: tokens.textPale, marginLeft: '4px' }}>({t.msg_count})</span>
        </button>
      ))}
      <Button
        variant="ghost"
        size="sm"
        onClick={onBranch}
        onMouseOver={e => (e.currentTarget.style.borderColor = tokens.primary)}
        onMouseOut={e => (e.currentTarget.style.borderColor = tokens.borderMedium)}
        title="Branch from current point"
        style={{ border: `1px dashed ${tokens.borderMedium}` }}
      >
        + Branch
      </Button>
    </div>
  );
}

/* ---- Collaborative Rules Panel ---- */

interface CollabRule {
  id: number;
  text: string;
  reason: string;
  proposed_by: string;
  status: string;
  created_at: string;
  decided_at: string;
}

function RulesPanel({ open, onToggle }: { open: boolean; onToggle: () => void }) {
  const [rules, setRules] = useState<CollabRule[]>([]);
  const [epoch, setEpoch] = useState(0);

  const fetchRules = useCallback(() => {
    fetch('/api/rules')
      .then(r => r.json())
      .then((data: CollabRule[]) => {
        if (Array.isArray(data)) setRules(data);
      })
      .catch(() => {});
    fetch('/api/rules/active')
      .then(r => r.json())
      .then((data: { epoch?: number }) => {
        if (typeof data.epoch === 'number') setEpoch(data.epoch);
      })
      .catch(() => {});
  }, []);

  useEffect(() => {
    if (open) fetchRules();
  }, [open, fetchRules]);

  async function doAction(id: number, action: string) {
    await fetch(`/api/rules/${id}/${action}`, {
      method: 'POST',
      headers: { 'X-CSRF-Token': window._csrf || '' },
    });
    fetchRules();
  }

  const proposed = rules.filter(r => r.status === 'proposed');
  const active = rules.filter(r => r.status === 'active');
  const inactive = rules.filter(r => r.status === 'rejected' || r.status === 'retired');

  return (
    <Drawer
      open={open}
      onToggle={onToggle}
      title="Rules"
      subtitle={`epoch ${epoch}`}
      badge={active.length || undefined}
      alert={proposed.length > 0 ? <span style={{ color: tokens.warning, fontWeight: 'bold' }}>{proposed.length} pending</span> : undefined}
      side="right"
      width={280}
    >
        {proposed.length > 0 && (
          <div style={{ marginBottom: 12 }}>
            <div style={{ color: tokens.warning, fontWeight: 'bold', fontSize: '11px', marginBottom: 4, textTransform: 'uppercase' }}>
              Pending Approval
            </div>
            {proposed.map(r => (
              <div key={r.id} style={{
                padding: '8px', marginBottom: 6, background: tokens.surface,
                border: `1px solid ${tokens.borderMedium}`, borderRadius: '4px',
              }}>
                <div style={{ color: tokens.text, marginBottom: 4 }}>{r.text}</div>
                {r.reason && (
                  <div style={{ color: tokens.textFaint, fontSize: '11px', marginBottom: 6 }}>
                    {r.reason}
                  </div>
                )}
                <div style={{ color: tokens.textHint, fontSize: '10px', marginBottom: 6 }}>
                  by {r.proposed_by || 'unknown'}
                </div>
                <div style={{ display: 'flex', gap: 6 }}>
                  <Button
                    variant="primary"
                    size="sm"
                    onClick={() => doAction(r.id, 'approve')}
                  >
                    Approve
                  </Button>
                  <Button
                    variant="danger"
                    size="sm"
                    onClick={() => doAction(r.id, 'reject')}
                  >
                    Reject
                  </Button>
                </div>
              </div>
            ))}
          </div>
        )}
        {active.length > 0 && (
          <div style={{ marginBottom: 12 }}>
            <div style={{ color: tokens.primary, fontWeight: 'bold', fontSize: '11px', marginBottom: 4, textTransform: 'uppercase' }}>
              Active ({active.length}/{COLLAB_MAX_ACTIVE})
            </div>
            {active.map(r => (
              <div key={r.id} style={{
                padding: '8px', marginBottom: 6, background: tokens.surface,
                border: `1px solid ${tokens.borderMedium}`, borderRadius: '4px',
              }}>
                <div style={{ color: tokens.text, marginBottom: 4 }}>{r.text}</div>
                <div style={{ display: 'flex', justifyContent: 'flex-end' }}>
                  <Button
                    variant="default"
                    size="sm"
                    onClick={() => doAction(r.id, 'retire')}
                  >
                    Retire
                  </Button>
                </div>
              </div>
            ))}
          </div>
        )}
        {inactive.length > 0 && (
          <details style={{ marginBottom: 12 }}>
            <summary style={{ color: tokens.textFaint, fontSize: '11px', cursor: 'pointer', textTransform: 'uppercase' }}>
              History ({inactive.length})
            </summary>
            <div style={{ marginTop: 4 }}>
              {inactive.map(r => (
                <div key={r.id} style={{
                  padding: '6px', marginBottom: 4, fontSize: '12px',
                  color: tokens.textFaint, borderLeft: `2px solid ${tokens.borderLight}`, paddingLeft: 8,
                }}>
                  <span style={{ textDecoration: 'line-through' }}>{r.text}</span>
                  <span style={{ marginLeft: 6, fontSize: '10px', color: tokens.textHint }}>
                    ({r.status})
                  </span>
                </div>
              ))}
            </div>
          </details>
        )}
        {rules.length === 0 && (
          <div style={{ color: tokens.textFaint, textAlign: 'center', padding: '20px 0' }}>
            No collaborative rules yet. Agents can propose rules via the rules_propose tool.
          </div>
        )}
    </Drawer>
  );
}

const COLLAB_MAX_ACTIVE = 10;

function ContextPanel({ open, onToggle, sessionUsage, sessionId, msgCount, metrics }: {
  open: boolean;
  onToggle: () => void;
  sessionUsage: { in: number; out: number; cost: number; kind: string };
  sessionId: string;
  msgCount: number;
  metrics: MetricRow[];
}) {
  // metrics rows are realized spend (the server reader filters out
  // estimated/avoided/partial), so this total is realized billable spend.
  const totalCost = metrics.reduce((s, m) => s + m.estimated_cost_usd, 0);
  const totalTokens = metrics.reduce((s, m) => s + m.tokens + m.cache_write_tokens + m.cache_read_tokens, 0);
  const sessionTokens = sessionUsage.in + sessionUsage.out;
  const usagePct = totalCost > 0 ? (sessionUsage.cost / totalCost * 100) : 0;

  return (
    <Drawer
      open={open}
      onToggle={onToggle}
      title="Context"
      side="right"
      width={260}
    >
        <div style={{ marginBottom: 12 }}>
          <div style={{ color: tokens.textSecondary, fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', marginBottom: 6 }}>Session</div>
          <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 3 }}>
            <span style={{ color: tokens.textFaint, fontSize: '12px' }}>ID</span>
            <span style={{ color: tokens.textPale, fontSize: '12px', fontFamily: 'monospace' }}>
              {sessionId ? sessionId.slice(0, 8) + '…' : '—'}
            </span>
          </div>
          <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 3 }}>
            <span style={{ color: tokens.textFaint, fontSize: '12px' }}>Messages</span>
            <span style={{ color: tokens.textPale, fontSize: '12px' }}>{msgCount}</span>
          </div>
        </div>

        <div style={{ marginBottom: 12 }}>
          <div style={{ color: tokens.textSecondary, fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', marginBottom: 6 }}>Tokens Used</div>
          <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 3 }}>
            <span style={{ color: tokens.textFaint, fontSize: '12px' }}>Input</span>
            <span style={{ color: tokens.textPale, fontSize: '12px' }}>{sessionUsage.in.toLocaleString()}</span>
          </div>
          <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 3 }}>
            <span style={{ color: tokens.textFaint, fontSize: '12px' }}>Output</span>
            <span style={{ color: tokens.textPale, fontSize: '12px' }}>{sessionUsage.out.toLocaleString()}</span>
          </div>
          <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 3 }}>
            <span style={{ color: tokens.textFaint, fontSize: '12px' }}>Total</span>
            <span style={{ color: tokens.text, fontSize: '12px', fontWeight: 600 }}>{sessionTokens.toLocaleString()}</span>
          </div>
        </div>

        <div style={{ marginBottom: 12 }}>
          <div style={{ color: tokens.textSecondary, fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', marginBottom: 6 }}>Spend (realized)</div>
          <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 3 }}>
            <span style={{ color: tokens.textFaint, fontSize: '12px' }}>
              Session{sessionUsage.kind && sessionUsage.kind !== 'realized' ? ` (${sessionUsage.kind})` : ''}
            </span>
            <span style={{ color: sessionUsage.cost > 0 ? tokens.text : tokens.textPale, fontSize: '12px' }}>
              {sessionUsage.cost > 0 ? `~$${sessionUsage.cost.toFixed(4)}` : '—'}
            </span>
          </div>
          {totalCost > 0 && (
            <>
              <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 3 }}>
                <span style={{ color: tokens.textFaint, fontSize: '12px' }}>All-time realized</span>
                <span style={{ color: tokens.textPale, fontSize: '12px' }}>${totalCost.toFixed(4)}</span>
              </div>
              <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 3 }}>
                <span style={{ color: tokens.textFaint, fontSize: '12px' }}>Usage %</span>
                <span style={{ color: tokens.textPale, fontSize: '12px' }}>
                  {usagePct > 0 ? `${usagePct.toFixed(1)}%` : '—'}
                </span>
              </div>
            </>
          )}
        </div>

        {totalTokens > 0 && metrics.length > 0 && (
          <div>
            <div style={{ color: tokens.textSecondary, fontSize: '11px', fontWeight: 700, textTransform: 'uppercase', marginBottom: 6 }}>Provider Breakdown</div>
            {metrics.map(m => {
              const mTokens = m.tokens + m.cache_write_tokens + m.cache_read_tokens;
              const pct = totalCost > 0 ? m.estimated_cost_usd / totalCost * 100 : 0;
              return (
                <div key={m.role} style={{
                  padding: '6px 8px', marginBottom: 4, background: tokens.surface,
                  border: `1px solid ${tokens.borderLight}`, borderRadius: '4px',
                }}>
                  <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 2 }}>
                    <span style={{ color: tokens.text, fontSize: '12px', fontWeight: 600 }}>{m.role}</span>
                    <span style={{ color: tokens.textSecondary, fontSize: '11px' }}>
                      {pct > 0.05 ? `${pct.toFixed(0)}%` : '<1%'}
                    </span>
                  </div>
                  <div style={{ display: 'flex', justifyContent: 'space-between' }}>
                    <span style={{ color: tokens.textFaint, fontSize: '11px' }}>{mTokens.toLocaleString()} tok</span>
                    <span style={{ color: tokens.textFaint, fontSize: '11px' }}>${m.estimated_cost_usd.toFixed(4)}</span>
                  </div>
                </div>
              );
            })}
          </div>
        )}
    </Drawer>
  );
}

function PluginsPanel({ open, plugins, loading, error, onToggle, onRefresh, onPluginToggle }: {
  open: boolean;
  plugins: PluginInfo[];
  loading: boolean;
  error: string | null;
  onToggle: () => void;
  onRefresh: () => void;
  onPluginToggle: (name: string) => void;
}) {
  return (
    <Drawer
      open={open}
      onToggle={onToggle}
      title="Plugins"
      badge={plugins.length || undefined}
      side="right"
      width={300}
    >
        <div style={{ display: 'flex', justifyContent: 'flex-end', marginBottom: 8 }}>
          <Button variant="ghost" size="sm" onClick={onRefresh}>Refresh</Button>
        </div>
        {loading && <div style={{ color: tokens.textFaint }}>Loading plugins…</div>}
        {!loading && error && <div style={{ color: tokens.danger }}>{error}</div>}
        {!loading && !error && plugins.length === 0 && (
          <div style={{ color: tokens.textFaint, lineHeight: 1.5 }}>
            No plugins installed. Use <code>aimee plugin install &lt;path&gt;</code> to add one.
          </div>
        )}
        {!loading && !error && plugins.map(plugin => (
          <div key={plugin.name} style={{
            padding: '10px', marginBottom: 8, background: tokens.surface,
            border: `1px solid ${tokens.borderMedium}`, borderRadius: '6px',
          }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', gap: 8, alignItems: 'center', marginBottom: 6 }}>
              <div style={{ minWidth: 0 }}>
                <div style={{ color: tokens.text, fontWeight: 600, overflow: 'hidden', textOverflow: 'ellipsis' }}>{plugin.name}</div>
                <div style={{ color: tokens.textHint, fontSize: '11px' }}>{plugin.version || 'unversioned'} · {plugin.kind || 'local'}</div>
              </div>
              <button
                onClick={() => onPluginToggle(plugin.name)}
                style={{
                  padding: '4px 10px', fontSize: '11px', borderRadius: '999px',
                  cursor: 'pointer', border: `1px solid ${plugin.enabled ? 'var(--sg-success-dark)' : tokens.borderMedium}`,
                  background: plugin.enabled ? 'var(--sg-success-bg)' : 'transparent',
                  color: plugin.enabled ? 'var(--sg-success)' : tokens.textSecondary,
                  whiteSpace: 'nowrap',
                }}
              >
                {plugin.enabled ? 'Enabled' : 'Disabled'}
              </button>
            </div>
            <div style={{ display: 'flex', gap: 12, color: tokens.textSecondary, fontSize: '11px', marginBottom: 6 }}>
              <span>{plugin.hook_count} hook{plugin.hook_count === 1 ? '' : 's'}</span>
              <span>{plugin.tool_count} tool{plugin.tool_count === 1 ? '' : 's'}</span>
            </div>
            <div style={{ color: tokens.textHint, fontSize: '10px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} title={plugin.source_path}>
              {plugin.source_path}
            </div>
          </div>
        ))}
    </Drawer>
  );
}


function WorkflowStatusCard({
  channel,
  session,
  loading,
  error,
  changed,
  onChannelChange,
  onRefresh,
  onPause,
}: {
  channel: string;
  session: WorkflowSessionInfo | null;
  loading: boolean;
  error: string | null;
  changed: boolean;
  onChannelChange: (value: string) => void;
  onRefresh: () => void;
  onPause: () => void;
}) {
  return (
    <div style={{
      margin: '8px 16px 0',
      padding: '10px 12px',
      background: tokens.surfaceAlt,
      border: `1px solid ${tokens.borderLight}`,
      borderRadius: '8px',
      flexShrink: 0,
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: '8px', marginBottom: '8px', flexWrap: 'wrap' }}>
        <span style={{ fontSize: '12px', fontWeight: 600, color: tokens.text }}>Workflow</span>
        {changed && (
          <span style={{
            padding: '2px 8px',
            borderRadius: '999px',
            fontSize: '10px',
            background: 'var(--sg-success-bg)',
            color: 'var(--sg-success)',
            border: '1px solid var(--sg-success-dark)',
          }}>
            Updated
          </span>
        )}
        <input
          value={channel}
          onChange={e => onChannelChange(e.target.value)}
          placeholder="Channel"
          style={{
            minWidth: '180px',
            flex: '0 1 240px',
            padding: '5px 8px',
            fontSize: '12px',
            background: tokens.surface,
            color: tokens.text,
            border: `1px solid ${tokens.borderMedium}`,
            borderRadius: '6px',
            outline: 'none',
          }}
        />
        <Button
          variant="default"
          size="sm"
          onClick={onRefresh}
        >
          Refresh
        </Button>
        {session && session.status !== 'complete' && (
          <Button
            variant="default"
            size="sm"
            onClick={onPause}
            style={{ color: tokens.warning }}
          >
            Pause
          </Button>
        )}
      </div>

      {loading && (
        <div style={{ fontSize: '12px', color: tokens.textHint }}>Loading workflow state…</div>
      )}
      {!loading && error && (
        <div style={{ fontSize: '12px', color: tokens.danger }}>{error}</div>
      )}
      {!loading && !error && !session && (
        <div style={{ fontSize: '12px', color: tokens.textFaint }}>
          No workflow session is currently bound to this channel.
        </div>
      )}
      {!loading && !error && session && (
        <div style={{ display: 'flex', flexDirection: 'column', gap: '6px' }}>
          <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', alignItems: 'center' }}>
            <span style={{ fontSize: '12px', color: tokens.text }}>
              <strong>{session.template}</strong> in <code>{session.channel}</code>
            </span>
            <span style={{
              padding: '2px 8px',
              borderRadius: '999px',
              fontSize: '10px',
              background: session.status === 'active' ? 'var(--sg-success-bg)' : session.status === 'paused' ? 'var(--sg-warning-bg)' : 'var(--sg-info-bg)',
              color: session.status === 'active' ? 'var(--sg-success)' : session.status === 'paused' ? 'var(--sg-warning)' : 'var(--sg-info-border)',
              border: `1px solid ${tokens.borderMedium}`,
              textTransform: 'uppercase',
            }}>
              {session.status}
            </span>
            <span style={{ fontSize: '11px', color: tokens.textSecondary }}>
              Phase {session.current_phase + 1}/{session.phase_count}: {session.phase_name}
            </span>
            <span style={{ fontSize: '11px', color: tokens.textSecondary }}>
              Turn {session.current_turn + 1}/{session.turns_in_phase}
            </span>
          </div>
          <div style={{ fontSize: '12px', color: tokens.textSecondary }}>
            Expected next: <strong>{session.expected_agent || 'none'}</strong>
            {session.expected_role ? ` (${session.expected_role})` : ''}
          </div>
          {session.paused_reason && (
            <div style={{ fontSize: '12px', color: tokens.warning }}>
              Pause reason: {session.paused_reason}
            </div>
          )}
          {session.next_prompt && (
            <details>
              <summary style={{ cursor: 'pointer', fontSize: '11px', color: tokens.textHint }}>
                Next prompt
              </summary>
              <pre style={{
                margin: '6px 0 0',
                padding: '8px',
                background: tokens.surface,
                border: `1px solid ${tokens.borderLight}`,
                borderRadius: '6px',
                fontSize: '11px',
                color: tokens.textPale,
                whiteSpace: 'pre-wrap',
                maxHeight: '140px',
                overflowY: 'auto',
              }}>
                {session.next_prompt}
              </pre>
            </details>
          )}
        </div>
      )}
    </div>
  );
}

/* ---- @mention highlight ---- */

/* ---- ChannelSidebar ---- */

interface ChannelSidebarProps {
  channels: ChannelInfo[];
  activeChannel: string | null;
  unreadCounts: Record<string, number>;
  onSelect: (name: string) => void;
  onDeselect: () => void;
  onCreateChannel: (name: string) => void;
}

function ChannelSidebar({
  channels, activeChannel, unreadCounts, onSelect, onDeselect, onCreateChannel,
}: ChannelSidebarProps) {
  const [newName, setNewName] = useState('');

  function handleCreate(e: React.FormEvent) {
    e.preventDefault();
    const n = newName.trim();
    if (!n) return;
    onCreateChannel(n);
    setNewName('');
  }

  return (
    <div style={{
      width: '180px', flexShrink: 0,
      borderRight: `1px solid ${tokens.border}`,
      background: tokens.surfaceAlt,
      display: 'flex', flexDirection: 'column',
      overflow: 'hidden',
    }}>
      <div style={{
        padding: '10px 12px', borderBottom: `1px solid ${tokens.borderLight}`,
        fontSize: '11px', fontWeight: 700, color: tokens.textSecondary,
        textTransform: 'uppercase', letterSpacing: '0.06em',
        display: 'flex', justifyContent: 'space-between', alignItems: 'center',
      }}>
        <span>Channels</span>
        {activeChannel && (
          <button
            onClick={onDeselect}
            title="Back to chat"
            style={{
              background: 'none', border: 'none', cursor: 'pointer',
              color: tokens.textPale, fontSize: '14px', padding: '0 2px',
            }}
          >✕</button>
        )}
      </div>

      <div style={{ flex: 1, overflowY: 'auto' }}>
        {channels.length === 0 && (
          <div style={{ padding: '12px', fontSize: '12px', color: tokens.textPale }}>
            No channels yet
          </div>
        )}
        {channels.map(ch => {
          const unread = unreadCounts[ch.name] ?? 0;
          const isActive = activeChannel === ch.name;
          return (
            <button
              key={ch.name}
              onClick={() => onSelect(ch.name)}
              style={{
                width: '100%', textAlign: 'left',
                padding: '8px 12px',
                background: isActive ? tokens.primary + '22' : 'none',
                border: 'none', cursor: 'pointer',
                color: isActive ? tokens.primary : tokens.text,
                fontSize: '13px',
                display: 'flex', justifyContent: 'space-between', alignItems: 'center',
                borderLeft: isActive ? `3px solid ${tokens.primary}` : '3px solid transparent',
              }}
            >
              <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                # {ch.name}
              </span>
              {unread > 0 && (
                <span style={{
                  background: tokens.primary, color: 'var(--sg-surface)',
                  borderRadius: '10px', fontSize: '10px', fontWeight: 700,
                  padding: '1px 6px', minWidth: '18px', textAlign: 'center',
                }}>
                  {unread > 99 ? '99+' : unread}
                </span>
              )}
            </button>
          );
        })}
      </div>

      {/* Create channel form */}
      <form onSubmit={handleCreate} style={{
        padding: '8px 10px', borderTop: `1px solid ${tokens.borderLight}`,
        display: 'flex', gap: '4px',
      }}>
        <input
          value={newName}
          onChange={e => setNewName(e.target.value)}
          placeholder="new channel…"
          style={{
            flex: 1, fontSize: '12px', padding: '4px 6px',
            background: tokens.surface, color: tokens.text,
            border: `1px solid ${tokens.borderMedium}`, borderRadius: '4px',
            outline: 'none',
          }}
        />
        <button type="submit" style={{
          fontSize: '14px', background: 'none', border: 'none',
          cursor: 'pointer', color: tokens.primary, padding: '0 4px',
        }}>+</button>
      </form>
    </div>
  );
}

/* ---- ChannelView: per-channel message timeline ---- */

interface ChannelViewProps {
  channelName: string;
  messages: ChannelMessage[];
  agents: ModelInfo[];
  onSend: (text: string) => void;
}

function mentionStateColor(state: string): string {
  if (state === 'active') return 'var(--sg-success)';
  if (state === 'idle') return 'var(--sg-warning)';
  return tokens.textPale;
}

function findMentionQuery(text: string, cursor: number): MentionQuery | null {
  const safeCursor = Math.max(0, Math.min(cursor, text.length));
  let start = safeCursor - 1;
  while (start >= 0) {
    const ch = text[start];
    if (ch === '@') break;
    if (!/[A-Za-z0-9._-]/.test(ch)) return null;
    start--;
  }
  if (start < 0 || text[start] !== '@') return null;
  if (start > 0 && /[A-Za-z0-9._-]/.test(text[start - 1])) return null;
  const query = text.slice(start + 1, safeCursor);
  if (!/^[A-Za-z0-9._-]*$/.test(query)) return null;
  return { start, end: safeCursor, query };
}

function applyMention(text: string, mention: MentionQuery, name: string): { nextText: string; nextCursor: number } {
  const replacement = `@${name}`;
  const suffix = text.slice(mention.end);
  const needsSpace = suffix.length === 0 || /\s/.test(suffix[0]) ? '' : ' ';
  const nextText = `${text.slice(0, mention.start)}${replacement}${needsSpace}${suffix}`;
  const nextCursor = mention.start + replacement.length + needsSpace.length;
  return { nextText, nextCursor };
}


function ChannelView({ channelName, messages, agents, onSend }: ChannelViewProps) {
  const [text, setText] = useState('');
  const [selectionStart, setSelectionStart] = useState(0);
  const [mentionIndex, setMentionIndex] = useState(0);
  const endRef = useRef<HTMLDivElement>(null);
  const textareaRef = useRef<HTMLTextAreaElement>(null);

  useEffect(() => {
    endRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages]);

  const mention = findMentionQuery(text, selectionStart);
  const mentionCandidates = mention
    ? agents
      .filter(agent => {
        if (agent.state === 'offline') return false;
        if (!mention.query) return true;
        return agent.name.toLowerCase().startsWith(mention.query.toLowerCase());
      })
      .sort((a, b) => {
        const stateRank = (value: string) => value === 'active' ? 0 : value === 'idle' ? 1 : 2;
        return stateRank(a.state) - stateRank(b.state) || a.name.localeCompare(b.name);
      })
      .slice(0, 6)
    : [];

  useEffect(() => {
    setMentionIndex(0);
  }, [mention?.query, mentionCandidates.length]);

  function selectMention(name: string) {
    if (!mention) return;
    const { nextText, nextCursor } = applyMention(text, mention, name);
    setText(nextText);
    setSelectionStart(nextCursor);
    requestAnimationFrame(() => {
      textareaRef.current?.focus();
      textareaRef.current?.setSelectionRange(nextCursor, nextCursor);
    });
  }

  function handleKeyDown(e: React.KeyboardEvent<HTMLTextAreaElement>) {
    if (mentionCandidates.length > 0) {
      if (e.key === 'ArrowDown') {
        e.preventDefault();
        setMentionIndex(prev => (prev + 1) % mentionCandidates.length);
        return;
      }
      if (e.key === 'ArrowUp') {
        e.preventDefault();
        setMentionIndex(prev => (prev - 1 + mentionCandidates.length) % mentionCandidates.length);
        return;
      }
      if (e.key === 'Tab') {
        e.preventDefault();
        selectMention(mentionCandidates[mentionIndex]?.name ?? mentionCandidates[0].name);
        return;
      }
      if (e.key === 'Escape') {
        e.preventDefault();
        setSelectionStart(text.length);
        return;
      }
      if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        selectMention(mentionCandidates[mentionIndex]?.name ?? mentionCandidates[0].name);
        return;
      }
    }
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      const t = text.trim();
      if (t) { onSend(t); setText(''); }
    }
  }

  return (
    <div style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      <div style={{
        padding: '8px 14px', background: tokens.surfaceAlt,
        borderBottom: `1px solid ${tokens.border}`,
        fontSize: '13px', fontWeight: 600, color: tokens.text,
      }}>
        # {channelName}
      </div>
      <div style={{ flex: 1, overflowY: 'auto', padding: '12px 16px', display: 'flex', flexDirection: 'column', gap: '8px' }}>
        {messages.length === 0 && (
          <div style={{ color: tokens.textPale, fontSize: '13px' }}>No messages yet.</div>
        )}
        {messages.map((m, i) => (
          <div key={m.id ?? i} style={{ fontSize: '13px' }}>
            <span style={{ fontWeight: 600, color: tokens.primary, marginRight: '6px' }}>
              {m.sender}
            </span>
            {m.created_at && (
              <span style={{ fontSize: '11px', color: tokens.textPale, marginRight: '8px' }}>
                {new Date(/(?:Z|[+-]\d\d:\d\d)$/.test(m.created_at) ? m.created_at : m.created_at + 'Z').toLocaleTimeString()}
              </span>
            )}
            <span
              dangerouslySetInnerHTML={{ __html: renderWithMentions(m.text) }}
            />
          </div>
        ))}
        <div ref={endRef} />
      </div>
      <div style={{
        padding: '10px 14px', background: tokens.surfaceAlt,
        borderTop: `1px solid ${tokens.border}`,
        display: 'flex', gap: '8px', position: 'relative',
      }}>
        {mentionCandidates.length > 0 && (
          <div style={{
            position: 'absolute',
            left: 14,
            right: 98,
            bottom: 62,
            background: tokens.surface,
            border: `1px solid ${tokens.borderMedium}`,
            borderRadius: '8px',
            boxShadow: '0 12px 28px rgba(0,0,0,0.18)',
            overflow: 'hidden',
            zIndex: 5,
          }}>
            {mentionCandidates.map((agent, index) => (
              <button
                key={agent.name}
                onMouseDown={e => {
                  e.preventDefault();
                  selectMention(agent.name);
                }}
                style={{
                  width: '100%',
                  padding: '8px 10px',
                  border: 'none',
                  borderTop: index === 0 ? 'none' : `1px solid ${tokens.borderLight}`,
                  background: index === mentionIndex ? `${tokens.primary}22` : tokens.surface,
                  color: tokens.text,
                  cursor: 'pointer',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'space-between',
                  textAlign: 'left',
                  gap: '8px',
                }}
              >
                <span style={{ minWidth: 0 }}>
                  <span style={{ fontWeight: 600, marginRight: '8px' }}>@{agent.name}</span>
                  {agent.role && (
                    <span style={{ color: tokens.textHint, fontSize: '11px' }}>{agent.role}</span>
                  )}
                </span>
                <span style={{ color: mentionStateColor(agent.state), fontSize: '11px', textTransform: 'capitalize' }}>
                  {agent.state}
                </span>
              </button>
            ))}
          </div>
        )}
        <textarea
          ref={textareaRef}
          value={text}
          onChange={e => {
            setText(e.target.value);
            setSelectionStart(e.target.selectionStart ?? e.target.value.length);
          }}
          onSelect={e => setSelectionStart(e.currentTarget.selectionStart ?? e.currentTarget.value.length)}
          onKeyDown={handleKeyDown}
          placeholder={`Message #${channelName}… (Enter to send, @mention to tag)`}
          rows={1}
          style={{
            flex: 1, padding: '8px 10px', background: tokens.surface,
            color: tokens.text, border: `1px solid ${tokens.borderMedium}`,
            borderRadius: '6px', fontSize: '13px', resize: 'none',
            fontFamily: 'system-ui', outline: 'none', maxHeight: '120px',
          }}
        />
        <Button
          variant="primary"
          size="md"
          onClick={() => { const t = text.trim(); if (t) { onSend(t); setText(''); } }}
        >Send</Button>
      </div>
    </div>
  );
}

/* Render one transcript block. `streaming` only matters for an assistant bubble
 * (it then renders raw growing text and skips the markdown parse); every other
 * block type ignores it. */
function renderBlock(m: StreamMsg, activeSid: string, streaming: boolean) {
  switch (m.type) {
    case 'user':
      return <Message key={m.id} role="user" text={m.text} />;
    case 'assistant':
      return <Message key={m.id} role="assistant" text={m.text} streaming={streaming} />;
    case 'thinking':
      return <ThinkingBlock key={m.id} text={m.thinkText ?? ''} />;
    case 'tool':
      return <ToolBlock key={m.id} name={m.toolName ?? ''} args={m.toolArgs ?? ''} result={m.toolResult} />;
    case 'narration':
      return <TurnSummaryCard key={m.id} text={m.text} />;
    case 'diff':
      return <DiffBlock key={m.id} path={m.diffPath} diff={m.diffContent ?? ''} />;
    case 'checkpoint':
      return <RewindMarker key={m.id} snapshotId={m.snapshotId!} sid={activeSid} />;
    default:
      return null;
  }
}

/* The committed (settled) part of the transcript — everything except the live,
 * still-streaming tail block. Memoized with a REFERENCE-ONLY comparator: a
 * stream flush rebuilds the `messages` array but keeps every settled message's
 * object identity (applyDeltas replaces only the one growing message), so this
 * subtree bails in an O(n) pointer scan instead of re-mapping and reconciling
 * the whole conversation as React elements. Without it, each of the ~10 flushes/s
 * costs O(conversation length) of React work, so a long session steadily pegs a
 * core — the "gets worse the longer I chat" symptom. It re-renders only when a
 * block is committed (count changes) or a settled block actually mutates (e.g. a
 * tool_result lands), both of which change an element reference. */
const CommittedTranscript = memo(function CommittedTranscript({ messages, count, activeSid }: {
  messages: StreamMsg[]; count: number; activeSid: string;
}) {
  const blocks = [];
  for (let i = 0; i < count; i++) blocks.push(renderBlock(messages[i], activeSid, false));
  return <>{blocks}</>;
}, (prev, next) => {
  if (prev.count !== next.count || prev.activeSid !== next.activeSid) return false;
  for (let i = 0; i < next.count; i++) {
    if (prev.messages[i] !== next.messages[i]) return false;
  }
  return true;
});

/* Renders the transcript. Extracted + memoized so re-renders of the big Chat
 * component that are NOT a message change (textarea typing, the remote-turn
 * preview, the workflow poll, working toggles) skip it entirely. During a turn
 * only the live tail block is re-rendered per flush; the committed history is
 * gated behind CommittedTranscript's reference comparator. */
const Transcript = memo(function Transcript({ messages, working, activeSid }: {
  messages: StreamMsg[]; working: boolean; activeSid: string;
}) {
  const n = messages.length;
  // While a turn is in flight the last block is the live tail (an assistant
  // bubble streams its raw text; it settles — and renders markdown once — when
  // another block follows it or the turn ends). Everything before it is settled.
  const liveTail = working && n > 0;
  const committedCount = liveTail ? n - 1 : n;
  return (
    <>
      <CommittedTranscript messages={messages} count={committedCount} activeSid={activeSid} />
      {liveTail && renderBlock(messages[n - 1], activeSid, true)}
    </>
  );
});

/* ---- Main Chat component ---- */

export default function Chat() {
  const [initialChatState] = useState(loadInitialChatState);
  const [tabs, setTabs] = useState<TabData[]>(initialChatState.tabs);
  const { sessions, active: activeSession, patchSession } = useSessions();
  const activeSessionId = activeSession?.id ?? '';
  const matchedIdx = tabs.findIndex(tab => tab.sessionId === activeSessionId);
  const activeIdx = matchedIdx >= 0 ? matchedIdx : 0;
  const [streamMsgs, setStreamMsgs] = useState<StreamMsg[]>([]);
  const irSessions = useRef<Record<string, SessionState>>({});
  const irPending = useRef<Record<string, Map<string, Envelope>>>({});
  const irFlush = useRef<number | null>(null);
  const [, setIRRevision] = useState(0);
  const [irTransportErrors, setIRTransportErrors] = useState<Record<string, string>>({});
  const [irReady, setIRReady] = useState<Record<string, boolean>>({});
  const [irReconnect, setIRReconnect] = useState(0);
  function ingestIR(owner: string, raw: Record<string, unknown>) {
    if (raw.status === 'error') throw new Error(String(raw.message || 'Session capture failed'));
    const envelope = raw as unknown as Envelope;
    if (envelope.version !== 1 || envelope.session_id !== owner || typeof envelope.sequence !== 'string' || !/^[1-9][0-9]*$/.test(envelope.sequence))
      throw new Error('Invalid canonical session envelope');
    let state = irSessions.current[owner] ?? emptySession(owner);
    if (BigInt(envelope.sequence) <= BigInt(state.sequence)) return;
    const pending = irPending.current[owner] ?? new Map<string, Envelope>();
    const previous = pending.get(envelope.sequence);
    if (previous && previous.event_id !== envelope.event_id) throw new Error('Conflicting session sequence');
    pending.set(envelope.sequence, envelope);
    if (pending.size > 4096) throw new Error('Session replay gap exceeds buffer limit');
    for (;;) {
      const next = String(BigInt(state.sequence) + 1n);
      const event = pending.get(next);
      if (!event) break;
      state = reduceSession(state, event); pending.delete(next);
    }
    irPending.current[owner] = pending;
    irSessions.current[owner] = state;
    if (irFlush.current === null) irFlush.current = window.setTimeout(() => {
      irFlush.current = null; setIRRevision(n => n + 1);
    }, 50);
  }
  useEffect(() => () => { if (irFlush.current !== null) window.clearTimeout(irFlush.current); }, []);

  // Work (in-flight/queued sends + iteration progress) keyed by the owning tab's
  // aimeeSid, so the busy indicator reflects the ACTIVE tab alone — sending on one
  // tab no longer shows "working" on every other tab. Derived values below.
  const [workBySid, setWorkBySid] = useState<Record<string, SidWork>>({});
  const activeWork = workBySid[tabs[activeIdx]?.aimeeSid ?? ''];
  const working = !!activeWork && (activeWork.pending > 0 || activeWork.queued > 0);
  const iterCur = activeWork?.iterCur ?? 0;
  const iterMax = activeWork?.iterMax ?? 0;
  const [inputText, setInputText] = useState('');
  const [banner, setBanner] = useState<{ stacks: BootstrapStack[] } | null>(null);
  const [rulesOpen, setRulesOpen] = useState(false);
  const [pluginsOpen, setPluginsOpen] = useState(false);
  const [contextOpen, setContextOpen] = useState(false);
  const [sessionUsage, setSessionUsage] = useState<{ in: number; out: number; cost: number; kind: string }>({ in: 0, out: 0, cost: 0, kind: 'realized' });
  const [allTimeMetrics, setAllTimeMetrics] = useState<MetricRow[]>([]);
  const [promptTier, setPromptTier] = useState<'MINIMAL' | 'STANDARD' | 'EXTENDED'>('STANDARD');
  // Driven solely by the active session's project (the single ProjectPicker);
  // never seeded from a stale global value, so we can't send an out-of-workspace cwd.
  const [projectRoot, setProjectRoot] = useState('');
  // The top session tabs are the source of truth. Each conversation tab mirrors
  // a session 1:1 (by sessionId), so every session keeps its own history; the
  // active session also drives the project (cwd).
  const sessionProject = activeSession?.projectRoot ?? '';

  // Keep one conversation tab per account-scoped session. Match both the UI id
  // and stable aimee id so legacy browser-only tabs migrate without duplication.
  useEffect(() => {
    if (!sessions.length) return;
    setTabs(prev => {
      const byId = new Map(prev.filter(t => t.sessionId).map(t => [t.sessionId as string, t]));
      const byAimeeId = new Map(prev.filter(t => t.aimeeSid).map(t => [t.aimeeSid, t]));
      const untagged = prev.filter(t => !t.sessionId);
      let u = 0;
      const next = sessions.map((s, i) => {
        const existing = byId.get(s.id) ?? byAimeeId.get(s.aimeeSid);
        if (existing) {
          const messages = existing.aimeeSid === s.aimeeSid
            ? irSessions.current[s.aimeeSid]?.messages.length ? existing.messages
              : reconcileSessionMessages(existing.messages, s.messages) : s.messages;
          if (existing.sessionId === s.id && existing.title === s.name &&
              existing.aimeeSid === s.aimeeSid && messages === existing.messages &&
              (!s.claudeSid || existing.sid === s.claudeSid)) return existing;
          return {
            ...existing,
            sessionId: s.id,
            title: s.name,
            messages,
            sid: s.claudeSid || existing.sid,
            aimeeSid: s.aimeeSid,
          };
        }
        const adopt = untagged[u++];
        if (adopt) return {
          ...adopt,
          sessionId: s.id,
          title: s.name,
          messages: reconcileSessionMessages(adopt.messages, s.messages),
          sid: s.claudeSid || adopt.sid,
          aimeeSid: s.aimeeSid,
        };
        return normalizeTab({
          sessionId: s.id,
          title: s.name,
          messages: s.messages,
          sid: s.claudeSid,
          aimeeSid: s.aimeeSid,
        }, i);
      });
      // No-op if unchanged (avoid a render loop).
      if (next.length === prev.length && next.every((t, i) => t === prev[i])) return prev;
      tabsRef.current = next;
      return next;
    });
  }, [sessions]);

  // Mirror the active session's project into the chat cwd (no conversation reset
  // — switching sessions preserves each one's history).
  useEffect(() => {
    setProjectRoot(sessionProject);
    saveProjectRoot(sessionProject);
  }, [sessionProject]);
  const [availableSkills, setAvailableSkills] = useState<string[]>([]);
  const [activeSkill, setActiveSkill] = useState<string>('');
  const [availablePersonas, setAvailablePersonas] = useState<PersonaInfo[]>([]);
  /* The agent serving this tab's turns. `activeAgent` is the SESSION PIN (empty
   * = not pinned); `defaultAgent` is what an unpinned turn actually lands on,
   * reported by /api/models (server-side agent_default_primary). The selector
   * shows the pin when there is one, else the default, so it always names the
   * agent you are really talking to. */
  const [defaultAgent, setDefaultAgent] = useState<string>('');
  const [lspDiag, setLspDiag] = useState<{ errors: number; warnings: number; active_servers: number } | null>(null);
  const [workflowInfo, setWorkflowInfo] = useState<WorkflowSessionInfo | null>(null);
  const [workflowLoading, setWorkflowLoading] = useState(false);
  const [workflowError, setWorkflowError] = useState<string | null>(null);
  const [workflowChanged, setWorkflowChanged] = useState(false);
  const [plugins, setPlugins] = useState<PluginInfo[]>([]);
  const [pluginLoaderAvailable, setPluginLoaderAvailable] = useState(false);
  const [pluginsLoading, setPluginsLoading] = useState(false);
  const [pluginsError, setPluginsError] = useState<string | null>(null);
  const [agents, setAgents] = useState<ModelInfo[]>([]);
  const [branchingSupported, setBranchingSupported] = useState(false);
  const [threadError, setThreadError] = useState('');

  /* Channel state */
  const [channels, setChannels] = useState<ChannelInfo[]>([]);
  const [activeChannel, setActiveChannel] = useState<string | null>(loadActiveChannel);
  const [channelsOpen, setChannelsOpen] = useState(() => !!loadActiveChannel());
  const [channelMsgs, setChannelMsgs] = useState<Record<string, ChannelMessage[]>>({});
  const [unreadCounts, setUnreadCounts] = useState<Record<string, number>>({});
  const channelSseRef = useRef<EventSource | null>(null);
  const presenceSseRef = useRef<EventSource | null>(null);

  const messagesEndRef = useRef<HTMLDivElement>(null);
  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const workflowUpdatedAtRef = useRef<string | null>(null);
  const skipNextStreamPersistRef = useRef(false);
  const streamPersistTimerRef = useRef<number | null>(null);
  // Controller → originSid, so a finishing send can decrement work for the tab it
  // belonged to (not whatever tab is active when it lands).
  const activeSendAbortRefs = useRef<Map<AbortController, string>>(new Map());
  /* Per-tab send queues + drainers, keyed by aimeeSid. Each session drains
   * independently and concurrently, so a turn on one tab never blocks another
   * tab's turn (the server already multiplexes sessions: per-session pools,
   * presence locks and tmux panes). Sends stay serial WITHIN a session — a
   * single conversation pane can only run one turn at a time. */
  const sendQueuesRef = useRef<Map<string, QueuedChatSend[]>>(new Map());
  const sendDrainingRef = useRef<Set<string>>(new Set());
  const sendQueueVersionRef = useRef(0);
  /* Append to a session's queue, creating it on first use. */
  function pushToSendQueue(sid: string, item: QueuedChatSend): void {
    let q = sendQueuesRef.current.get(sid);
    if (!q) {
      q = [];
      sendQueuesRef.current.set(sid, q);
    }
    q.push(item);
  }
  const tabsRef = useRef<TabData[]>(tabs);
  const activeIdxRef = useRef(activeIdx);
  // Holds the aimeeSid we just sent a steer for (chat.interrupt). The next
  // turn_started on THAT session's events stream is the server-initiated
  // continuation — render it even though this surface may still look "working"
  // while the cancelled turn's stream closes (otherwise the wasWorking self-echo
  // guard would drop the steered reply). Keyed by sid so switching tabs can't
  // make a different tab's turn consume it. '' = none.
  const expectSteerRef = useRef<string>('');
  // turn_ids already committed to history from the presence ring this session.
  // The ring replays turn_started→deltas→turn_done from cursor 0 on EVERY new
  // subscription (reconnect / tab switch back), so dedup is required to avoid
  // re-appending the same observed turn. Survives effect re-runs (component ref).
  /* Stream-token batching: text/thinking deltas arrive far faster than the
   * screen refreshes. Rather than a setStreamMsgs (→ full re-render) per token,
   * we accumulate deltas here and flush them in a single state update per
   * 100 ms timer tick. Message
   * *creation* stays synchronous so ids/order are assigned immediately; only the
   * append of further text into an existing bubble is batched. */
  const pendingAppendsRef = useRef<Array<{ owner: string; id: number; field: 'text' | 'thinkText'; delta: string }>>([]);
  const flushTimerRef = useRef<number | null>(null);
  /* Live mirror of `streamMsgs` for synchronous reads (effects/switch logic). */
  const streamMsgsRef = useRef<StreamMsg[]>(streamMsgs);
  /* Off-screen live transcript for every NON-active tab keyed by its aimeeSid.
   * A stream is routed to its OWNING tab's buffer (by streamRefs.originSid), so a
   * turn that is in flight while the user is on another tab accumulates here
   * instead of bleeding into — and being saved onto — the active tab. `streamMsgs`
   * stays strictly the active tab's view; the switch effect swaps buffers in/out. */
  const bgStreamsRef = useRef<Map<string, StreamMsg[]>>(new Map());
  /* aimeeSid that `streamMsgs` currently represents (the tab whose buffer is
   * on-screen). '' until the first load so the initial committed history loads. */
  const renderedSidRef = useRef<string>('');
  const queueActive = working;
  const activePersonaSid = tabs[activeIdx]?.aimeeSid ?? '';
  const persona = useSessionPreference(activePersonaSid, '/api/chat/persona', 'name');
  const primary = useSessionPreference(activePersonaSid, '/api/chat/primary', 'agent');
  const activePersona = persona.value;
  const activeAgent = primary.value;

  /* Auto-scroll. Instant, not smooth: this runs ~10×/s during a stream, and a
   * smooth scrollIntoView restarts an animated scroll on every call — the
   * animation never settles, so it pins the main thread/compositor for the
   * whole turn (a sustained CPU burn the re-render throttle can't touch).
   * Instant scroll is effectively free. We also skip if the user has scrolled
   * up to read back, so a live stream doesn't yank them to the bottom. */
  const atBottomRef = useRef(true);
  const handleMessagesScroll = useCallback((e: React.UIEvent<HTMLDivElement>) => {
    const el = e.currentTarget;
    atBottomRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 80;
  }, []);
  const scrollToBottom = useCallback(() => {
    if (!atBottomRef.current) return;
    messagesEndRef.current?.scrollIntoView({ behavior: 'auto', block: 'end' });
  }, []);

  // How many trailing messages the transcript currently renders. Full history
  // stays in streamMsgs (and persisted); we just don't mount/reconcile all of it.
  // "Show earlier" grows the window; switching conversations resets it so a fresh
  // tab opens windowed, not expanded from a previous one.
  const [visibleCount, setVisibleCount] = useState(TRANSCRIPT_WINDOW);
  useEffect(() => { setVisibleCount(TRANSCRIPT_WINDOW); }, [activePersonaSid]);
  const hiddenLegacyCount = Math.max(0, streamMsgs.length - visibleCount);
  const windowedMsgs = hiddenLegacyCount > 0 ? streamMsgs.slice(hiddenLegacyCount) : streamMsgs;

  useEffect(() => { scrollToBottom(); }, [streamMsgs, working, scrollToBottom]);
  useEffect(() => { streamMsgsRef.current = streamMsgs; }, [streamMsgs]);
  useEffect(() => { tabsRef.current = tabs; }, [tabs]);
  /* Drop off-screen stream buffers for tabs that have been closed, so they don't
   * accumulate across open/close cycles. */
  useEffect(() => {
    const live = new Set(tabs.map(t => t.aimeeSid));
    for (const sid of bgStreamsRef.current.keys()) {
      if (!live.has(sid)) bgStreamsRef.current.delete(sid);
    }
  }, [tabs]);
  // Cancel any pending stream-flush timer on unmount.
  useEffect(() => () => { if (flushTimerRef.current !== null) window.clearTimeout(flushTimerRef.current); }, []);

  /* Detach every attached presence surface when the page unloads, so a closed
   * browser doesn't leak attachments until the session is closed. keepalive
   * lets the POSTs survive the unload; best-effort. */
  useEffect(() => {
    const detachAll = () => {
      for (const tab of tabsRef.current) {
        if (!tab.attachId || !tab.aimeeSid) continue;
        try {
          fetch('/api/chat/detach', {
            method: 'POST',
            keepalive: true,
            headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
            body: JSON.stringify({ sid: tab.aimeeSid, attach_id: tab.attachId }),
          }).catch(() => {});
        } catch { /* ignore */ }
      }
    };
    // Always flush the coalesced tabs write when the page goes away or is hidden
    // — NOT just on beforeunload. bfcache navigation (back/forward) and OS
    // background-kill on mobile don't fire beforeunload. Keeping this fast cache
    // current also avoids waiting for a server refresh after bfcache navigation.
    // visibilitychange→hidden catches a backgrounded tab before a kill.
    // Detach presence only on a GENUINE unload: a tab going to the background — or
    // entering bfcache (pagehide persisted=true, restored without re-mounting) —
    // is still an active session and must stay attached.
    const onBeforeUnload = () => { flushPendingTabs(); detachAll(); };
    const onPageHide = (e: PageTransitionEvent) => { flushPendingTabs(); if (!e.persisted) detachAll(); };
    const onVisibility = () => { if (document.visibilityState === 'hidden') flushPendingTabs(); };
    window.addEventListener('beforeunload', onBeforeUnload);
    window.addEventListener('pagehide', onPageHide);
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      window.removeEventListener('beforeunload', onBeforeUnload);
      window.removeEventListener('pagehide', onPageHide);
      document.removeEventListener('visibilitychange', onVisibility);
    };
  }, []);

  /* Subscribe to the active session's unified-presence event stream so the UI
   * can show when aimee is responding on another surface (CLI/TUI, another
   * tab). Only meaningful once the tab has attached (presence exists). */
  const activeAimeeSid = tabs[activeIdx]?.aimeeSid ?? '';
  useEffect(() => {
    if (!activeAimeeSid) return;
    let disposed = false;
    setIRReady(prev => ({ ...prev, [activeAimeeSid]: false }));
    const state = irSessions.current[activeAimeeSid] ?? emptySession(activeAimeeSid);
    const es = new EventSource(`/api/chat/session-events?format=ir&sid=${encodeURIComponent(activeAimeeSid)}&cursor=${state.sequence}`);
    presenceSseRef.current = es;
    es.addEventListener('error', () => {
      if (!disposed) setIRReady(prev => ({ ...prev, [activeAimeeSid]: false }));
    });
    es.addEventListener('replay_complete', (event: MessageEvent<string>) => {
      if (disposed) return;
      try {
        const ready = parseJSON(event.data) as { session_id?: string; cursor?: string };
        const cursor = irSessions.current[activeAimeeSid]?.sequence ?? '0';
        if (ready.session_id !== activeAimeeSid || typeof ready.cursor !== 'string' ||
            !/^(0|[1-9][0-9]*)$/.test(ready.cursor) || BigInt(ready.cursor) > BigInt(cursor))
          throw new Error('Invalid conversation replay boundary');
        setIRReady(prev => prev[activeAimeeSid] ? prev : { ...prev, [activeAimeeSid]: true });
        setIRTransportErrors(prev => prev[activeAimeeSid] ? { ...prev, [activeAimeeSid]: '' } : prev);
      } catch (error) {
        disposed = true;
        setIRReady(prev => ({ ...prev, [activeAimeeSid]: false }));
        setIRTransportErrors(prev => ({ ...prev, [activeAimeeSid]: error instanceof Error ? error.message : 'Session replay failed' }));
        es.close();
      }
    });
    es.addEventListener('ir', (event: MessageEvent<string>) => {
      if (disposed) return;
      try {
        ingestIR(activeAimeeSid, parseJSON(event.data) as Record<string, unknown>);
        setIRTransportErrors(prev => prev[activeAimeeSid] ? { ...prev, [activeAimeeSid]: '' } : prev);
      } catch (error) {
        disposed = true;
        setIRReady(prev => ({ ...prev, [activeAimeeSid]: false }));
        setIRTransportErrors(prev => ({ ...prev, [activeAimeeSid]: error instanceof Error ? error.message : 'Session replay failed' }));
        es.close();
      }
    });
    return () => { disposed = true; es.close(); if (presenceSseRef.current === es) presenceSseRef.current = null; };
  }, [activeAimeeSid, irReconnect]);
  const canonicalState = irSessions.current[activeAimeeSid];
  // Execution activity is a canonical projection, not separately copied UI state.
  // Keeping a second state/effect pair can lag a replayed running turn.
  const remoteTurnActive = !!irReady[activeAimeeSid] && !!canonicalState?.activeTurns.length;
  const hiddenCount = canonicalState?.messages.length
    ? Math.max(0, canonicalState.messages.length - visibleCount) : hiddenLegacyCount;
  const submittedControls = useRef(new Set<string>());
  const [pendingReplies, setPendingReplies] = useState<Record<string, ReadonlySet<string>>>({});
  const replyControl = useCallback(async (requestId: string, reply: ControlReply, sessionId = activeAimeeSid) => {
    const key = `${sessionId}\0${requestId}`;
    if (submittedControls.current.has(key)) return;
    submittedControls.current.add(key);
    setPendingReplies(prev => ({ ...prev, [sessionId]: new Set([...(prev[sessionId] ?? []), requestId]) }));
    try {
      const response = await fetch('/api/chat/control-reply', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: stringifyJSON({ session_id: sessionId, request_id: requestId, reply }),
      });
      const body = await response.json().catch(() => ({})) as { error?: string };
      if (!response.ok) throw new Error(body.error || 'Could not send the reply');
    } catch (error) {
      submittedControls.current.delete(key);
      setPendingReplies(prev => {
        const remaining = new Set(prev[sessionId]); remaining.delete(requestId);
        return { ...prev, [sessionId]: remaining };
      });
      throw error;
    }
  }, [activeAimeeSid]);

  const usage = canonicalState?.messages.slice().reverse().find(m => m.usage)?.usage;
  const lastUsage = usage ? { in: usage.input, out: usage.output, cost: 0 } : null;
  useEffect(() => {
    scrollToBottom();
  }, [canonicalState, irReady, activeAimeeSid, scrollToBottom]);
  useEffect(() => { activeIdxRef.current = activeIdx; }, [activeIdx]);

  /* Rebuild per-sid pending/queued counts from the live send refs. Iteration
   * progress is cleared for any sid that has no more in-flight or queued work, and
   * sids with no work at all are dropped so the map only holds active sessions. */
  function recomputeWorkCounts(): void {
    const pending = new Map<string, number>();
    activeSendAbortRefs.current.forEach(sid => pending.set(sid, (pending.get(sid) ?? 0) + 1));
    const queued = new Map<string, number>();
    for (const q of sendQueuesRef.current.values())
      for (const it of q) queued.set(it.originSid, (queued.get(it.originSid) ?? 0) + 1);
    setWorkBySid(prev => {
      const next: Record<string, SidWork> = {};
      const sids = new Set<string>([...Object.keys(prev), ...pending.keys(), ...queued.keys()]);
      for (const sid of sids) {
        const p = pending.get(sid) ?? 0;
        const q = queued.get(sid) ?? 0;
        const active = p > 0 || q > 0;
        const iterCur = active ? (prev[sid]?.iterCur ?? 0) : 0;
        const iterMax = active ? (prev[sid]?.iterMax ?? 0) : 0;
        if (!active && iterCur === 0 && iterMax === 0) continue;
        next[sid] = { pending: p, queued: q, iterCur, iterMax };
      }
      // Bail if nothing changed: this runs on every drained item, and returning a
      // fresh object each time forces a needless whole-Chat re-render.
      const pk = Object.keys(prev);
      const nk = Object.keys(next);
      if (pk.length === nk.length && pk.every(k => {
        const a = prev[k], b = next[k];
        return b && a.pending === b.pending && a.queued === b.queued &&
          a.iterCur === b.iterCur && a.iterMax === b.iterMax;
      })) return prev;
      return next;
    });
  }

  function abortActiveSends(): void {
    sendQueueVersionRef.current++;
    sendQueuesRef.current.clear();
    /* Do NOT clear sendDrainingRef here: a drainer mid-`await sendQueuedMessage`
     * still owns its sid and will self-remove in its own finally. Clearing it
     * would let a re-enqueue (same sid, post-abort) start a SECOND drainer while
     * the first is still unwinding — two concurrent turns on one pane, which the
     * server rejects with presence_busy. The version bump above + the cleared
     * queues make the in-flight drainer a no-op as it unwinds. */
    activeSendAbortRefs.current.forEach((_sid, controller) => controller.abort());
    activeSendAbortRefs.current.clear();
    setWorkBySid({});
  }

  useEffect(() => {
    return () => {
      saveOwnerStream(renderedSidRef.current);
      for (const owner of bgStreamsRef.current.keys()) saveOwnerStream(owner);
      saveTabs(tabsRef.current);
      flushPendingTabs();
      abortActiveSends();
    };
  }, []);

  /* Tab persistence */
  useEffect(() => {
    saveTabs(tabs);
  }, [tabs]);

  /* SessionContext restores and refreshes the authenticated user's server-side
   * session list; the reconciliation effect above maps it onto chat tabs. */

  useEffect(() => {
    saveActiveTabIndex(activeIdx);
  }, [activeIdx]);

  /* Bootstrap banner */
  useEffect(() => {
    const qs = projectRoot ? `?cwd=${encodeURIComponent(projectRoot)}` : '';
    fetch(`/api/chat/bootstrap-status${qs}`)
      .then(r => r.json())
      .then((bs: BootstrapStatus) => {
        if (!bs.has_rules && !loadRulesBannerDismissed(projectRoot)) {
          setBanner({ stacks: bs.stacks ?? [] });
        } else {
          setBanner(null);
          if (bs.has_rules) saveRulesBannerDismissed(false, projectRoot);
        }
      })
      .catch(() => {});
  }, [projectRoot]);

  /* The chat cwd is driven SOLELY by the active session's project (set via the
   * single ProjectPicker). It is never defaulted to the server's own cwd — that
   * is outside the webuser's workspace and the server rejects it ("project must
   * be within your workspace"). An unset project sends an empty cwd, which the
   * webchat resolves to the user's workspace root. */

  /* Load initial prompt tier from server */
  useEffect(() => {
    fetch('/api/chat/prompt-tier')
      .then(r => r.json())
      .then((d: { prompt_tier?: string }) => {
        const t = d.prompt_tier;
        if (t === 'MINIMAL' || t === 'STANDARD' || t === 'EXTENDED') setPromptTier(t);
      })
      .catch(() => {});
  }, []);

  /* Load available skills from server */
  useEffect(() => {
    fetch('/api/chat/skills')
      .then(r => r.json())
      .then((d: { skills?: string[]; active?: string }) => {
        setAvailableSkills(d.skills ?? []);
        setActiveSkill(d.active ?? '');
      })
      .catch(() => {});
  }, []);

  /* Load the persona list once (for the selector) */
  useEffect(() => {
    fetch('/api/chat/personas')
      .then(r => r.json())
      .then((d: { personas?: PersonaInfo[] }) => setAvailablePersonas(d.personas ?? []))
      .catch(() => {});
  }, []);

  const refreshPlugins = useCallback(async () => {
    setPluginsLoading(true);
    try {
      const resp = await fetch('/api/plugins');
      if (!resp.ok) {
        setPluginLoaderAvailable(false);
        setPlugins([]);
        setPluginsError(null);
        return;
      }
      const data = await resp.json() as PluginInfo[];
      setPluginLoaderAvailable(true);
      setPlugins(Array.isArray(data) ? data : []);
      setPluginsError(null);
    } catch {
      setPluginLoaderAvailable(false);
      setPlugins([]);
      setPluginsError('Failed to load plugins');
    } finally {
      setPluginsLoading(false);
    }
  }, []);

  useEffect(() => {
    void refreshPlugins();
  }, [refreshPlugins]);

  useEffect(() => {
    if (pluginsOpen) void refreshPlugins();
  }, [pluginsOpen, refreshPlugins]);

  // Unsupported controls stay hidden until the backend advertises their capability.
  useEffect(() => {
    let disposed = false;
    void fetch('/api/chat/threads').then(response => response.ok ? response.json() : {})
      .then((body: { capabilities?: { branching?: boolean } }) => {
        if (!disposed) setBranchingSupported(body.capabilities?.branching === true);
      }).catch(() => {});
    return () => { disposed = true; };
  }, []);

  /* Fetch channel list on mount */
  useEffect(() => {
    fetch('/api/channels')
      .then(r => r.json())
      .then((d: { channels?: ChannelInfo[] }) => setChannels(d.channels ?? []))
      .catch(() => {});
  }, []);

  useEffect(() => {
    fetch('/api/models')
      .then(r => r.json())
      .then((d: { models?: ModelInfo[]; agents?: ModelInfo[]; default_agent?: string }) => {
        setAgents(d.models ?? d.agents ?? []);
        setDefaultAgent(d.default_agent ?? '');
      })
      .catch(() => {});
  }, []);

  useEffect(() => {
    fetch('/api/metrics')
      .then(r => r.json())
      .then((d: MetricRow[]) => { if (Array.isArray(d)) setAllTimeMetrics(d); })
      .catch(() => {});
  }, []);

  useEffect(() => {
    setSessionUsage({ in: 0, out: 0, cost: 0, kind: 'realized' });
  }, [activeIdx]);

  /* Persist active channel to localStorage */
  useEffect(() => { saveActiveChannel(activeChannel); }, [activeChannel]);

  /* SSE subscription for active channel */
  useEffect(() => {
    if (channelSseRef.current) { channelSseRef.current.close(); channelSseRef.current = null; }
    if (!activeChannel) return;

    let disposed = false;
    const abort = new AbortController();
    const refresh = () => {
      void fetch(`/api/channels/${encodeURIComponent(activeChannel)}/messages`, { signal: abort.signal })
        .then(r => { if (!r.ok) throw new Error('Channel history unavailable'); return r.json(); })
        .then((d: { messages?: ChannelMessage[] }) => {
          if (disposed) return;
          setChannelMsgs(prev => ({ ...prev, [activeChannel]: mergeChannelMessages(d.messages ?? [], prev[activeChannel] ?? []) }));
          setUnreadCounts(prev => ({ ...prev, [activeChannel]: 0 }));
        }).catch(() => {});
    };
    const es = new EventSource(`/api/channels/${encodeURIComponent(activeChannel)}/events`);
    channelSseRef.current = es;
    // Refresh after every successful connection, including reconnects. Merge
    // the snapshot with live arrivals instead of replacing the live timeline.
    es.addEventListener('open', refresh);
    refresh();
    es.addEventListener('message', (ev: MessageEvent<string>) => {
      if (disposed) return;
      try {
        const msg = JSON.parse(ev.data) as ChannelMessage;
        if (typeof msg.sender !== 'string' || typeof msg.text !== 'string') return;
        setChannelMsgs(prev => ({ ...prev, [activeChannel]: mergeChannelMessages(prev[activeChannel] ?? [], [msg]) }));
      } catch { /* ignore malformed frames */ }
    });
    return () => { disposed = true; abort.abort(); es.close(); channelSseRef.current = null; };
  }, [activeChannel]); // eslint-disable-line react-hooks/exhaustive-deps

  function selectChannel(name: string): void {
    abortActiveSends();
    setActiveChannel(name);
    setChannelsOpen(false);
    setUnreadCounts(prev => ({ ...prev, [name]: 0 }));
    /* Ensure channel exists in message map */
    setChannelMsgs(prev => prev[name] ? prev : { ...prev, [name]: [] });
  }

  async function createChannel(name: string): Promise<void> {
    try {
      await fetch('/api/channels', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ name }),
      });
      const d = await fetch('/api/channels').then(r => r.json()) as { channels?: ChannelInfo[] };
      setChannels(d.channels ?? []);
      selectChannel(name);
    } catch { /* ignore */ }
  }

  async function sendChannelMessage(text: string): Promise<void> {
    if (!activeChannel) return;
    try {
      await fetch(`/api/channels/${encodeURIComponent(activeChannel)}/messages`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ speaker: 'user', text }),
      });
    } catch { /* ignore */ }
  }

  async function changePromptTier(tier: 'MINIMAL' | 'STANDARD' | 'EXTENDED') {
    try {
      await fetch('/api/chat/prompt-tier', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ tier }),
      });
      setPromptTier(tier);
    } catch { /* ignore */ }
  }

  async function changeSkill(skill: string) {
    try {
      await fetch('/api/chat/skill', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ skill }),
      });
      setActiveSkill(skill);
    } catch { /* ignore */ }
  }

  async function togglePlugin(name: string) {
    try {
      const resp = await fetch(`/api/plugins/${encodeURIComponent(name)}/toggle`, {
        method: 'POST',
        headers: { 'X-CSRF-Token': window._csrf || '' },
      });
      const data = await resp.json() as PluginInfo & { error?: string };
      if (!resp.ok || data.error) {
        setPluginsError(data.error ?? 'Toggle failed');
        return;
      }
      setPlugins(prev => prev.map(plugin => plugin.name === data.name ? data : plugin));
      setPluginsError(null);
    } catch {
      setPluginsError('Failed to toggle plugin');
    }
  }

  /* Swap the on-screen transcript when the active tab changes. The leaving tab's
   * live buffer (possibly mid-stream) is stashed off-screen so its stream keeps
   * accumulating against its OWN history instead of the newly-active tab; the
   * entering tab is restored from its stashed buffer if one exists (in-flight or
   * recently streamed, keeping rich tool/thinking blocks), else loaded fresh from
   * committed history. */
  useEffect(() => {
    const newSid = tabs[activeIdx]?.aimeeSid ?? '';
    const oldSid = renderedSidRef.current;
    if (oldSid === newSid) return;
    if (oldSid) {
      bgStreamsRef.current.set(oldSid, streamMsgsRef.current);
      saveTabMessages(oldSid, streamMsgsRef.current);
    }
    renderedSidRef.current = newSid;
    skipNextStreamPersistRef.current = true;
    const buffered = bgStreamsRef.current.get(newSid);
    if (buffered) {
      // Take ownership: the buffer is now on-screen (in streamMsgs), so drop the
      // off-screen copy to avoid a stale duplicate on the next switch-out stash.
      bgStreamsRef.current.delete(newSid);
      streamMsgsRef.current = buffered;
      setStreamMsgs(buffered);
      return;
    }
    const tab = tabs[activeIdx];
    const msgs: StreamMsg[] = tab
      ? tab.messages.map(m => ({ id: nextId(), type: m.role, text: m.text }))
      : [];
    streamMsgsRef.current = msgs;
    setStreamMsgs(msgs);
  }, [activePersonaSid]); // eslint-disable-line react-hooks/exhaustive-deps

  // A server refresh can hydrate the currently selected session without
  // changing its tab index (the normal fresh-browser case). Load that account-
  // scoped transcript directly; later local stream updates do not retrigger this
  // effect because SessionContext's server snapshot is unchanged.
  useEffect(() => {
    // Wait until the selected session has a matching tab before hydrating it.
    if (tabs[activeIdx]?.sessionId !== activeSessionId) return;
    const serverMessages = activeSession?.messages;
    if (!serverMessages) return;
    const tab = tabsRef.current.find(candidate => candidate.sessionId === activeSessionId);
    if (!tab || tab.aimeeSid !== renderedSidRef.current || irSessions.current[tab.aimeeSid]?.messages.length) return;
    // A live reply owns its message ids. Replacing it with a snapshot would
    // orphan the stream's ids, causing later deltas to lose their target.
    for (const owner of activeSendAbortRefs.current.values()) {
      if (owner === tab.aimeeSid) return;
    }
    const live = streamToTabMessages(streamMsgsRef.current);
    if (reconcileSessionMessages(live, serverMessages) === live) return;
    const hydrated = serverMessages.map(message => ({
      id: nextId(), type: message.role, text: message.text,
    }));
    streamMsgsRef.current = hydrated;
    renderedSidRef.current = tab.aimeeSid;
    setStreamMsgs(hydrated);
  }, [activeSessionId, activeSession?.messages, tabs[activeIdx]?.sessionId]);

  /* Commit by stable id: a refresh may reorder tabs while a timer is pending. */
  const saveTabMessages = useCallback((owner: string, msgs: StreamMsg[]) => {
    const tab = tabsRef.current.find(candidate => candidate.aimeeSid === owner);
    if (!tab) return;
    const saved = reconcileSessionMessages(tab.messages, streamToTabMessages(msgs));
    if (sameTabMessages(tab.messages, saved)) return;
    const next = tabsRef.current.map(candidate => candidate === tab ? { ...tab, messages: saved } : candidate);
    tabsRef.current = next;
    setTabs(next);
    if (tab.sessionId) patchSession(tab.sessionId, { messages: saved });
  }, [patchSession]);

  /* Route updates to the buffer actually on screen, including the interval
   * between selecting a session and swapping its transcript. Keep refs current
   * synchronously so final commits cannot save an older React render. */
  const applyToOwnerStream = useCallback((owner: string, updater: (prev: StreamMsg[]) => StreamMsg[]) => {
    if (owner === renderedSidRef.current) {
      const next = updater(streamMsgsRef.current);
      streamMsgsRef.current = next;
      setStreamMsgs(next);
    } else {
      bgStreamsRef.current.set(owner, updater(bgStreamsRef.current.get(owner) ?? []));
    }
  }, []);

  const saveOwnerStream = useCallback((owner: string) => {
    flushStreamAppends();
    saveTabMessages(owner, owner === renderedSidRef.current
      ? streamMsgsRef.current : bgStreamsRef.current.get(owner) ?? []);
  }, [saveTabMessages]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (skipNextStreamPersistRef.current) {
      skipNextStreamPersistRef.current = false;
      return;
    }

    const owner = renderedSidRef.current;
    if (streamPersistTimerRef.current !== null) {
      window.clearTimeout(streamPersistTimerRef.current);
    }
    streamPersistTimerRef.current = window.setTimeout(() => {
      streamPersistTimerRef.current = null;
      saveTabMessages(owner, streamMsgs);
    }, STREAM_PERSIST_DEBOUNCE_MS);

    return () => {
      if (streamPersistTimerRef.current !== null) {
        window.clearTimeout(streamPersistTimerRef.current);
        streamPersistTimerRef.current = null;
      }
    };
  }, [streamMsgs, activePersonaSid, saveTabMessages]);

  const workflowChannel = ((tabs[activeIdx]?.workflowChannel ?? '').trim()
    || (tabs[activeIdx]?.title ?? '').trim()
    || 'general');

  const refreshWorkflow = useCallback(async () => {
    const channel = workflowChannel.trim();
    if (!channel) {
      setWorkflowInfo(null);
      setWorkflowError(null);
      setWorkflowLoading(false);
      workflowUpdatedAtRef.current = null;
      return;
    }

    setWorkflowLoading(true);
    try {
      const resp = await fetch(`/api/sessions/workflows/channel/${encodeURIComponent(channel)}`);
      if (resp.status === 404) {
        setWorkflowInfo(null);
        setWorkflowError(null);
        workflowUpdatedAtRef.current = null;
        return;
      }
      if (resp.status === 502 || resp.status === 503 || resp.status === 504) {
        setWorkflowInfo(null);
        setWorkflowError(null);
        workflowUpdatedAtRef.current = null;
        return;
      }
      if (!resp.ok) {
        const text = await resp.text();
        let msg = `Workflow state unavailable (${resp.status})`;
        try {
          const parsed = JSON.parse(text) as { error?: string };
          if (parsed.error) msg = parsed.error;
        } catch { /* ignore */ }
        setWorkflowInfo(null);
        setWorkflowError(msg);
        return;
      }
      const data = await resp.json() as WorkflowSessionInfo & { error?: string };
      if (data.error) {
        if (data.error.includes('not found')) {
          setWorkflowInfo(null);
          setWorkflowError(null);
          workflowUpdatedAtRef.current = null;
        } else {
          setWorkflowInfo(null);
          setWorkflowError(data.error);
        }
        return;
      }

      if (workflowUpdatedAtRef.current && workflowUpdatedAtRef.current !== data.updated_at) {
        setWorkflowChanged(true);
      }
      workflowUpdatedAtRef.current = data.updated_at;
      setWorkflowInfo(data);
      setWorkflowError(null);
    } catch {
      setWorkflowInfo(null);
      setWorkflowError(null);
      workflowUpdatedAtRef.current = null;
    } finally {
      setWorkflowLoading(false);
    }
  }, [workflowChannel]);

  useEffect(() => {
    void refreshWorkflow();
  }, [refreshWorkflow]);

  useEffect(() => {
    if (!workflowChanged) return;
    const timer = window.setTimeout(() => setWorkflowChanged(false), 4000);
    return () => window.clearTimeout(timer);
  }, [workflowChanged]);

  useEffect(() => {
    const timer = window.setInterval(() => {
      void refreshWorkflow();
    }, 10000);
    return () => window.clearInterval(timer);
  }, [refreshWorkflow]);

  function setCurrentWorkflowChannel(value: string) {
    setTabs(prev => {
      const next = [...prev];
      if (next[activeIdx]) next[activeIdx] = { ...next[activeIdx], workflowChannel: value };
      return next;
    });
  }


  /* Clear the current conversation: drop the visible transcript and mint a fresh
   * provider session (empty claude session id + new aimee session id) so the next
   * turn starts clean — no resume of a stale/gone session. */
  function clearChat() {
    const nextAimeeSid = newAimeeSessionId();
    setStreamMsgs([]);
    setTabs(prev => {
      const next = [...prev];
      if (next[activeIdx]) {
        next[activeIdx] = { ...next[activeIdx], sid: '', aimeeSid: nextAimeeSid, attachId: undefined, messages: [] };
      }
      return next;
    });
    if (activeSession) patchSession(activeSession.id, {
      claudeSid: '', aimeeSid: nextAimeeSid, attachId: '', messages: [],
    });
  }

  async function pauseWorkflow() {
    if (!workflowInfo) return;
    try {
      await fetch(`/api/sessions/workflows/${workflowInfo.id}/pause`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'X-CSRF-Token': window._csrf || '',
        },
        body: JSON.stringify({ reason: `Paused from webchat channel ${workflowChannel}` }),
      });
      await refreshWorkflow();
    } catch { /* ignore */ }
  }

  /* Thread operations */
  async function branchThread() {
    setThreadError('');
    try {
      const label = `branch-${(tabs[activeIdx]?.threads?.length ?? 0) + 1}`;
      const r = await fetch('/api/chat/branch', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ label }),
      });
      const d = await r.json() as { ok?: boolean; thread_id?: number; error?: string };
      if (!r.ok || !d.ok) throw new Error(d.error || 'Could not branch conversation');
      if (d.ok) {
        await refreshThreads();
      }
    } catch (error) { setThreadError(error instanceof Error ? error.message : 'Could not branch conversation'); }
  }

  async function switchThread(threadId: number) {
    try {
      const r = await fetch('/api/chat/switch-thread', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ thread_id: threadId }),
      });
      const d = await r.json() as { ok?: boolean; messages?: TabMessage[] };
      if (d.ok && d.messages) {
        const msgs: StreamMsg[] = d.messages.map(m => ({
          id: nextId(),
          type: m.role as 'user' | 'assistant',
          text: m.text,
        }));
        setStreamMsgs(msgs);
        setTabs(prev => {
          const next = [...prev];
          if (next[activeIdx]) {
            next[activeIdx] = {
              ...next[activeIdx],
              messages: d.messages!.map(m => ({ role: m.role, text: m.text })),
              activeThreadId: threadId,
            };
          }
          return next;
        });
        await refreshThreads();
      }
    } catch { /* ignore */ }
  }

  async function refreshThreads() {
    try {
      const r = await fetch('/api/chat/threads');
      const d = await r.json() as { threads: ThreadInfo[]; active_id: number };
      setTabs(prev => {
        const next = [...prev];
        if (next[activeIdx]) {
          next[activeIdx] = {
            ...next[activeIdx],
            threads: d.threads,
            activeThreadId: d.active_id,
          };
        }
        return next;
      });
    } catch { /* ignore */ }
  }

  /* Generate .aimee-rules */
  async function generateRules() {
    try {
      const r = await fetch('/api/chat/init-rules', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ cwd: projectRoot }),
      });
      const d = await r.json() as { ok?: boolean; status?: string; created?: boolean };
      if (r.ok && (d.ok || d.status === 'ok')) {
        saveRulesBannerDismissed(false, projectRoot);
        setBanner(null);
        setStreamMsgs(prev => [
          ...prev,
          { id: nextId(), type: 'assistant', text: d.created ? 'Created .aimee-rules.' : '.aimee-rules already exists.' },
        ]);
        return;
      }
      setStreamMsgs(prev => [
        ...prev,
        { id: nextId(), type: 'assistant', text: 'Failed to create .aimee-rules.' },
      ]);
    } catch {
      setStreamMsgs(prev => [
        ...prev,
        { id: nextId(), type: 'assistant', text: 'Failed to create .aimee-rules.' },
      ]);
    }
  }

  function dismissRulesBanner() {
    saveRulesBannerDismissed(true, projectRoot);
    setBanner(null);
  }

  /* Send message */
  async function sendMessage() {
    const text = inputText.trim();
    if (!text || primary.pending || persona.pending) return;

    setInputText('');
    // AutoGrowTextarea reflows its own height when inputText clears.

    // Steering: if a turn is already in flight for the active tab — a local send
    // (client stream open) or a server/foreign turn (events stream) — sending
    // INTERRUPTS the running turn and submits this message as the steer, which the
    // server auto-continues as the next turn (it arrives on the events stream).
    const sid = tabsRef.current[activeIdxRef.current]?.aimeeSid ?? '';
    let clientInflight = false;
    activeSendAbortRefs.current.forEach(s => { if (s === sid) clientInflight = true; });
    if (sid && (clientInflight || (!!irReady[sid] && !!irSessions.current[sid]?.activeTurns.length))) {
      // Optimistically show the steer in the transcript and re-stick to bottom.
      atBottomRef.current = true;
      // The admitted user message arrives through the canonical session log.
      void steerInterrupt(sid, text);
      return;
    }

    enqueueChatMessage(text);
  }

  // Stop the in-flight turn for `sid` and queue `text` as the steer continuation.
  // The server only honours the steer when a turn was actually in flight; on the
  // race where it just finished (interrupted:false), fall back to a normal send.
  async function steerInterrupt(sid: string, text: string) {
    const projectRoot = activeSession?.projectRoot ?? '';
    expectSteerRef.current = sid;
    // Safety net: if the server continuation never starts (dispatch failed), clear
    // the one-shot so it can't force-render a later turn on this session.
    window.setTimeout(() => { if (expectSteerRef.current === sid) expectSteerRef.current = ''; }, 12000);
    const sendNormally = () => {
      if (expectSteerRef.current === sid) expectSteerRef.current = '';
      pushToSendQueue(sid, { text, version: sendQueueVersionRef.current, originSid: sid, projectRoot });
      recomputeWorkCounts();
      void drainSendQueue(sid);
    };
    try {
      const resp = await fetch('/api/chat/interrupt', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ aimee_session_id: sid }),
      });
      if (resp.status === 401) { window.location.href = '/login'; return; }
      const j = resp.ok ? (await resp.json().catch(() => ({})) as { interrupted?: boolean }) : {};
      // No turn was actually in flight (or the steer couldn't be queued): send it
      // as an ordinary turn instead so the message is never lost.
      void j; sendNormally();
    } catch {
      sendNormally();
    }
  }

  function enqueueChatMessage(text: string) {
    /* Auto-title from first message */
    const idx = activeIdxRef.current;
    const activeSidForTitle = tabsRef.current[idx]?.aimeeSid ?? '';
    const hasExistingMessages = (tabsRef.current[idx]?.messages.length ?? 0) > 0 ||
      streamMsgs.length > 0 || activeSendAbortRefs.current.size > 0 ||
      (sendQueuesRef.current.get(activeSidForTitle)?.length ?? 0) > 0;
    if (!hasExistingMessages) {
      const title = text.substring(0, 30) + (text.length > 30 ? '…' : '');
      const sessionId = tabsRef.current[idx]?.sessionId;
      setTabs(prev => {
        const next = [...prev];
        if (next[idx]) next[idx] = { ...next[idx], title };
        tabsRef.current = next;
        return next;
      });
      if (sessionId) patchSession(sessionId, { name: title });
    }

    const userMsgId = nextId();
    // Sending is an explicit intent to follow the conversation: re-stick to the
    // bottom even if the user had scrolled up to read back.
    atBottomRef.current = true;
    void userMsgId; // canonical admission owns the user message

    const originSid = tabsRef.current[idx]?.aimeeSid ?? '';
    pushToSendQueue(originSid, { text, version: sendQueueVersionRef.current, originSid,
      projectRoot: activeSession?.projectRoot ?? '' });
    recomputeWorkCounts();
    void drainSendQueue(originSid);
  }

  /* Drain one session's queue. A drainer runs per aimeeSid: at most one per
   * session (so a session's turns stay ordered), but different sessions drain
   * concurrently — sending on one tab never waits on another tab's turn. */
  async function drainSendQueue(sid: string) {
    /* No `!sid` early-return: an empty key is a valid (degenerate) bucket, and
     * bailing here would strand a queued item forever (work spinner stuck on).
     * `aimeeSid` is always assigned (normalizeTab), so '' is not expected — but
     * if it ever occurs the message still drains rather than wedging. */
    if (sendDrainingRef.current.has(sid)) return;
    sendDrainingRef.current.add(sid);
    try {
      for (;;) {
        const q = sendQueuesRef.current.get(sid);
        if (!q || q.length === 0) break;
        const item = q.shift()!;
        recomputeWorkCounts();
        if (item.version !== sendQueueVersionRef.current) continue;
        await sendQueuedMessage(item);
      }
    } finally {
      sendDrainingRef.current.delete(sid);
      const q = sendQueuesRef.current.get(sid);
      if (q && q.length > 0) {
        void drainSendQueue(sid);
      } else {
        /* Prune the now-empty queue so the map doesn't accumulate one empty
         * array per ever-used session across long-lived tab churn. */
        sendQueuesRef.current.delete(sid);
      }
    }
  }

  async function sendQueuedMessage(item: QueuedChatSend) {
    const text = item.text;
    if (item.version !== sendQueueVersionRef.current) return;

    // Bind this turn to the tab it was ENQUEUED from (captured at enqueue), not
    // whatever tab is active now — the user may have switched tabs while the queue
    // drained. This keeps the stream, the busy indicator and out-of-band SSE events
    // (the provider `session` id in particular) on the originating tab.
    const aimeeSid = item.originSid;
    const streamRefs: ActiveStreamRefs = { assistantId: null, thinkId: null, toolId: null, originSid: aimeeSid };
    const controller = new AbortController();
    activeSendAbortRefs.current.set(controller, aimeeSid);
    recomputeWorkCounts();

    try {
      const tabIdx = tabsRef.current.findIndex(t => t.aimeeSid === aimeeSid);
      const activeTab = tabIdx >= 0 ? tabsRef.current[tabIdx] : undefined;
      // Unified-presence: attach a "webchat" surface once per tab/session so this
      // turn is arbitrated (a racing surface on the same session is declined with
      // presence_busy) and other surfaces see the live turn on the events stream.
      // Best-effort: on failure the turn just proceeds unarbitrated, as before.
      let attachId = activeTab?.attachId ?? '';
      if (aimeeSid && !attachId) {
        try {
          const ar = await fetch('/api/chat/attach', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
            body: JSON.stringify({ sid: aimeeSid }),
          });
          if (ar.ok) {
            const ad = await ar.json() as { attach_id?: string };
            if (ad.attach_id) {
              attachId = ad.attach_id;
              setTabs(prev => {
                const i = prev.findIndex(t => t.aimeeSid === aimeeSid);
                if (i < 0) return prev;
                const next = [...prev];
                next[i] = { ...next[i], attachId };
                return next;
              });
            }
          }
        } catch { /* best-effort: proceed unarbitrated */ }
      }
      const resp = await fetch('/api/chat/send', {
        method: 'POST',
        signal: controller.signal,
        headers: {
          'Content-Type': 'application/json',
          'X-CSRF-Token': window._csrf || '',
        },
        body: JSON.stringify({
          message: text,
          aimee_session_id: aimeeSid,
          attach_id: attachId,
          cwd: item.projectRoot,
        }),
      });

      if (resp.status === 401) {
        window.location.href = '/login';
        return;
      }
      if (!resp.ok || !resp.body) {
        const text = await resp.text();
        let msg = text || `HTTP ${resp.status}`;
        try {
          const parsed = JSON.parse(text) as { error?: string; message?: string };
          msg = parsed.error ?? parsed.message ?? msg;
        } catch { /* ignore */ }
        throw new Error(msg);
      }

      const reader = resp.body.getReader();
      const decoder = new TextDecoder();
      let buf = '';
      let evtType: string | null = null;

      const processSseBuffer = (flush: boolean): boolean => {
        const lines = buf.split('\n');
        buf = lines.pop() ?? '';
        if (flush && buf.length > 0) {
          lines.push(buf);
          buf = '';
        }

        for (const rawLine of lines) {
          const line = rawLine.endsWith('\r') ? rawLine.slice(0, -1) : rawLine;
          if (line.startsWith('event: ')) {
            evtType = line.slice(7).trim();
          } else if (line.startsWith('data: ') && evtType) {
            try {
              const data = parseJSON(line.slice(6)) as Record<string, unknown>;
              if (evtType === 'ir') {
                ingestIR(streamRefs.originSid, data);
                evtType = null;
                continue;
              }

              if (controller.signal.aborted || !activeSendAbortRefs.current.has(controller)) {
                return false;
              }
              throw new Error(evtType === 'error' ? String(data.message || 'Chat transport failed') : `Unsupported chat event: ${evtType}`);
            } catch (error) {
              throw error instanceof Error ? error : new Error('Invalid session event');
            }
            evtType = null;
          }
        }
        return true;
      };

      while (true) {
        const { done, value } = await reader.read();
        if (done) {
          buf += decoder.decode();
          if (!processSseBuffer(true)) return;
          break;
        }
        if (controller.signal.aborted || !activeSendAbortRefs.current.has(controller)) return;
        buf += decoder.decode(value, { stream: true });
        if (!processSseBuffer(false)) return;
      }
    } catch (e) {
      if (isAbortError(e)) return;
      const errMsg = e instanceof Error ? e.message : 'Unknown error';
      setIRTransportErrors(prev => ({ ...prev, [aimeeSid]: errMsg }));
    } finally {
      // The durable IR log owns the transcript; keep local tab metadata current.
      fetch('/api/lsp/diagnostics/summary').then(r => r.json()).then((d: { errors: number; warnings: number; active_servers: number }) => {
        setLspDiag(d.active_servers > 0 ? d : null);
      }).catch(() => {});
      if (!controller.signal.aborted) saveOwnerStream(streamRefs.originSid);
      flushStreamAppends();
      activeSendAbortRefs.current.delete(controller);
      recomputeWorkCounts(); // clears this sid's busy/iteration once it has no work left
      streamRefs.assistantId = null;
      streamRefs.thinkId = null;
      streamRefs.toolId = null;
      textareaRef.current?.focus();
    }
  }

  /* Apply all buffered stream deltas in one state update, coalesced per message.
   * Safe to call manually (e.g. at turn end) or as the throttled timer callback. */
  function flushStreamAppends() {
    if (flushTimerRef.current !== null) {
      window.clearTimeout(flushTimerRef.current);
      flushTimerRef.current = null;
    }
    const ups = pendingAppendsRef.current;
    if (ups.length === 0) return;
    pendingAppendsRef.current = [];
    // Partition deltas by owning tab, then coalesce per message, so a stream's
    // text lands in ITS tab's buffer even if the user switched tabs since the
    // delta was queued (the flush timer fires asynchronously).
    const byOwner = new Map<string, Map<number, { text: string; think: string }>>();
    for (const u of ups) {
      let byId = byOwner.get(u.owner);
      if (!byId) { byId = new Map(); byOwner.set(u.owner, byId); }
      const e = byId.get(u.id) ?? { text: '', think: '' };
      if (u.field === 'text') e.text += u.delta;
      else e.think += u.delta;
      byId.set(u.id, e);
    }
    // Only the messages that received a delta change — in practice the 1–2 tail
    // blocks of the turn (the streaming assistant bubble, maybe a thinking
    // block). The whole conversation lives in `msgs` (windowing is render-only),
    // so map()-ing a closure + Map.get over the FULL history every flush is
    // O(history) of wasted work — and it runs for EVERY streaming tab ~10×/s, so
    // it scales with running-tab count × session length, pegging a core. Instead
    // copy the array once (cheap pointer copy) and rewrite only the matching
    // indices, scanning from the tail and stopping once every delta is placed.
    const applyDeltas = (msgs: StreamMsg[], byId: Map<number, { text: string; think: string }>): StreamMsg[] => {
      if (byId.size === 0) return msgs;
      let next: StreamMsg[] | null = null;
      let remaining = byId.size;
      for (let i = msgs.length - 1; i >= 0 && remaining > 0; i--) {
        const e = byId.get(msgs[i].id);
        if (!e) continue;
        if (!next) next = msgs.slice();
        const m = msgs[i];
        const nm = { ...m };
        if (e.text) nm.text = m.text + e.text;
        if (e.think) nm.thinkText = (m.thinkText ?? '') + e.think;
        next[i] = nm;
        remaining--;
      }
      return next ?? msgs;
    };
    for (const [owner, byId] of byOwner) {
      applyToOwnerStream(owner, prev => applyDeltas(prev, byId));
    }
  }

  function handleKeyDown(e: React.KeyboardEvent<HTMLTextAreaElement>) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      sendMessage();
    }
  }

  return (
    <div className="chat-shell" style={{
      display: 'flex', flexDirection: 'column', height: '100%',
      overflow: 'hidden', background: tokens.surface,
    }}>
      {/* The project belongs to the active SESSION (top tab); picking one here
          binds it to this session, and the agent runs with it as cwd. A Clear
          button resets this conversation (fresh provider session). */}
      {/* Reserve the tutorial launcher's top-right corner. Without this padding,
          its absolute "?" button sits directly over Clear and steals clicks. */}
      <div className="chat-session-toolbar" style={{ display: 'flex', alignItems: 'center', gap: 8, paddingRight: 48, boxSizing: 'border-box' }}>
        <div style={{ flex: 1, minWidth: 0 }}>
          <ProjectPicker
            key={activeSessionId}
            value={activeSession?.projectRoot ?? ''}
            onChange={sel => {
              const r = sel ? `${sel.root}/${sel.project}` : '';
              if (activeSession) patchSession(activeSession.id, { projectRoot: r, projectName: sel?.project ?? '' });
              else { setProjectRoot(r); saveProjectRoot(r); }
            }}
          />
        </div>
        <Button size="md" aria-expanded={channelsOpen} aria-controls="chat-channels"
          onClick={() => setChannelsOpen(open => !open)}>Channels{channels.length ? ` (${channels.length})` : ''}</Button>
        <Button
          variant="default"
          size="md"
          onClick={clearChat}
          title="Clear this conversation and start a fresh session"
          style={{ flexShrink: 0 }}
        >
          Clear
        </Button>
      </div>

      {/* Thread bar (conversation branching) */}
      {branchingSupported && <ThreadBar
        threads={tabs[activeIdx]?.threads ?? []}
        activeThreadId={tabs[activeIdx]?.activeThreadId ?? 0}
        onSwitch={switchThread}
        onBranch={branchThread}
      />}
      {threadError && <p role="alert">{threadError}</p>}

      {/* Main content area: channel sidebar + chat + optional rules panel */}
      <div style={{ display: 'flex', flex: 1, overflow: 'hidden', position: 'relative' }}>
        {/* Channel sidebar */}
        {channelsOpen && <aside id="chat-channels" className="chat-channels" aria-label="Channels">
        <ChannelSidebar
          channels={channels}
          activeChannel={activeChannel}
          unreadCounts={unreadCounts}
          onSelect={selectChannel}
          onDeselect={() => setActiveChannel(null)}
          onCreateChannel={name => { void createChannel(name); }}
        /></aside>}

        {/* Chat column — shows channel view when a channel is active */}
        <div className="chat-column" style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>

        {activeChannel ? (
          <ChannelView
            channelName={activeChannel}
            messages={channelMsgs[activeChannel] ?? []}
            agents={agents}
            onSend={text => { void sendChannelMessage(text); }}
          />
        ) : (<>

      <details className="chat-workflow" open={workflowInfo ? true : undefined}>
      <summary>{workflowInfo ? `Workflow · ${workflowInfo.status}` : 'Workflow options'}</summary>
      <WorkflowStatusCard
        channel={workflowChannel}
        session={workflowInfo}
        loading={workflowLoading}
        error={workflowError}
        changed={workflowChanged}
        onChannelChange={setCurrentWorkflowChannel}
        onRefresh={() => { void refreshWorkflow(); }}
        onPause={() => { void pauseWorkflow(); }}
      /></details>

      {/* Bootstrap banner */}
      {banner && (
        <BootstrapBanner
          stacks={banner.stacks}
          onGenerate={generateRules}
          onDismiss={dismissRulesBanner}
        />
      )}

      {/* Messages */}
      <div
        className="chat-messages" role="log" aria-label="Conversation" aria-live="off"
        onScroll={handleMessagesScroll}
        style={{
          flex: 1, overflowY: 'auto', padding: '16px',
          display: 'flex', flexDirection: 'column', gap: '12px',
        }}
      >
        {hiddenCount > 0 && (
          <Button
            variant="default"
            size="sm"
            onClick={() => setVisibleCount(c => c + TRANSCRIPT_WINDOW_STEP)}
            style={{ alignSelf: 'center', margin: '0 0 4px', borderRadius: '12px' }}
          >
            Show earlier messages ({hiddenCount})
          </Button>
        )}
        {canonicalState && (canonicalState.messages.length > 0 || Object.keys(canonicalState.children ?? {}).length > 0 || Object.keys(canonicalState.controls).length > 0) ? <>
          <IRTranscript state={canonicalState} onReply={replyControl} controlsReady={!!irReady[activeAimeeSid]} messageLimit={visibleCount} pendingReplies={pendingReplies[activeAimeeSid]} pendingRepliesBySession={pendingReplies} /></> :
          <Transcript messages={windowedMsgs} working={working} activeSid={tabs[activeIdx]?.aimeeSid ?? ''} />}
        {irTransportErrors[activeAimeeSid] ? <div role="alert">
          {irTransportErrors[activeAimeeSid]}{' '}
          <button type="button" onClick={() => {
            irPending.current[activeAimeeSid] = new Map();
            setIRReady(prev => ({ ...prev, [activeAimeeSid]: false }));
            setIRReconnect(value => value + 1);
          }}>Reconnect conversation</button>
        </div> : !irReady[activeAimeeSid] && <p role="status">Synchronizing conversation…</p>}
        {working && <div style={{ alignSelf: 'flex-start' }}><TypingIndicator /></div>}
        {remoteTurnActive && !working && (
          <div style={{
            alignSelf: 'flex-start', maxWidth: '85%', padding: '6px 10px',
            background: tokens.borderLight, border: `1px solid ${tokens.borderMedium}`,
            borderRadius: '10px', color: tokens.textFaint, fontSize: '11px', fontFamily: 'system-ui',
          }}>
            <div>● aimee is active on another surface…</div>

          </div>
        )}
        {iterCur > 0 && (
          <div style={{
            alignSelf: 'flex-start', padding: '2px 8px', background: tokens.borderLight,
            border: `1px solid ${tokens.borderMedium}`, borderRadius: '10px', color: tokens.textFaint,
            fontSize: '11px', fontFamily: 'system-ui',
          }}>
            Turn {iterCur}/{iterMax}
          </div>
        )}
        <div ref={messagesEndRef} />
      </div>

      {/* Usage status bar */}
      {(lastUsage || lspDiag) && (
        <div style={{
          padding: '2px 16px', background: tokens.surfaceAlt,
          borderTop: `1px solid ${tokens.borderLight}`,
          fontSize: '11px', color: tokens.textHint, flexShrink: 0,
          fontFamily: 'system-ui', display: 'flex', alignItems: 'center', gap: '8px',
        }}>
          {lastUsage && (
            <span>
              {lastUsage.in} in / {lastUsage.out} out
              {lastUsage.cost > 0 && ` | ~$${lastUsage.cost.toFixed(4)}`}
            </span>
          )}
          {lspDiag && (lspDiag.errors > 0 || lspDiag.warnings > 0) && (
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: '4px' }}>
              {lastUsage && <span style={{ color: tokens.borderLight }}>|</span>}
              {lspDiag.errors > 0 && (
                <span style={{
                  padding: '1px 5px', borderRadius: '8px', fontSize: '10px',
                  background: 'var(--sg-danger-bg)', color: 'var(--sg-danger-light)', border: '1px solid var(--sg-danger-bg)',
                }} title="LSP errors">
                  ✕ {lspDiag.errors}
                </span>
              )}
              {lspDiag.warnings > 0 && (
                <span style={{
                  padding: '1px 5px', borderRadius: '8px', fontSize: '10px',
                  background: 'var(--sg-warning-bg)', color: 'var(--sg-warning)', border: '1px solid var(--sg-warning-bg)',
                }} title="LSP warnings">
                  ⚠ {lspDiag.warnings}
                </span>
              )}
            </span>
          )}
        </div>
      )}

      {/* Prompt tier selector */}
      <div role="group" aria-label="Session preferences" style={{
        padding: '4px 16px', background: tokens.surfaceAlt,
        borderTop: `1px solid ${tokens.borderLight}`,
        display: 'flex', flexWrap: 'wrap', alignItems: 'center', gap: '6px 12px', flexShrink: 0,
      }}>
        {/* The project is set in ONE place — the session ProjectPicker at the top
            of the chat (bound to the active session). No per-prompt project
            selector here. */}
        <div style={{ display: 'flex', alignItems: 'center', gap: '6px', flexShrink: 0 }}>
          <span
            style={{ fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui' }}
            title="How much context Aimee assembles for this turn: Minimal is fastest and cheapest, Extended packs in the most background."
          >
            Prompt:
          </span>
          <Tabs
            size="sm"
            ariaLabel="Prompt tier"
            value={promptTier}
            onChange={(v) => changePromptTier(v as 'MINIMAL' | 'STANDARD' | 'EXTENDED')}
            options={(['MINIMAL', 'STANDARD', 'EXTENDED'] as const).map(tier => ({
              value: tier,
              label: tier.charAt(0) + tier.slice(1).toLowerCase(),
            }))}
          />
        </div>
        {availableSkills.length > 0 && (
          <div style={{ display: 'flex', alignItems: 'center', gap: '6px', minWidth: 0, maxWidth: '100%' }}>
            <span style={{ fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui', flexShrink: 0 }}>
              Skill:
            </span>
            <select
              aria-label="Skill"
              value={activeSkill}
              onChange={e => changeSkill(e.target.value)}
              title="Load a packaged skill for this session — its instructions steer how Aimee handles the turn. None runs the default behaviour."
              style={{
                fontSize: '11px', fontFamily: 'system-ui', minWidth: 0, maxWidth: '100%',
                background: activeSkill ? tokens.primary : tokens.surface,
                color: activeSkill ? tokens.surface : tokens.textFaint,
                border: `1px solid ${activeSkill ? tokens.primary : tokens.borderLight}`,
                borderRadius: '10px', cursor: 'pointer', padding: '2px 6px',
                outline: 'none',
              }}
            >
              <option value="">None</option>
              {availableSkills.map(s => (
                <option key={s} value={s}>{s}</option>
              ))}
            </select>
          </div>
        )}
        {agents.length > 0 && (
          <div style={{ display: 'flex', alignItems: 'center', gap: '6px', minWidth: 0, maxWidth: '100%' }}>
            <span style={{ fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui', flexShrink: 0 }}>
              Agent:
            </span>
            <select
              value={primary.known ? activeAgent || defaultAgent : ''}
              onChange={e => primary.change(e.target.value)}
              aria-label="Agent" aria-busy={primary.pending}
              title={activeAgent
                ? 'The agent serving this tab (pinned for this session)'
                : `Following the configured primary${defaultAgent ? ` (${defaultAgent})` : ''} — pick one to pin it for this session`}
              style={{
                fontSize: '11px', fontFamily: 'system-ui', minWidth: 0, maxWidth: '100%',
                // Unpinned (following the default) reads as the muted state, the
                // same way an unset persona does.
                background: activeAgent ? tokens.primary : tokens.surface,
                color: activeAgent ? tokens.surface : tokens.textFaint,
                border: `1px solid ${activeAgent ? tokens.primary : tokens.borderLight}`,
                borderRadius: '10px', cursor: 'pointer', padding: '2px 6px',
                outline: 'none',
              }}
            >
              {!primary.known && <option value="" disabled>{primary.error ? 'Agent unavailable' : 'Loading agent…'}</option>}
              {primary.known && !agents.some(a => a.name === (activeAgent || defaultAgent)) &&
                <option value={activeAgent || defaultAgent} disabled>{activeAgent || defaultAgent ? `${activeAgent || defaultAgent} (not in current catalog)` : 'Configured primary'}</option>}
              {agents.map(a => (
                <option key={a.name} value={a.name}>
                  {a.name}{a.name === defaultAgent ? ' (primary)' : ''}
                </option>
              ))}
            </select>
          </div>
        )}
        {availablePersonas.length > 0 && (
          <div style={{ display: 'flex', alignItems: 'center', gap: '6px', minWidth: 0, maxWidth: '100%' }}>
            <span style={{ fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui', flexShrink: 0 }}>
              Persona:
            </span>
            <select
              value={persona.known ? activePersona : ''}
              onChange={e => persona.change(e.target.value)}
              aria-label="Persona" aria-busy={persona.pending}
              title="Steers Aimee's role and delegate policy for this session"
              style={{
                fontSize: '11px', fontFamily: 'system-ui', minWidth: 0, maxWidth: '100%',
                background: activePersona ? tokens.primary : tokens.surface,
                color: activePersona ? tokens.surface : tokens.textFaint,
                border: `1px solid ${activePersona ? tokens.primary : tokens.borderLight}`,
                borderRadius: '10px', cursor: 'pointer', padding: '2px 6px',
                outline: 'none',
              }}
            >
              {!persona.known && <option value="" disabled>{persona.error ? 'Persona unavailable' : 'Loading persona…'}</option>}
              {persona.known && !availablePersonas.some(p => p.name === activePersona) &&
                <option value={activePersona} disabled>{activePersona ? `${activePersona} (not in current catalog)` : 'Session default'}</option>}
              {availablePersonas.map(p => (
                <option key={p.name} value={p.name} title={p.description}>{p.name}</option>
              ))}
            </select>
          </div>
        )}
      </div>
      {(primary.error || persona.error) && <p role="alert">{primary.error || persona.error}</p>}
      {(primary.pending || persona.pending) && <p role="status">Saving session selection…</p>}

      {/* Input area */}
      <div style={{
        padding: '12px 16px', background: tokens.surfaceAlt, borderTop: `1px solid ${tokens.border}`,
        display: 'flex', gap: '8px', flexShrink: 0,
      }}>
        <AutoGrowTextarea
          ref={textareaRef}
          value={inputText}
          onChange={setInputText}
          onKeyDown={handleKeyDown}
          maxHeight={200}
          placeholder={(working || remoteTurnActive)
            ? 'Steer the running turn…'
            : 'Type a message… (Shift+Enter for newline)'}
          style={{ flex: 1, minHeight: '44px' }}
          onFocus={e => (e.target.style.borderColor = tokens.primary)}
          onBlur={e => (e.target.style.borderColor = tokens.borderMedium)}
          autoFocus
        />
        {/* A turn in flight for the active tab (local or a server/foreign turn)
            means sending will interrupt-and-steer; show the spinner + "Steer" and
            keep aria-busy aligned so the announced and visible state agree. */}
        {(() => {
          const steering = working || remoteTurnActive;
          const busy = queueActive || steering;
          const label = steering ? 'Steer' : 'Send';
          return (
            <Button
              variant="primary"
              busy={busy}
              disabled={primary.pending || persona.pending}
              onClick={sendMessage}
              title={steering ? 'Interrupt the running turn and continue with this message' : 'Send'}
              style={{
                padding: '10px 12px', minHeight: 44, alignSelf: 'flex-end', flexShrink: 0, background: tokens.primary,
                color: tokens.surface, border: 'none', borderRadius: '6px',
                cursor: 'pointer', fontSize: '14px', whiteSpace: 'nowrap',
              }}
            >
              {busy ? (
                <span style={{ display: 'inline-flex', alignItems: 'center', gap: '8px' }}>
                  <span className="chat-send-spinner" aria-hidden="true" />
                  {label}
                </span>
              ) : label}
            </Button>
          );
        })()}
      </div>

        </>)}{/* end chat/channel conditional */}
        </div>{/* end chat column */}

        <ContextPanel
          open={contextOpen}
          onToggle={() => { setContextOpen(o => !o); setPluginsOpen(false); setRulesOpen(false); }}
          sessionUsage={sessionUsage}
          sessionId={tabs[activeIdx]?.aimeeSid ?? ''}
          msgCount={streamMsgs.filter(m => m.type === 'user').length}
          metrics={allTimeMetrics}
        />

        {pluginLoaderAvailable && <PluginsPanel
          open={pluginsOpen}
          plugins={plugins}
          loading={pluginsLoading}
          error={pluginsError}
          onToggle={() => { setPluginsOpen(o => !o); setRulesOpen(false); setContextOpen(false); }}
          onRefresh={() => { void refreshPlugins(); }}
          onPluginToggle={name => { void togglePlugin(name); }}
        />}

        {/* Rules sidebar */}
        <RulesPanel open={rulesOpen} onToggle={() => { setRulesOpen(o => !o); setPluginsOpen(false); setContextOpen(false); }} />
      </div>{/* end main content row */}
    </div>
  );
}
