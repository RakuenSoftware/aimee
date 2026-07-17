import { useCallback, useEffect, useState } from 'react';
import { Badge, Button, Panel } from '@rakuensoftware/smoothgui';
import { useSessions } from '../SessionContext';

/* §8 code-graph visualization. A read-only, navigable view of the code projection
 * graph for the active session's project, backed by /api/graph/* (which forward
 * aimee-server's index_graph_* MCP tools). It is an adjacency explorer rather than a
 * force-directed canvas: rank the hubs, click one to expand its callers/callees/
 * neighbors with provenance, drill into any neighbor, and surface "surprising links"
 * (semantically close yet structurally far file pairs). Off the agent's hot path. */

async function api(path: string): Promise<Response> {
  return fetch(path, { headers: { 'X-CSRF-Token': window._csrf || '' } });
}

interface Hub { node: string; degree: number; in_degree: number; out_degree: number; weighted_degree: number; }
interface Neighbor { neighbor: string; relation: string; direction: string; structural_weight: number; provenance: string; }
interface Link { a: string; b: string; cosine: number; hops: number; disconnected: boolean; confirmed?: boolean; reason?: string; shared_symbols?: number; }

const dirVariant = (d: string): 'success' | 'info' | 'neutral' =>
  d === 'out' ? 'info' : d === 'in' ? 'success' : 'neutral';

export default function Graph() {
  const { active } = useSessions();
  const project = active?.projectName || '';

  const [hubs, setHubs] = useState<Hub[]>([]);
  const [edgeCount, setEdgeCount] = useState<number | null>(null);
  const [node, setNode] = useState<string>('');
  const [neighbors, setNeighbors] = useState<Neighbor[]>([]);
  const [links, setLinks] = useState<Link[]>([]);
  const [judge, setJudge] = useState(false);
  const [err, setErr] = useState<string>('');
  const [loading, setLoading] = useState(false);

  const loadHubs = useCallback(async () => {
    if (!project) return;
    setErr(''); setLoading(true); setNode(''); setNeighbors([]);
    try {
      const r = await api(`/api/graph/hubs?project=${encodeURIComponent(project)}&max_results=30`);
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'could not load hubs'); setHubs([]); return; }
      setHubs(d.hubs || []);
      setEdgeCount(typeof d.edge_count === 'number' ? d.edge_count : null);
    } catch (e) { setErr(String(e)); } finally { setLoading(false); }
  }, [project]);

  const loadNeighbors = useCallback(async (n: string) => {
    if (!project || !n) return;
    setErr(''); setLoading(true); setNode(n);
    try {
      const r = await api(`/api/graph/neighbors?project=${encodeURIComponent(project)}&node=${encodeURIComponent(n)}&max_results=50`);
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'could not load neighbors'); setNeighbors([]); return; }
      setNeighbors(d.neighbors || []);
    } catch (e) { setErr(String(e)); } finally { setLoading(false); }
  }, [project]);

  const loadSurprising = useCallback(async () => {
    if (!project) return;
    setErr(''); setLoading(true);
    try {
      const r = await api(`/api/graph/surprising?project=${encodeURIComponent(project)}&max_results=20${judge ? '&judge=true' : ''}`);
      const d = await r.json();
      if (!r.ok) { setErr(d.error || 'could not load surprising links'); setLinks([]); return; }
      setLinks(d.links || []);
    } catch (e) { setErr(String(e)); } finally { setLoading(false); }
  }, [project, judge]);

  useEffect(() => { loadHubs(); setLinks([]); }, [loadHubs]);

  if (!project) {
    return <div style={{ padding: 24, color: '#666' }}>Bind this session to a project to explore its code graph.</div>;
  }

  return (
    <div style={{ padding: 16, display: 'flex', flexDirection: 'column', gap: 12, overflowY: 'auto' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <strong>Code graph — {project}</strong>
        {edgeCount != null && <Badge label={`${edgeCount} edges`} variant="neutral" />}
        {loading && <span style={{ color: '#888', fontSize: 12 }}>loading…</span>}
        <Button size="sm" onClick={loadHubs} style={{ marginLeft: 'auto' }} title="Reload the ranked hub list for this project's code graph.">↻ refresh</Button>
      </div>
      {err && <div style={{ color: '#b00', fontSize: 13 }}>{err}</div>}

      <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap', alignItems: 'flex-start' }}>
        <Panel title="Hubs" count={hubs.length}>
          <div style={{ display: 'flex', flexDirection: 'column', gap: 2, maxHeight: 420, overflowY: 'auto' }}>
            {hubs.length === 0 && <div style={empty}>No projection graph yet — index this project first.</div>}
            {hubs.map(h => (
              <button key={h.node} onClick={() => loadNeighbors(h.node)} title={h.node}
                style={{ ...row, fontWeight: h.node === node ? 600 : 400, background: h.node === node ? '#eef4ff' : '#fff' }}>
                <span style={ellipsis}>{h.node}</span>
                <Badge label={String(h.degree)} variant="info" />
              </button>
            ))}
          </div>
        </Panel>

        <Panel title={node ? `Neighbors of ${shortNode(node)}` : 'Neighbors'} count={neighbors.length}>
          <div style={{ display: 'flex', flexDirection: 'column', gap: 2, maxHeight: 420, overflowY: 'auto', minWidth: 280 }}>
            {!node && <div style={empty}>Click a hub to see its callers / callees / neighbors.</div>}
            {node && neighbors.length === 0 && <div style={empty}>No incident edges.</div>}
            {neighbors.map((nb, i) => (
              <button key={`${nb.neighbor}-${i}`} onClick={() => loadNeighbors(nb.neighbor)} title={nb.neighbor} style={row}>
                <Badge label={nb.direction} variant={dirVariant(nb.direction)} />
                <span style={{ color: '#888', fontSize: 11 }}>{nb.relation}</span>
                <span style={{ ...ellipsis, flex: 1 }}>{nb.neighbor}</span>
                <Badge label={nb.provenance} variant="neutral" />
              </button>
            ))}
          </div>
        </Panel>
      </div>

      <Panel title="Surprising links" count={links.length}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
          <Button size="sm" onClick={loadSurprising} title="Find file pairs that are semantically close yet structurally far apart.">find</Button>
          <label style={{ fontSize: 12, color: '#555' }} title="Run an LLM to confirm or reject each surprising link (slower).">
            <input type="checkbox" checked={judge} onChange={e => setJudge(e.target.checked)} /> LLM confirm
          </label>
          <span style={{ fontSize: 11, color: '#999' }}>semantically close yet structurally far</span>
        </div>
        <div style={{ display: 'flex', flexDirection: 'column', gap: 3, maxHeight: 300, overflowY: 'auto' }}>
          {links.map((l, i) => (
            <div key={i} style={{ ...row, cursor: 'default', alignItems: 'baseline', flexWrap: 'wrap' }}>
              <span style={ellipsis}>{l.a}</span><span style={{ color: '#bbb' }}>↔</span><span style={ellipsis}>{l.b}</span>
              <Badge label={`cos ${l.cosine.toFixed(2)}`} variant="info" />
              <Badge label={l.disconnected ? 'disconnected' : `${l.hops} hops`} variant="neutral" />
              {l.confirmed != null && <Badge label={l.confirmed ? 'confirmed' : 'rejected'} variant={l.confirmed ? 'success' : 'neutral'} />}
              {l.reason && <span style={{ fontSize: 11, color: '#888' }}>{l.reason}</span>}
            </div>
          ))}
        </div>
      </Panel>
    </div>
  );
}

function shortNode(n: string): string {
  const i = n.lastIndexOf(':');
  return i >= 0 ? n.slice(i + 1) : n;
}

const row: React.CSSProperties = { display: 'flex', alignItems: 'center', gap: 6, padding: '4px 6px', borderRadius: 4, border: 'none', background: '#fff', cursor: 'pointer', fontSize: 12, textAlign: 'left', width: '100%' };
const ellipsis: React.CSSProperties = { whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', maxWidth: 240 };
const empty: React.CSSProperties = { color: '#999', fontSize: 12, padding: 8 };
