import { useEffect, useRef, useState, useCallback } from 'react';
import { Spinner, tokens } from '@rakuensoftware/smoothgui';
import { BootstrapBanner, DiffBlock, Message, RewindMarker, ThinkingBlock, ToolBlock, TurnSummaryCard } from './chat/ChatPrimitives';
import { escHtml, renderMd, renderWithMentions } from './chat/markdown';
import ProjectPicker from '../components/ProjectPicker';
import { useSessions } from '../SessionContext';

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

interface ProjectInfo {
  name: string;
  root: string;
  scanned_at?: string;
  current?: boolean;
}

interface ChatProjectsResponse {
  current_root?: string;
  projects?: ProjectInfo[];
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

interface ChannelMessage {
  sender: string;
  text: string;
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

interface AgentInfo {
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

function loadActiveChannel(): string | null {
  try { return localStorage.getItem(CHANNEL_KEY); } catch { return null; }
}

function saveActiveChannel(name: string | null): void {
  try {
    if (name) localStorage.setItem(CHANNEL_KEY, name);
    else localStorage.removeItem(CHANNEL_KEY);
  } catch { /* ignore */ }
}

function loadProjectRoot(): string {
  try { return localStorage.getItem(PROJECT_ROOT_KEY) ?? ''; } catch { return ''; }
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

function saveTabs(tabs: TabData[]): void {
  try { localStorage.setItem(TABS_KEY, JSON.stringify(tabs)); } catch { /* ignore */ }
}

/* Server-side session persistence (per logged-in user). The conversation lives
 * on aimee-server keyed by aimeeSid; webchat stores which sessions belong to the
 * user so tabs survive a browser/laptop crash or a move to another device.
 * localStorage remains a fast local cache (and holds per-tab message history);
 * the server is authoritative for *which* tabs exist. */

interface ServerSession {
  id: string;
  title?: string;
  cwd?: string;
  created_at?: string;
  last_active?: string;
}

async function fetchServerSessions(): Promise<ServerSession[]> {
  try {
    const r = await fetch('/api/chat/sessions');
    if (!r.ok) return [];
    const data = await r.json();
    return Array.isArray(data) ? (data as ServerSession[]) : [];
  } catch {
    return [];
  }
}

// mergeServerSessions restores server-persisted sessions the local cache is
// missing (e.g. after a crash cleared localStorage, or on a fresh device),
// appending them as message-empty tabs keyed by the stored aimeeSid. Local tabs
// are never dropped — except a single pristine, unused default tab, which the
// restored sessions replace so the user does not see a stray empty "Chat".
function mergeServerSessions(local: TabData[], server: ServerSession[]): TabData[] {
  const known = new Set(local.map(t => t.aimeeSid).filter(Boolean));
  const restored: TabData[] = [];
  for (const s of server) {
    if (s.id && !known.has(s.id)) {
      restored.push(normalizeTab({ title: s.title ?? '', messages: [], sid: '', aimeeSid: s.id }, 0));
      known.add(s.id);
    }
  }
  if (restored.length === 0) return local;
  const pristineDefault =
    local.length === 1 && local[0].messages.length === 0 && !local[0].sid;
  return pristineDefault ? restored : [...local, ...restored];
}

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
}

interface QueuedChatSend {
  text: string;
  version: number;
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
        <button
          onClick={onBranch}
          style={{
            padding: '2px 10px', fontSize: '11px', color: tokens.textSecondary,
            background: tokens.surface, border: `1px solid ${tokens.borderMedium}`,
            borderRadius: '4px', cursor: 'pointer',
          }}
          onMouseOver={e => (e.currentTarget.style.borderColor = tokens.primary)}
          onMouseOut={e => (e.currentTarget.style.borderColor = tokens.borderMedium)}
          title="Branch conversation to explore an alternative approach"
        >
          Branch
        </button>
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
      <button
        onClick={onBranch}
        style={{
          padding: '2px 10px', fontSize: '11px', color: tokens.textPale,
          background: 'transparent', border: `1px dashed ${tokens.borderMedium}`,
          borderRadius: '4px', cursor: 'pointer',
        }}
        onMouseOver={e => (e.currentTarget.style.borderColor = tokens.primary)}
        onMouseOut={e => (e.currentTarget.style.borderColor = tokens.borderMedium)}
        title="Branch from current point"
      >
        + Branch
      </button>
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

  if (!open) {
    return (
      <button
        onClick={onToggle}
        title="Collaborative Rules"
        style={{
          position: 'absolute', right: 8, top: 8, zIndex: 10,
          padding: '4px 10px', fontSize: '12px', borderRadius: '4px',
          cursor: 'pointer', border: `1px solid ${tokens.borderMedium}`,
          background: tokens.surfaceAlt, color: tokens.textSecondary,
        }}
      >
        Rules{active.length > 0 ? ` (${active.length})` : ''}
        {proposed.length > 0 && (
          <span style={{ marginLeft: 4, color: tokens.warning, fontWeight: 'bold' }}>
            {proposed.length} pending
          </span>
        )}
      </button>
    );
  }

  return (
    <div style={{
      width: 280, flexShrink: 0, borderLeft: `1px solid ${tokens.border}`,
      background: tokens.surfaceAlt, display: 'flex', flexDirection: 'column',
      fontSize: '13px', overflow: 'hidden',
    }}>
      <div style={{
        padding: '10px 12px', borderBottom: `1px solid ${tokens.border}`,
        display: 'flex', justifyContent: 'space-between', alignItems: 'center',
      }}>
        <span style={{ fontWeight: 'bold', color: tokens.text }}>
          Rules <span style={{ fontWeight: 'normal', color: tokens.textFaint, fontSize: '11px' }}>epoch {epoch}</span>
        </span>
        <button
          onClick={onToggle}
          style={{
            border: 'none', background: 'transparent', cursor: 'pointer',
            color: tokens.textPale, fontSize: '16px', lineHeight: 1,
          }}
        >
          ×
        </button>
      </div>
      <div style={{ flex: 1, overflowY: 'auto', padding: '8px 12px' }}>
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
                  <button
                    onClick={() => doAction(r.id, 'approve')}
                    style={{
                      padding: '3px 10px', fontSize: '11px', borderRadius: '3px',
                      cursor: 'pointer', border: '1px solid #2d4d2d',
                      background: '#1a3a1a', color: '#8c8',
                    }}
                  >
                    Approve
                  </button>
                  <button
                    onClick={() => doAction(r.id, 'reject')}
                    style={{
                      padding: '3px 10px', fontSize: '11px', borderRadius: '3px',
                      cursor: 'pointer', border: `1px solid ${tokens.borderMedium}`,
                      background: 'transparent', color: tokens.danger,
                    }}
                  >
                    Reject
                  </button>
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
                  <button
                    onClick={() => doAction(r.id, 'retire')}
                    style={{
                      padding: '3px 10px', fontSize: '11px', borderRadius: '3px',
                      cursor: 'pointer', border: `1px solid ${tokens.borderMedium}`,
                      background: 'transparent', color: tokens.textSecondary,
                    }}
                  >
                    Retire
                  </button>
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
      </div>
    </div>
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

  if (!open) {
    return (
      <button
        onClick={onToggle}
        title="Context"
        style={{
          position: 'absolute', right: 160, top: 8, zIndex: 10,
          padding: '4px 10px', fontSize: '12px', borderRadius: '4px',
          cursor: 'pointer', border: `1px solid ${tokens.borderMedium}`,
          background: tokens.surfaceAlt, color: tokens.textSecondary,
        }}
      >
        Context
      </button>
    );
  }

  return (
    <div style={{
      width: 260, flexShrink: 0, borderLeft: `1px solid ${tokens.border}`,
      background: tokens.surfaceAlt, display: 'flex', flexDirection: 'column',
      fontSize: '13px', overflow: 'hidden',
    }}>
      <div style={{
        padding: '10px 12px', borderBottom: `1px solid ${tokens.border}`,
        display: 'flex', justifyContent: 'space-between', alignItems: 'center',
      }}>
        <span style={{ fontWeight: 'bold', color: tokens.text }}>Context</span>
        <button
          onClick={onToggle}
          style={{ border: 'none', background: 'transparent', cursor: 'pointer', color: tokens.textPale, fontSize: '16px', lineHeight: 1 }}
        >×</button>
      </div>
      <div style={{ flex: 1, overflowY: 'auto', padding: '10px 12px' }}>
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
      </div>
    </div>
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
  if (!open) {
    return (
      <button
        onClick={onToggle}
        title="Plugins"
        style={{
          position: 'absolute', right: 84, top: 8, zIndex: 10,
          padding: '4px 10px', fontSize: '12px', borderRadius: '4px',
          cursor: 'pointer', border: `1px solid ${tokens.borderMedium}`,
          background: tokens.surfaceAlt, color: tokens.textSecondary,
        }}
      >
        Plugins{plugins.length > 0 ? ` (${plugins.length})` : ''}
      </button>
    );
  }

  return (
    <div style={{
      width: 300, flexShrink: 0, borderLeft: `1px solid ${tokens.border}`,
      background: tokens.surfaceAlt, display: 'flex', flexDirection: 'column',
      fontSize: '13px', overflow: 'hidden',
    }}>
      <div style={{
        padding: '10px 12px', borderBottom: `1px solid ${tokens.border}`,
        display: 'flex', justifyContent: 'space-between', alignItems: 'center',
      }}>
        <span style={{ fontWeight: 'bold', color: tokens.text }}>Plugins</span>
        <div style={{ display: 'flex', gap: 8 }}>
          <button onClick={onRefresh} style={{ border: 'none', background: 'transparent', cursor: 'pointer', color: tokens.textPale, fontSize: '12px' }}>Refresh</button>
          <button onClick={onToggle} style={{ border: 'none', background: 'transparent', cursor: 'pointer', color: tokens.textPale, fontSize: '16px', lineHeight: 1 }}>×</button>
        </div>
      </div>
      <div style={{ flex: 1, overflowY: 'auto', padding: '8px 12px' }}>
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
                  cursor: 'pointer', border: `1px solid ${plugin.enabled ? '#2c5b3b' : tokens.borderMedium}`,
                  background: plugin.enabled ? '#1b3a26' : 'transparent',
                  color: plugin.enabled ? '#8fd3a8' : tokens.textSecondary,
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
      </div>
    </div>
  );
}

/* ---- Working indicator ---- */

function WorkingIndicator() {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '8px 14px', color: tokens.primary, fontSize: '13px', alignSelf: 'flex-start' }}>
      <div style={{ display: 'inline-flex', gap: '4px' }}>
        {[0, 0.2, 0.4].map((delay, i) => (
          <span
            key={i}
            style={{
              width: 6, height: 6, borderRadius: '50%', background: tokens.primary,
              animation: `pulse 1.4s ${delay}s infinite ease-in-out`,
            }}
          />
        ))}
      </div>
      Working
      <style>{`
        @keyframes pulse {
          0%, 80%, 100% { opacity: 0.2; transform: scale(0.8); }
          40% { opacity: 1; transform: scale(1); }
        }
      `}</style>
    </div>
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
            background: '#1b3a26',
            color: '#8fd3a8',
            border: '1px solid #2c5b3b',
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
        <button
          onClick={onRefresh}
          style={{
            padding: '5px 10px',
            fontSize: '11px',
            borderRadius: '6px',
            cursor: 'pointer',
            border: `1px solid ${tokens.borderMedium}`,
            background: 'transparent',
            color: tokens.textSecondary,
          }}
        >
          Refresh
        </button>
        {session && session.status !== 'complete' && (
          <button
            onClick={onPause}
            style={{
              padding: '5px 10px',
              fontSize: '11px',
              borderRadius: '6px',
              cursor: 'pointer',
              border: `1px solid ${tokens.borderMedium}`,
              background: 'transparent',
              color: tokens.warning,
            }}
          >
            Pause
          </button>
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
              background: session.status === 'active' ? '#193226' : session.status === 'paused' ? '#3a3017' : '#1f2b3a',
              color: session.status === 'active' ? '#7bd9a2' : session.status === 'paused' ? '#f2d06b' : '#9ec7ff',
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
                  background: tokens.primary, color: '#fff',
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
  agents: AgentInfo[];
  onSend: (text: string) => void;
}

function mentionStateColor(state: string): string {
  if (state === 'active') return '#8fd3a8';
  if (state === 'idle') return '#f0c36a';
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

function projectDisplayName(project: ProjectInfo): string {
  if (project.current) return `${project.name || 'Current'} (current)`;
  if (project.name) return project.name;
  return project.root.split(/[\\/]/).filter(Boolean).pop() || project.root;
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
          <div key={i} style={{ fontSize: '13px' }}>
            <span style={{ fontWeight: 600, color: tokens.primary, marginRight: '6px' }}>
              {m.sender}
            </span>
            {m.created_at && (
              <span style={{ fontSize: '11px', color: tokens.textPale, marginRight: '8px' }}>
                {new Date(m.created_at + 'Z').toLocaleTimeString()}
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
        <button
          onClick={() => { const t = text.trim(); if (t) { onSend(t); setText(''); } }}
          style={{
            padding: '8px 16px', background: tokens.primary, color: '#fff',
            border: 'none', borderRadius: '6px', cursor: 'pointer', fontSize: '13px',
          }}
        >Send</button>
      </div>
    </div>
  );
}

/* ---- Main Chat component ---- */

export default function Chat() {
  const [initialChatState] = useState(loadInitialChatState);
  const [tabs, setTabs] = useState<TabData[]>(initialChatState.tabs);
  const [activeIdx, setActiveIdx] = useState(initialChatState.activeIdx);
  const [streamMsgs, setStreamMsgs] = useState<StreamMsg[]>([]);
  const [working, setWorking] = useState(false);
  // True while a turn is in flight on this tab's session driven by *another*
  // surface (CLI/TUI, another tab). Fed by the presence-event stream; shown only
  // when this tab isn't itself working (see render).
  const [remoteTurnActive, setRemoteTurnActive] = useState(false);
  // Live text of the other surface's in-flight turn, accumulated from
  // turn_delta events (emitted by the server when >1 surface shares a session).
  const [remoteTurnText, setRemoteTurnText] = useState('');
  const [pendingSends, setPendingSends] = useState(0);
  const [queuedSends, setQueuedSends] = useState(0);
  const [iterCur, setIterCur] = useState(0);
  const [iterMax, setIterMax] = useState(0);
  const [inputText, setInputText] = useState('');
  const [banner, setBanner] = useState<{ stacks: BootstrapStack[] } | null>(null);
  const [rulesOpen, setRulesOpen] = useState(false);
  const [pluginsOpen, setPluginsOpen] = useState(false);
  const [contextOpen, setContextOpen] = useState(false);
  const [lastUsage, setLastUsage] = useState<{ in: number; out: number; cost: number } | null>(null);
  const [sessionUsage, setSessionUsage] = useState<{ in: number; out: number; cost: number; kind: string }>({ in: 0, out: 0, cost: 0, kind: 'realized' });
  const [allTimeMetrics, setAllTimeMetrics] = useState<MetricRow[]>([]);
  const [promptTier, setPromptTier] = useState<'MINIMAL' | 'STANDARD' | 'EXTENDED'>('STANDARD');
  const [projectRoot, setProjectRoot] = useState(loadProjectRoot);
  // The active top-level session owns the project this chat runs in. Mirror the
  // session's project into the chat cwd, and start that session's conversation
  // fresh when the session (or its project) changes.
  const { active: activeSession, patchSession } = useSessions();
  const activeSessionId = activeSession?.id ?? '';
  const sessionProject = activeSession?.projectRoot ?? '';
  useEffect(() => {
    changeProjectRoot(sessionProject);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeSessionId, sessionProject]);
  const [projects, setProjects] = useState<ProjectInfo[]>([]);
  const [availableSkills, setAvailableSkills] = useState<string[]>([]);
  const [activeSkill, setActiveSkill] = useState<string>('');
  const [availablePersonas, setAvailablePersonas] = useState<PersonaInfo[]>([]);
  const [activePersona, setActivePersona] = useState<string>('');
  const [lspDiag, setLspDiag] = useState<{ errors: number; warnings: number; active_servers: number } | null>(null);
  const [workflowInfo, setWorkflowInfo] = useState<WorkflowSessionInfo | null>(null);
  const [workflowLoading, setWorkflowLoading] = useState(false);
  const [workflowError, setWorkflowError] = useState<string | null>(null);
  const [workflowChanged, setWorkflowChanged] = useState(false);
  const [plugins, setPlugins] = useState<PluginInfo[]>([]);
  const [pluginsLoading, setPluginsLoading] = useState(false);
  const [pluginsError, setPluginsError] = useState<string | null>(null);
  const [agents, setAgents] = useState<AgentInfo[]>([]);

  /* Channel state */
  const [channels, setChannels] = useState<ChannelInfo[]>([]);
  const [activeChannel, setActiveChannel] = useState<string | null>(loadActiveChannel);
  const [channelMsgs, setChannelMsgs] = useState<Record<string, ChannelMessage[]>>({});
  const [unreadCounts, setUnreadCounts] = useState<Record<string, number>>({});
  const channelSseRef = useRef<EventSource | null>(null);
  const presenceSseRef = useRef<EventSource | null>(null);
  const activeChannelRef = useRef<string | null>(activeChannel);

  const messagesEndRef = useRef<HTMLDivElement>(null);
  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const workflowUpdatedAtRef = useRef<string | null>(null);
  const skipNextStreamPersistRef = useRef(false);
  const streamPersistTimerRef = useRef<number | null>(null);
  const activeSendAbortRefs = useRef<Set<AbortController>>(new Set());
  const sendQueueRef = useRef<QueuedChatSend[]>([]);
  const sendQueueDrainingRef = useRef(false);
  const sendQueueVersionRef = useRef(0);
  const tabsRef = useRef<TabData[]>(tabs);
  const activeIdxRef = useRef(activeIdx);
  const projectRootRef = useRef(projectRoot);
  const sending = pendingSends > 0;
  const queueActive = sending || queuedSends > 0;
  const activePersonaSid = tabs[activeIdx]?.aimeeSid ?? '';

  /* Auto-scroll */
  const scrollToBottom = useCallback(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, []);

  useEffect(() => { scrollToBottom(); }, [streamMsgs, working, scrollToBottom]);
  useEffect(() => { tabsRef.current = tabs; }, [tabs]);

  /* Detach every attached presence surface when the page unloads, so a closed
   * browser doesn't leak attachments until the session is closed. keepalive
   * lets the POSTs survive the unload; best-effort. */
  useEffect(() => {
    const onUnload = () => {
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
    window.addEventListener('beforeunload', onUnload);
    return () => window.removeEventListener('beforeunload', onUnload);
  }, []);

  /* Subscribe to the active session's unified-presence event stream so the UI
   * can show when aimee is responding on another surface (CLI/TUI, another
   * tab). Only meaningful once the tab has attached (presence exists). */
  const activeAimeeSid = tabs[activeIdx]?.aimeeSid ?? '';
  const activeAttachId = tabs[activeIdx]?.attachId ?? '';
  useEffect(() => {
    if (presenceSseRef.current) { presenceSseRef.current.close(); presenceSseRef.current = null; }
    setRemoteTurnActive(false);
    setRemoteTurnText('');
    if (!activeAimeeSid || !activeAttachId) return;

    const es = new EventSource(`/api/chat/session-events?sid=${encodeURIComponent(activeAimeeSid)}`);
    presenceSseRef.current = es;
    es.addEventListener('turn_started', () => { setRemoteTurnActive(true); setRemoteTurnText(''); });
    es.addEventListener('turn_delta', (ev: MessageEvent<string>) => {
      try {
        const d = JSON.parse(ev.data) as { content?: string };
        if (d.content) setRemoteTurnText(prev => (prev + d.content).slice(-2000));
      } catch { /* ignore malformed frame */ }
    });
    es.addEventListener('turn_done', () => { setRemoteTurnActive(false); setRemoteTurnText(''); });
    es.onerror = () => { setRemoteTurnActive(false); setRemoteTurnText(''); };
    return () => { es.close(); presenceSseRef.current = null; };
  }, [activeAimeeSid, activeAttachId]);
  useEffect(() => { activeIdxRef.current = activeIdx; }, [activeIdx]);
  useEffect(() => { projectRootRef.current = projectRoot; }, [projectRoot]);

  function hasPendingChatWork(): boolean {
    return activeSendAbortRefs.current.size > 0 || sendQueueRef.current.length > 0;
  }

  function abortActiveSends(): void {
    sendQueueVersionRef.current++;
    sendQueueRef.current = [];
    activeSendAbortRefs.current.forEach(controller => controller.abort());
    activeSendAbortRefs.current.clear();
    setPendingSends(0);
    setQueuedSends(0);
    setWorking(false);
    setIterCur(0);
    setIterMax(0);
  }

  useEffect(() => {
    return () => {
      abortActiveSends();
    };
  }, []);

  /* Tab persistence */
  useEffect(() => {
    saveTabs(tabs);
  }, [tabs]);

  /* Restore server-persisted sessions on load (per-user, survives a crash or a
   * move to another device). Runs once; merges in any sessions the local cache
   * is missing. */
  useEffect(() => {
    let cancelled = false;
    void (async () => {
      const server = await fetchServerSessions();
      if (cancelled || server.length === 0) return;
      setTabs(prev => mergeServerSessions(prev, server));
    })();
    return () => { cancelled = true; };
  }, []);

  useEffect(() => {
    if (activeIdx >= tabs.length) {
      setActiveIdx(Math.max(0, tabs.length - 1));
    }
  }, [activeIdx, tabs.length]);

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

  useEffect(() => {
    fetch('/api/chat/projects')
      .then(r => r.json())
      .then((d: ChatProjectsResponse) => {
        const nextProjects = d.projects ?? [];
        setProjects(nextProjects);
        const saved = loadProjectRoot();
        const savedExists = saved && nextProjects.some(project => project.root === saved);
        const currentExists = projectRoot && nextProjects.some(project => project.root === projectRoot);
        const current = d.current_root ?? nextProjects[0]?.root ?? '';
        if (!projectRoot || !currentExists) {
          const nextRoot = savedExists ? saved : current;
          setProjectRoot(nextRoot);
          saveProjectRoot(nextRoot);
        }
      })
      .catch(() => {});
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

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

  /* Reflect the active tab's current persona (per-session, else durable default) */
  useEffect(() => {
    if (!activePersonaSid) return;
    fetch(`/api/chat/persona?sid=${encodeURIComponent(activePersonaSid)}`)
      .then(r => r.json())
      .then((d: { name?: string }) => setActivePersona(d.name ?? ''))
      .catch(() => {});
  }, [activePersonaSid]);

  const refreshPlugins = useCallback(async () => {
    setPluginsLoading(true);
    try {
      const resp = await fetch('/api/plugins');
      const data = await resp.json() as PluginInfo[];
      setPlugins(Array.isArray(data) ? data : []);
      setPluginsError(null);
    } catch {
      setPluginsError('Failed to load plugins');
    } finally {
      setPluginsLoading(false);
    }
  }, []);

  useEffect(() => {
    if (pluginsOpen) void refreshPlugins();
  }, [pluginsOpen, refreshPlugins]);

  /* Fetch channel list on mount */
  useEffect(() => {
    fetch('/api/channels')
      .then(r => r.json())
      .then((d: { channels?: ChannelInfo[] }) => setChannels(d.channels ?? []))
      .catch(() => {});
  }, []);

  useEffect(() => {
    fetch('/api/agents')
      .then(r => r.json())
      .then((d: { agents?: AgentInfo[] }) => setAgents(d.agents ?? []))
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

  /* Keep activeChannelRef in sync for SSE handler closure */
  useEffect(() => { activeChannelRef.current = activeChannel; }, [activeChannel]);

  /* Persist active channel to localStorage */
  useEffect(() => { saveActiveChannel(activeChannel); }, [activeChannel]);

  /* SSE subscription for active channel */
  useEffect(() => {
    if (channelSseRef.current) { channelSseRef.current.close(); channelSseRef.current = null; }
    if (!activeChannel) return;

    /* Fetch existing messages first */
    fetch(`/api/channels/${encodeURIComponent(activeChannel)}/messages`)
      .then(r => r.json())
      .then((d: { messages?: ChannelMessage[] }) => {
        setChannelMsgs(prev => ({ ...prev, [activeChannel]: d.messages ?? [] }));
        setUnreadCounts(prev => ({ ...prev, [activeChannel]: 0 }));
      })
      .catch(() => {});

    const es = new EventSource(`/api/channels/${encodeURIComponent(activeChannel)}/events`);
    channelSseRef.current = es;

    es.addEventListener('message', (ev: MessageEvent<string>) => {
      try {
        const msg = JSON.parse(ev.data) as ChannelMessage;
        const ch = activeChannelRef.current;
        setChannelMsgs(prev => ({
          ...prev,
          [activeChannel]: [...(prev[activeChannel] ?? []), msg],
        }));
        if (ch !== activeChannel) {
          setUnreadCounts(prev => ({ ...prev, [activeChannel]: (prev[activeChannel] ?? 0) + 1 }));
        }
      } catch { /* ignore */ }
    });

    return () => { es.close(); channelSseRef.current = null; };
  }, [activeChannel]); // eslint-disable-line react-hooks/exhaustive-deps

  function selectChannel(name: string): void {
    abortActiveSends();
    setActiveChannel(name);
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

  /* Set the active tab's persona server-side. It is sticky per session, so the
     chat system prompt and delegate policy both follow it. */
  async function changePersona(name: string) {
    const sid = tabsRef.current[activeIdxRef.current]?.aimeeSid;
    if (!sid || !name) return;
    const prev = activePersona;
    setActivePersona(name);
    try {
      const resp = await fetch('/api/chat/persona', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ sid, name }),
      });
      if (!resp.ok) setActivePersona(prev);
    } catch { setActivePersona(prev); }
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

  /* Load messages for active tab into stream view */
  useEffect(() => {
    const tab = tabs[activeIdx];
    skipNextStreamPersistRef.current = true;
    if (!tab) { setStreamMsgs([]); return; }
    const msgs: StreamMsg[] = tab.messages.map(m => ({
      id: nextId(),
      type: m.role,
      text: m.text,
    }));
    setStreamMsgs(msgs);
  }, [activeIdx]); // eslint-disable-line react-hooks/exhaustive-deps

  /* Save current tab messages back to tabs state */
  const saveTabMessages = useCallback((tabIndex: number, msgs: StreamMsg[]) => {
    const saved = streamToTabMessages(msgs);
    setTabs(prev => {
      const tab = prev[tabIndex];
      if (!tab || sameTabMessages(tab.messages, saved)) return prev;
      const next = [...prev];
      next[tabIndex] = { ...tab, messages: saved };
      tabsRef.current = next;
      return next;
    });
  }, []);

  const saveCurrentTabMessages = useCallback((msgs: StreamMsg[]) => {
    saveTabMessages(activeIdxRef.current, msgs);
  }, [saveTabMessages]);

  useEffect(() => {
    if (skipNextStreamPersistRef.current) {
      skipNextStreamPersistRef.current = false;
      return;
    }

    const tabIndex = activeIdx;
    if (streamPersistTimerRef.current !== null) {
      window.clearTimeout(streamPersistTimerRef.current);
    }
    streamPersistTimerRef.current = window.setTimeout(() => {
      streamPersistTimerRef.current = null;
      saveTabMessages(tabIndex, streamMsgs);
    }, STREAM_PERSIST_DEBOUNCE_MS);

    return () => {
      if (streamPersistTimerRef.current !== null) {
        window.clearTimeout(streamPersistTimerRef.current);
        streamPersistTimerRef.current = null;
      }
    };
  }, [streamMsgs, activeIdx, saveTabMessages]);

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

  function changeProjectRoot(root: string) {
    setProjectRoot(root);
    saveProjectRoot(root);
    setTabs(prev => {
      const next = [...prev];
      if (next[activeIdx]) {
        next[activeIdx] = { ...next[activeIdx], sid: '', aimeeSid: newAimeeSessionId() };
      }
      return next;
    });
  }

  /* Clear the current conversation: drop the visible transcript and mint a fresh
   * provider session (empty claude session id + new aimee session id) so the next
   * turn starts clean — no resume of a stale/gone session. */
  function clearChat() {
    setStreamMsgs([]);
    setTabs(prev => {
      const next = [...prev];
      if (next[activeIdx]) {
        next[activeIdx] = { ...next[activeIdx], sid: '', aimeeSid: newAimeeSessionId(), attachId: undefined, messages: [] };
      }
      return next;
    });
    if (activeSession) patchSession(activeSession.id, { claudeSid: '', aimeeSid: '', attachId: '' });
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
    try {
      const label = `branch-${(tabs[activeIdx]?.threads?.length ?? 0) + 1}`;
      const r = await fetch('/api/chat/branch', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': window._csrf || '' },
        body: JSON.stringify({ label }),
      });
      const d = await r.json() as { ok?: boolean; thread_id?: number };
      if (d.ok) {
        await refreshThreads();
      }
    } catch { /* ignore */ }
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
    if (!text) return;

    setInputText('');
    if (textareaRef.current) {
      textareaRef.current.style.height = 'auto';
    }

    enqueueChatMessage(text);
  }

  function enqueueChatMessage(text: string) {
    /* Auto-title from first message */
    const idx = activeIdxRef.current;
    const hasExistingMessages = (tabsRef.current[idx]?.messages.length ?? 0) > 0 ||
      streamMsgs.length > 0 || activeSendAbortRefs.current.size > 0 || sendQueueRef.current.length > 0;
    if (!hasExistingMessages) {
      const title = text.substring(0, 30) + (text.length > 30 ? '…' : '');
      setTabs(prev => {
        const next = [...prev];
        if (next[idx]) next[idx] = { ...next[idx], title };
        tabsRef.current = next;
        return next;
      });
    }

    const userMsgId = nextId();
    setStreamMsgs(prev => [...prev, { id: userMsgId, type: 'user', text }]);

    sendQueueRef.current.push({ text, version: sendQueueVersionRef.current });
    setQueuedSends(sendQueueRef.current.length);
    setWorking(true);
    void drainSendQueue();
  }

  async function drainSendQueue() {
    if (sendQueueDrainingRef.current) return;
    sendQueueDrainingRef.current = true;
    try {
      while (sendQueueRef.current.length > 0) {
        const item = sendQueueRef.current.shift()!;
        setQueuedSends(sendQueueRef.current.length);
        if (item.version !== sendQueueVersionRef.current) continue;
        await sendQueuedMessage(item);
      }
    } finally {
      sendQueueDrainingRef.current = false;
      if (sendQueueRef.current.length > 0) {
        void drainSendQueue();
      }
    }
  }

  async function sendQueuedMessage(item: QueuedChatSend) {
    const text = item.text;
    if (item.version !== sendQueueVersionRef.current) return;

    const streamRefs: ActiveStreamRefs = { assistantId: null, thinkId: null, toolId: null };
    const controller = new AbortController();
    activeSendAbortRefs.current.add(controller);
    setPendingSends(activeSendAbortRefs.current.size);
    setWorking(true);

    try {
      const activeTab = tabsRef.current[activeIdxRef.current];
      const aimeeSid = activeTab?.aimeeSid ?? '';
      // Unified-presence: attach a "webchat" surface once per tab/session so this
      // turn is arbitrated (a racing surface on the same session is declined with
      // presence_busy) and other surfaces see the live turn on the events stream.
      // Best-effort: on failure the turn just proceeds unarbitrated, as before.
      let attachId = activeTab?.attachId ?? '';
      if (aimeeSid && !attachId) {
        const tabIdx = activeIdxRef.current;
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
                const next = [...prev];
                if (next[tabIdx] && next[tabIdx].aimeeSid === aimeeSid) {
                  next[tabIdx] = { ...next[tabIdx], attachId };
                }
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
          claude_session_id: activeTab?.sid ?? '',
          attach_id: attachId,
          cwd: projectRootRef.current,
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
              const data = JSON.parse(line.slice(6)) as Record<string, unknown>;
              if (controller.signal.aborted || !activeSendAbortRefs.current.has(controller)) {
                return false;
              }
              handleSseEvent(evtType, data, streamRefs);
            } catch { /* ignore */ }
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
      setStreamMsgs(prev => {
        const aid = streamRefs.assistantId;
        if (aid !== null) {
          return prev.map(m =>
            m.id === aid ? { ...m, text: m.text + `\n\n**Connection error:** ${errMsg}` } : m
          );
        }
        return [...prev, { id: nextId(), type: 'assistant', text: `**Connection error:** ${errMsg}` }];
      });
    } finally {
      activeSendAbortRefs.current.delete(controller);
      setPendingSends(activeSendAbortRefs.current.size);
      setWorking(hasPendingChatWork());
      if (!hasPendingChatWork()) {
        setIterCur(0);
        setIterMax(0);
      }
      streamRefs.assistantId = null;
      streamRefs.thinkId = null;
      streamRefs.toolId = null;
      textareaRef.current?.focus();
    }
  }

  function handleSseEvent(type: string, data: Record<string, unknown>, streamRefs: ActiveStreamRefs) {
    switch (type) {
      case 'turn_start': {
        setWorking(hasPendingChatWork());
        const aid = nextId();
        streamRefs.assistantId = aid;
        streamRefs.thinkId = null;
        streamRefs.toolId = null;
        setStreamMsgs(prev => [...prev, { id: aid, type: 'assistant', text: '' }]);
        break;
      }
      case 'tool_start': {
        const toolName = String(data.name ?? '');
        const toolArgs = String(data.args ?? '');
        const tid = nextId();
        streamRefs.toolId = tid;
        setStreamMsgs(prev => [...prev, {
          id: tid, type: 'tool', text: '',
          toolName, toolArgs, toolResult: undefined,
        }]);
        break;
      }
      case 'tool_result': {
        const tid = streamRefs.toolId;
        if (tid !== null) {
          const toolResult = String(data.result ?? '');
          setStreamMsgs(prev => prev.map(m =>
            m.id === tid ? { ...m, toolResult } : m
          ));
          streamRefs.toolId = null;
        }
        break;
      }
      case 'thinking': {
        const content = String(data.content ?? '');
        if (streamRefs.thinkId === null) {
          const tid = nextId();
          streamRefs.thinkId = tid;
          setStreamMsgs(prev => {
            const aid = streamRefs.assistantId;
            const idx = aid !== null ? prev.findIndex(m => m.id === aid) : -1;
            const thinkMsg: StreamMsg = { id: tid, type: 'thinking', text: '', thinkText: content };
            if (idx > 0) {
              const next = [...prev];
              next.splice(idx, 0, thinkMsg);
              return next;
            }
            return [...prev, thinkMsg];
          });
        } else {
          const tid = streamRefs.thinkId;
          setStreamMsgs(prev =>
            prev.map(m => m.id === tid ? { ...m, thinkText: (m.thinkText ?? '') + content } : m)
          );
        }
        break;
      }
      case 'text': {
        const content = String(data.content ?? '');
        setStreamMsgs(prev => {
          let aid = streamRefs.assistantId;
          if (aid === null) {
            setWorking(hasPendingChatWork());
            aid = nextId();
            streamRefs.assistantId = aid;
            return [...prev, { id: aid, type: 'assistant', text: content }];
          }
          return prev.map(m => m.id === aid ? { ...m, text: m.text + content } : m);
        });
        break;
      }
      case 'turn_end': {
        setWorking(hasPendingChatWork());
        setStreamMsgs(prev => {
          saveCurrentTabMessages(prev);
          return prev;
        });
        streamRefs.assistantId = null;
        streamRefs.thinkId = null;
        streamRefs.toolId = null;
        break;
      }
      case 'error': {
        setWorking(hasPendingChatWork());
        const msg = String(data.message ?? 'Unknown error');
        setStreamMsgs(prev => {
          const aid = streamRefs.assistantId;
          if (aid !== null) {
            return prev.map(m =>
              m.id === aid ? { ...m, text: m.text + `\n\n**Error:** ${msg}` } : m
            );
          }
          return [...prev, { id: nextId(), type: 'assistant', text: `**Error:** ${msg}` }];
        });
        break;
      }
      case 'session': {
        const sid = String(data.id ?? '');
        setTabs(prev => {
          const next = [...prev];
          const idx = activeIdxRef.current;
          if (next[idx]) next[idx] = { ...next[idx], sid };
          tabsRef.current = next;
          return next;
        });
        break;
      }
      case 'iteration': {
        setIterCur(Number(data.iteration ?? 0));
        setIterMax(Number(data.max ?? 0));
        break;
      }
      case 'usage': {
        const u = {
          in: Number(data.in ?? 0),
          out: Number(data.out ?? 0),
          cost: Number(data.cost ?? 0),
        };
        // usage_kind distinguishes realized (provider-reported, billable) spend
        // from estimated/avoided/partial; default realized for the chat path.
        const kind = typeof data.usage_kind === 'string' && data.usage_kind ? data.usage_kind : 'realized';
        setLastUsage(u);
        setSessionUsage(prev => ({ in: prev.in + u.in, out: prev.out + u.out, cost: prev.cost + u.cost, kind }));
        break;
      }
      case 'turn_summary': {
        const text = String(data.text ?? '');
        if (text) {
          setStreamMsgs(prev => [...prev, { id: nextId(), type: 'narration', text }]);
        }
        break;
      }
      case 'diff_output': {
        const path = String(data.path ?? '');
        const diff = String(data.diff ?? '');
        if (diff) {
          setStreamMsgs(prev => [...prev, {
            id: nextId(), type: 'diff', text: '',
            diffPath: path || undefined, diffContent: diff,
          }]);
        }
        break;
      }
      case 'checkpoint': {
        const snapshotId = Number(data.snapshot_id ?? 0);
        if (snapshotId > 0) {
          setStreamMsgs(prev => [...prev, {
            id: nextId(), type: 'checkpoint', text: '', snapshotId,
          }]);
        }
        break;
      }
      case 'done': {
        setStreamMsgs(prev => {
          saveCurrentTabMessages(prev);
          return prev;
        });
        void refreshWorkflow();
        /* Refresh LSP diagnostic badge after each completed turn */
        fetch('/api/lsp/diagnostics/summary')
          .then(r => r.json())
          .then((d: { errors: number; warnings: number; active_servers: number }) => {
            if (d.active_servers > 0) setLspDiag(d);
            else setLspDiag(null);
          })
          .catch(() => {});
        break;
      }
    }
  }

  function handleKeyDown(e: React.KeyboardEvent<HTMLTextAreaElement>) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      sendMessage();
    }
  }

  function handleInput(e: React.ChangeEvent<HTMLTextAreaElement>) {
    setInputText(e.target.value);
    e.target.style.height = 'auto';
    e.target.style.height = Math.min(e.target.scrollHeight, 200) + 'px';
  }

  const projectOptions = projectRoot && !projects.some(project => project.root === projectRoot)
    ? [{ name: projectRoot, root: projectRoot, current: true }, ...projects]
    : projects;

  return (
    <div style={{
      display: 'flex', flexDirection: 'column', height: '100%',
      overflow: 'hidden', background: tokens.surface,
    }}>
      {/* The project belongs to the active SESSION (top tab); picking one here
          binds it to this session, and the agent runs with it as cwd. A Clear
          button resets this conversation (fresh provider session). */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <div style={{ flex: 1, minWidth: 0 }}>
          <ProjectPicker
            key={activeSessionId}
            storageKey={`aimee_session_project_${activeSessionId}`}
            onChange={sel => {
              const r = sel ? `${sel.root}/${sel.project}` : '';
              if (activeSession) patchSession(activeSession.id, { projectRoot: r, projectName: sel?.project ?? '' });
              else { setProjectRoot(r); saveProjectRoot(r); }
            }}
          />
        </div>
        <button
          onClick={clearChat}
          title="Clear this conversation and start a fresh session"
          style={{
            flexShrink: 0, padding: '5px 12px', fontSize: 13, cursor: 'pointer',
            background: '#fff', color: '#666', border: '1px solid #ddd', borderRadius: 6,
          }}
        >
          Clear
        </button>
      </div>

      {/* Thread bar (conversation branching) */}
      <ThreadBar
        threads={tabs[activeIdx]?.threads ?? []}
        activeThreadId={tabs[activeIdx]?.activeThreadId ?? 0}
        onSwitch={switchThread}
        onBranch={branchThread}
      />

      {/* Main content area: channel sidebar + chat + optional rules panel */}
      <div style={{ display: 'flex', flex: 1, overflow: 'hidden', position: 'relative' }}>
        {/* Channel sidebar */}
        <ChannelSidebar
          channels={channels}
          activeChannel={activeChannel}
          unreadCounts={unreadCounts}
          onSelect={selectChannel}
          onDeselect={() => setActiveChannel(null)}
          onCreateChannel={name => { void createChannel(name); }}
        />

        {/* Chat column — shows channel view when a channel is active */}
        <div style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>

        {activeChannel ? (
          <ChannelView
            channelName={activeChannel}
            messages={channelMsgs[activeChannel] ?? []}
            agents={agents}
            onSend={text => { void sendChannelMessage(text); }}
          />
        ) : (<>

      <WorkflowStatusCard
        channel={workflowChannel}
        session={workflowInfo}
        loading={workflowLoading}
        error={workflowError}
        changed={workflowChanged}
        onChannelChange={setCurrentWorkflowChannel}
        onRefresh={() => { void refreshWorkflow(); }}
        onPause={() => { void pauseWorkflow(); }}
      />

      {/* Bootstrap banner */}
      {banner && (
        <BootstrapBanner
          stacks={banner.stacks}
          onGenerate={generateRules}
          onDismiss={dismissRulesBanner}
        />
      )}

      {/* Messages */}
      <div style={{
        flex: 1, overflowY: 'auto', padding: '16px',
        display: 'flex', flexDirection: 'column', gap: '12px',
      }}>
        {streamMsgs.map(m => {
          if (m.type === 'user') {
            return <Message key={m.id} role="user" html={escHtml(m.text)} />;
          }
          if (m.type === 'assistant') {
            return <Message key={m.id} role="assistant" html={renderMd(m.text)} />;
          }
          if (m.type === 'thinking') {
            return <ThinkingBlock key={m.id} text={m.thinkText ?? ''} />;
          }
          if (m.type === 'tool') {
            return (
              <ToolBlock
                key={m.id}
                name={m.toolName ?? ''}
                args={m.toolArgs ?? ''}
                result={m.toolResult}
              />
            );
          }
          if (m.type === 'narration') {
            return <TurnSummaryCard key={m.id} text={m.text} />;
          }
          if (m.type === 'diff') {
            return <DiffBlock key={m.id} path={m.diffPath} diff={m.diffContent ?? ''} />;
          }
          if (m.type === 'checkpoint') {
            return <RewindMarker key={m.id} snapshotId={m.snapshotId!} sid={tabs[activeIdx]?.aimeeSid ?? ''} />;
          }
          return null;
        })}
        {working && <WorkingIndicator />}
        {remoteTurnActive && !working && (
          <div style={{
            alignSelf: 'flex-start', maxWidth: '85%', padding: '6px 10px',
            background: tokens.borderLight, border: `1px solid ${tokens.borderMedium}`,
            borderRadius: '10px', color: tokens.textFaint, fontSize: '11px', fontFamily: 'system-ui',
          }}>
            <div>● aimee is active on another surface…</div>
            {remoteTurnText && (
              <div style={{
                marginTop: '4px', color: tokens.textSecondary, whiteSpace: 'pre-wrap',
                maxHeight: '8em', overflowY: 'auto', fontSize: '12px',
              }}>
                {remoteTurnText}
              </div>
            )}
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
                  background: '#3a1a1a', color: '#ff6b6b', border: '1px solid #5a2a2a',
                }} title="LSP errors">
                  ✕ {lspDiag.errors}
                </span>
              )}
              {lspDiag.warnings > 0 && (
                <span style={{
                  padding: '1px 5px', borderRadius: '8px', fontSize: '10px',
                  background: '#2a2a1a', color: '#ffd93d', border: '1px solid #4a4a2a',
                }} title="LSP warnings">
                  ⚠ {lspDiag.warnings}
                </span>
              )}
            </span>
          )}
        </div>
      )}

      {/* Prompt tier selector */}
      <div style={{
        padding: '4px 16px', background: tokens.surfaceAlt,
        borderTop: `1px solid ${tokens.borderLight}`,
        display: 'flex', alignItems: 'center', gap: '6px', flexShrink: 0,
      }}>
        <span style={{ fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui' }}>
          Project:
        </span>
        <select
          value={projectRoot}
          onChange={e => changeProjectRoot(e.target.value)}
          title={projectRoot || 'Project root'}
          style={{
            fontSize: '11px', fontFamily: 'system-ui', maxWidth: '240px',
            background: tokens.surface, color: tokens.textPale,
            border: `1px solid ${tokens.borderLight}`, borderRadius: '10px',
            cursor: 'pointer', padding: '2px 6px', outline: 'none',
          }}
        >
          {(!projectRoot || projectOptions.length === 0) && <option value="">Server cwd</option>}
          {projectOptions.map(project => (
            <option key={project.root} value={project.root}>
              {projectDisplayName(project)}
            </option>
          ))}
        </select>
        <span style={{ fontSize: '11px', color: tokens.borderLight, fontFamily: 'system-ui', marginLeft: '4px' }}>|</span>
        <span style={{ fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui' }}>
          Prompt:
        </span>
        {(['MINIMAL', 'STANDARD', 'EXTENDED'] as const).map(tier => (
          <button
            key={tier}
            onClick={() => changePromptTier(tier)}
            style={{
              padding: '2px 8px', fontSize: '11px', fontFamily: 'system-ui',
              background: promptTier === tier ? tokens.primary : 'transparent',
              color: promptTier === tier ? tokens.surface : tokens.textFaint,
              border: `1px solid ${promptTier === tier ? tokens.primary : tokens.borderLight}`,
              borderRadius: '10px', cursor: 'pointer',
            }}
          >
            {tier.charAt(0) + tier.slice(1).toLowerCase()}
          </button>
        ))}
        {availableSkills.length > 0 && (
          <>
            <span style={{ fontSize: '11px', color: tokens.borderLight, fontFamily: 'system-ui', marginLeft: '4px' }}>|</span>
            <span style={{ fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui' }}>
              Skill:
            </span>
            <select
              value={activeSkill}
              onChange={e => changeSkill(e.target.value)}
              style={{
                fontSize: '11px', fontFamily: 'system-ui',
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
          </>
        )}
        {availablePersonas.length > 0 && (
          <>
            <span style={{ fontSize: '11px', color: tokens.borderLight, fontFamily: 'system-ui', marginLeft: '4px' }}>|</span>
            <span style={{ fontSize: '11px', color: tokens.textFaint, fontFamily: 'system-ui' }}>
              Persona:
            </span>
            <select
              value={activePersona}
              onChange={e => changePersona(e.target.value)}
              title="Steers Aimee's role and delegate policy for this session"
              style={{
                fontSize: '11px', fontFamily: 'system-ui',
                background: activePersona ? tokens.primary : tokens.surface,
                color: activePersona ? tokens.surface : tokens.textFaint,
                border: `1px solid ${activePersona ? tokens.primary : tokens.borderLight}`,
                borderRadius: '10px', cursor: 'pointer', padding: '2px 6px',
                outline: 'none',
              }}
            >
              {availablePersonas.map(p => (
                <option key={p.name} value={p.name} title={p.description}>{p.name}</option>
              ))}
            </select>
          </>
        )}
      </div>

      {/* Input area */}
      <div style={{
        padding: '12px 16px', background: tokens.surfaceAlt, borderTop: `1px solid ${tokens.border}`,
        display: 'flex', gap: '8px', flexShrink: 0,
      }}>
        <textarea
          ref={textareaRef}
          value={inputText}
          onChange={handleInput}
          onKeyDown={handleKeyDown}
          placeholder="Type a message… (Shift+Enter for newline)"
          rows={1}
          style={{
            flex: 1, padding: '10px', backgroundColor: tokens.surface, color: tokens.text,
            border: `1px solid ${tokens.borderMedium}`, borderRadius: '6px', fontSize: '14px',
            resize: 'none', fontFamily: 'system-ui', minHeight: '44px', maxHeight: '200px',
            outline: 'none',
          }}
          onFocus={e => (e.target.style.borderColor = tokens.primary)}
          onBlur={e => (e.target.style.borderColor = tokens.borderMedium)}
          autoFocus
        />
        <button
          onClick={sendMessage}
          aria-busy={queueActive}
          style={{
            padding: '10px 20px', background: tokens.primary,
            color: tokens.surface, border: 'none', borderRadius: '6px',
            cursor: 'pointer', fontSize: '14px', whiteSpace: 'nowrap',
          }}
        >
          {queueActive ? (
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: '8px' }}>
              <Spinner loading text="" />
              Send
            </span>
          ) : 'Send'}
        </button>
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

        <PluginsPanel
          open={pluginsOpen}
          plugins={plugins}
          loading={pluginsLoading}
          error={pluginsError}
          onToggle={() => { setPluginsOpen(o => !o); setRulesOpen(false); setContextOpen(false); }}
          onRefresh={() => { void refreshPlugins(); }}
          onPluginToggle={name => { void togglePlugin(name); }}
        />

        {/* Rules sidebar */}
        <RulesPanel open={rulesOpen} onToggle={() => { setRulesOpen(o => !o); setPluginsOpen(false); setContextOpen(false); }} />
      </div>{/* end main content row */}
    </div>
  );
}
