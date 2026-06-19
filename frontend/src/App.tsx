import { Component, useEffect, useRef, useState } from 'react';
import type { ErrorInfo, ReactNode } from 'react';
import { Routes, Route, Navigate, NavLink, useLocation } from 'react-router-dom';
import { Toast } from '@rakuensoftware/smoothgui';
import Chat from './pages/Chat';
import Dashboard from './pages/Dashboard';
import Workflows from './pages/Workflows';
import Projects from './pages/Projects';
import Editor from './pages/Editor';
import { SessionProvider, useSessions } from './SessionContext';
import SettingsPanel from './components/SettingsPanel';

// A render error in any page used to throw past the root and unmount the whole
// app, leaving a blank screen (the AppShell, nav, and other pages vanished too).
// This boundary contains the failure to the page being rendered so the shell and
// other routes keep working, and surfaces the error instead of a blank page.
class ErrorBoundary extends Component<{ children: ReactNode }, { error: Error | null }> {
  state: { error: Error | null } = { error: null };

  static getDerivedStateFromError(error: Error) {
    return { error };
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('Page render error', error, info);
  }

  render() {
    if (this.state.error) {
      return (
        <div style={{ padding: '24px', fontFamily: 'system-ui', color: '#555' }}>
          <div style={{ fontSize: '15px', fontWeight: 600, color: '#c62828', marginBottom: '8px' }}>
            This page hit an error and couldn’t render.
          </div>
          <div style={{ fontSize: '13px', color: '#888', marginBottom: '12px' }}>
            Other pages still work — use the navigation to switch. Details below.
          </div>
          <pre style={{
            fontSize: '12px', color: '#a33', background: '#fff3f3', border: '1px solid #ffd2d2',
            borderRadius: '6px', padding: '10px', overflow: 'auto', whiteSpace: 'pre-wrap',
          }}>
            {this.state.error.message}
          </pre>
          <button
            onClick={() => this.setState({ error: null })}
            style={{
              marginTop: '12px', padding: '6px 14px', background: '#fff', color: '#666',
              border: '1px solid #ddd', borderRadius: '4px', cursor: 'pointer', fontSize: '13px',
            }}
          >
            Retry
          </button>
        </div>
      );
    }
    return this.props.children;
  }
}

type Tab = { label: string; icon: string; route: string };
const NAV_ITEMS: Tab[] = [
  { label: 'Chat', icon: '💬', route: '/chat' },
  { label: 'Dashboard', icon: '📊', route: '/dashboard' },
  { label: 'Workflows', icon: '🔀', route: '/workflows' },
  { label: 'Projects', icon: '📁', route: '/projects' },
  { label: 'Editor', icon: '🖥️', route: '/editor' },
];

/* Top bar: one tab per session. A session bundles a chat + the project it runs
 * in; every tool operates on the active session's project. */
function SessionTabBar() {
  const { sessions, activeId, addSession, closeSession, selectSession, renameSession } = useSessions();
  const [editing, setEditing] = useState<string>('');
  const editRef = useRef<HTMLInputElement>(null);

  useEffect(() => { if (editing) editRef.current?.focus(); }, [editing]);

  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 4, flex: 1, overflowX: 'auto' }}>
      {sessions.map(s => {
        const isActive = s.id === activeId;
        return (
          <div
            key={s.id}
            onClick={() => selectSession(s.id)}
            onDoubleClick={() => setEditing(s.id)}
            title={s.projectName ? `${s.name} · ${s.projectName}` : s.name}
            style={{
              display: 'flex', alignItems: 'center', gap: 6, padding: '4px 8px', borderRadius: 6,
              cursor: 'pointer', fontSize: 13, whiteSpace: 'nowrap', maxWidth: 220,
              background: isActive ? '#23233a' : 'transparent',
              color: isActive ? '#cde' : '#889', border: '1px solid', borderColor: isActive ? '#3a3a55' : 'transparent',
            }}
          >
            {editing === s.id ? (
              <input
                ref={editRef}
                defaultValue={s.name}
                onClick={e => e.stopPropagation()}
                onBlur={e => { renameSession(s.id, e.target.value); setEditing(''); }}
                onKeyDown={e => {
                  if (e.key === 'Enter') { renameSession(s.id, (e.target as HTMLInputElement).value); setEditing(''); }
                  if (e.key === 'Escape') setEditing('');
                }}
                style={{ width: 110, background: '#13131f', color: '#cde', border: '1px solid #3a3a55', borderRadius: 4, fontSize: 13, padding: '1px 4px' }}
              />
            ) : (
              <span style={{ overflow: 'hidden', textOverflow: 'ellipsis' }}>
                {s.name}{s.projectName ? <span style={{ color: '#667', marginLeft: 5 }}>· {s.projectName}</span> : null}
              </span>
            )}
            <span
              onClick={e => { e.stopPropagation(); closeSession(s.id); }}
              title="Close session"
              style={{ color: '#667', fontSize: 14, lineHeight: 1, padding: '0 2px' }}
              onMouseOver={e => (e.currentTarget.style.color = '#e88')}
              onMouseOut={e => (e.currentTarget.style.color = '#667')}
            >×</span>
          </div>
        );
      })}
      <button
        onClick={() => addSession()}
        title="New session"
        style={{ background: 'transparent', color: '#8cf', border: '1px dashed #3a3a55', borderRadius: 6, cursor: 'pointer', fontSize: 16, lineHeight: 1, padding: '3px 9px' }}
      >+</button>
    </div>
  );
}

function LogoutButton() {

  function handleLogout(e: React.MouseEvent) {
    e.preventDefault();
    localStorage.removeItem('aimee_chat_tabs');
    localStorage.removeItem('aimee_active_chat_tab');
    fetch('/logout', {
      method: 'POST',
      headers: { 'X-CSRF-Token': window._csrf || '' },
    }).finally(() => {
      window.location.href = '/login';
    });
  }

  return (
    <a
      href="/logout"
      onClick={handleLogout}
      style={{ color: '#666', fontSize: '13px', textDecoration: 'none' }}
      onMouseOver={e => (e.currentTarget.style.color = '#333')}
      onMouseOut={e => (e.currentTarget.style.color = '#666')}
    >
      Logout
    </a>
  );
}

export default function App() {
  const [ready, setReady] = useState(false);
  const location = useLocation();

  useEffect(() => {
    fetch('/api/chat/session')
      .then(r => {
        if (r.status === 401) {
          window.location.href = '/login';
          return null;
        }
        return r.json();
      })
      .then((data: { csrf?: string } | null) => {
        if (data?.csrf) window._csrf = data.csrf;
        setReady(true);
      })
      .catch(() => {
        window.location.href = '/login';
      });
  }, []);

  if (!ready) {
    return (
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100vh', color: '#888', fontFamily: 'system-ui' }}>
        Loading…
      </div>
    );
  }

  return (
    <SessionProvider>
      <div style={{ display: 'flex', flexDirection: 'column', height: '100vh' }}>
        {/* Top bar: brand + session tabs + logout. */}
        <header
          style={{
            display: 'flex', alignItems: 'center', height: '46px', flexShrink: 0, gap: 14,
            background: '#13131f', borderBottom: '1px solid #2a2a3a', padding: '0 14px',
          }}
        >
          <span style={{ color: '#8cf', fontWeight: 700, fontSize: 18 }}>aimee</span>
          <SessionTabBar />
          <SettingsPanel />
          <LogoutButton />
        </header>
        {/* Body: vertical tool nav (left) + content. */}
        <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
          <nav
            style={{
              display: 'flex', flexDirection: 'column', gap: 2, width: 132, flexShrink: 0,
              background: '#1a1a28', borderRight: '1px solid #2a2a3a', padding: '8px 6px',
            }}
          >
            {NAV_ITEMS.map(it => (
              <NavLink
                key={it.route}
                to={it.route}
                style={({ isActive }) => ({
                  display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', borderRadius: 6,
                  fontSize: 14, textDecoration: 'none',
                  color: isActive ? '#cde' : '#889',
                  background: isActive ? '#23233a' : 'transparent',
                  fontWeight: isActive ? 600 : 400,
                })}
              >
                <span aria-hidden style={{ fontSize: 16 }}>{it.icon}</span> {it.label}
              </NavLink>
            ))}
          </nav>
          {/* Content: flex:1 + minHeight:0 so pages using height:100% resolve. */}
          <main style={{ flex: 1, minWidth: 0, minHeight: 0, overflow: 'auto', background: '#fff' }}>
            <ErrorBoundary key={location.pathname}>
              <Routes>
                <Route path="/chat" element={<Chat />} />
                <Route path="/dashboard" element={<Dashboard />} />
                <Route path="/workflows" element={<Workflows />} />
                <Route path="/projects" element={<Projects />} />
                <Route path="/editor" element={<Editor />} />
                <Route path="*" element={<Navigate to="/chat" replace />} />
              </Routes>
            </ErrorBoundary>
          </main>
        </div>
      </div>
      <Toast />
    </SessionProvider>
  );
}
