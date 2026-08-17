import { Component, useEffect, useRef, useState } from 'react';
import type { ErrorInfo, ReactNode } from 'react';
import { Routes, Route, Navigate, NavLink, useLocation } from 'react-router-dom';
import { Toast, ThemeToggle } from '@rakuensoftware/smoothgui';
import Chat from './pages/Chat';
import Dashboard from './pages/Dashboard';
import Logs from './pages/Logs';
import EditWorkflows from './pages/EditWorkflows';
import WorkflowActions from './pages/WorkflowActions';
import Providers from './pages/Providers';
import Models from './pages/Models';
import Personas from './pages/Personas';
import Roles from './pages/Roles';
import Roundtable from './pages/Roundtable';
import Settings from './pages/Settings';
import Projects from './pages/Projects';
import Graph from './pages/Graph';
import Editor from './pages/Editor';
import { SessionProvider, useSessions } from './SessionContext';
import TabTutorial from './components/TabTutorial';
import SetupChip from './components/SetupChip';
import HealthBanner from './components/HealthBanner';
import SetupWizard from './components/SetupWizard';
// Silent-by-default error boundary for optional chrome (setup chip, tab
// tutorial, setup wizard): renders NOTHING on error so a broken overlay is
// dropped instead of unmounting the shell. Shared smoothgui ErrorBoundary,
// aliased to keep the local call-site name and make the silent contract explicit.
import { ErrorBoundary as SilentBoundary } from '@rakuensoftware/smoothgui';
import { OPEN_WIZARD_EVENT } from './setup/setupState';
import { NAV_ITEMS } from './nav';

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
        <div style={{ padding: '24px', fontFamily: 'system-ui', color: 'var(--sg-text-muted)' }}>
          <div style={{ fontSize: '15px', fontWeight: 600, color: 'var(--sg-danger-dark)', marginBottom: '8px' }}>
            This page hit an error and couldn’t render.
          </div>
          <div style={{ fontSize: '13px', color: 'var(--sg-text-faint)', marginBottom: '12px' }}>
            Other pages still work — use the navigation to switch. Details below.
          </div>
          <pre style={{
            fontSize: '12px', color: 'var(--sg-danger)', background: 'var(--sg-danger-bg)', border: '1px solid var(--sg-danger-bg)',
            borderRadius: '6px', padding: '10px', overflow: 'auto', whiteSpace: 'pre-wrap',
          }}>
            {this.state.error.message}
          </pre>
          <button
            onClick={() => this.setState({ error: null })}
            style={{
              marginTop: '12px', padding: '6px 14px', background: 'var(--sg-surface)', color: 'var(--sg-text-secondary)',
              border: '1px solid var(--sg-border-medium)', borderRadius: '4px', cursor: 'pointer', fontSize: '13px',
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
              background: isActive ? 'var(--sg-dark-surface-alt)' : 'transparent',
              color: isActive ? 'var(--sg-sidebar-text)' : 'var(--sg-text-faint)', border: '1px solid', borderColor: isActive ? 'var(--sg-dark-surface-alt)' : 'transparent',
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
                style={{ width: 110, background: 'var(--sg-dark-surface)', color: 'var(--sg-sidebar-text)', border: '1px solid var(--sg-dark-surface-alt)', borderRadius: 4, fontSize: 13, padding: '1px 4px' }}
              />
            ) : (
              <span style={{ overflow: 'hidden', textOverflow: 'ellipsis' }}>
                {s.name}{s.projectName ? <span style={{ color: 'var(--sg-text-secondary)', marginLeft: 5 }}>· {s.projectName}</span> : null}
              </span>
            )}
            <span
              onClick={e => { e.stopPropagation(); closeSession(s.id); }}
              title="Close session"
              style={{ color: 'var(--sg-text-secondary)', fontSize: 14, lineHeight: 1, padding: '0 2px' }}
              onMouseOver={e => (e.currentTarget.style.color = 'var(--sg-danger)')}
              onMouseOut={e => (e.currentTarget.style.color = 'var(--sg-text-secondary)')}
            >×</span>
          </div>
        );
      })}
      <button
        onClick={() => addSession()}
        title="New session"
        style={{ background: 'transparent', color: 'var(--sg-primary)', border: '1px dashed var(--sg-dark-surface-alt)', borderRadius: 6, cursor: 'pointer', fontSize: 16, lineHeight: 1, padding: '3px 9px' }}
      >+</button>
    </div>
  );
}

function LogoutButton() {

  function handleLogout(e: React.MouseEvent) {
    e.preventDefault();
    localStorage.removeItem('aimee_chat_tabs');
    localStorage.removeItem('aimee_active_chat_tab');
    localStorage.removeItem('aimee_sessions');
    localStorage.removeItem('aimee_active_session');
    localStorage.removeItem('aimee_server_sessions_authoritative_v1');
    localStorage.removeItem('aimee_sessions_owner');
    localStorage.removeItem('aimee_proposal_draft');
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
      style={{ color: 'var(--sg-text-secondary)', fontSize: '13px', textDecoration: 'none' }}
      onMouseOver={e => (e.currentTarget.style.color = 'var(--sg-text)')}
      onMouseOut={e => (e.currentTarget.style.color = 'var(--sg-text-secondary)')}
    >
      Logout
    </a>
  );
}

export default function App() {
  const [ready, setReady] = useState(false);
  const [wizardOpen, setWizardOpen] = useState(false);
  const location = useLocation();

  // The setup chip / "Re-run setup" dispatch this event to open the wizard.
  useEffect(() => {
    const openWizard = () => setWizardOpen(true);
    window.addEventListener(OPEN_WIZARD_EVENT, openWizard);
    return () => window.removeEventListener(OPEN_WIZARD_EVENT, openWizard);
  }, []);

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
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100vh', color: 'var(--sg-text-faint)', fontFamily: 'system-ui' }}>
        Loading…
      </div>
    );
  }

  return (
    <SessionProvider>
      <div style={{ display: 'flex', flexDirection: 'column', height: '100vh' }}>
        {/* Top bar: brand + session tabs + logout. Runtime options live on the
            Settings page and the tabs that own them — there is no gear menu. */}
        <header
          style={{
            display: 'flex', alignItems: 'center', height: '46px', flexShrink: 0, gap: 14,
            background: 'var(--sg-dark-surface)', borderBottom: '1px solid var(--sg-dark-border)', padding: '0 14px',
          }}
        >
          <span style={{ color: 'var(--sg-primary)', fontWeight: 700, fontSize: 18 }}>aimee</span>
          <SessionTabBar />
          <SilentBoundary><SetupChip /></SilentBoundary>
          {/* Follows the OS until clicked; the first click pins an explicit
              choice and persists it (smoothgui owns that logic and the storage
              key the pre-paint script in index.html reads). */}
          <SilentBoundary><ThemeToggle /></SilentBoundary>
          <LogoutButton />
        </header>
        <SilentBoundary><HealthBanner /></SilentBoundary>
        {/* Body: vertical tool nav (left) + content. */}
        <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
          <nav
            style={{
              display: 'flex', flexDirection: 'column', gap: 2, width: 132, flexShrink: 0,
              background: 'var(--sg-dark-surface)', borderRight: '1px solid var(--sg-dark-border)', padding: '8px 6px',
            }}
          >
            {NAV_ITEMS.map(it => (
              <NavLink
                key={it.route}
                to={it.route}
                title={it.hint}
                style={({ isActive }) => ({
                  display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px', borderRadius: 6,
                  fontSize: 14, textDecoration: 'none',
                  color: isActive ? 'var(--sg-sidebar-text)' : 'var(--sg-text-faint)',
                  background: isActive ? 'var(--sg-dark-surface-alt)' : 'transparent',
                  fontWeight: isActive ? 600 : 400,
                })}
              >
                <span aria-hidden style={{ fontSize: 16 }}>{it.icon}</span> {it.label}
              </NavLink>
            ))}
          </nav>
          {/* Content: flex:1 + minHeight:0 so pages using height:100% resolve.
           * position:relative anchors the per-tab tutorial overlay/"?" button. */}
          <main style={{ position: 'relative', flex: 1, minWidth: 0, minHeight: 0, overflow: 'auto', background: 'var(--sg-surface)' }}>
            <SilentBoundary><TabTutorial route={location.pathname} /></SilentBoundary>
            <ErrorBoundary key={location.pathname}>
              <Routes>
                <Route path="/chat" element={<Chat />} />
                <Route path="/dashboard" element={<Dashboard />} />
                <Route path="/logs" element={<Logs />} />
                <Route path="/edit-workflows" element={<EditWorkflows />} />
                <Route path="/workflow-actions" element={<WorkflowActions />} />
                <Route path="/providers" element={<Providers />} />
                <Route path="/models" element={<Models />} />
                {/* Pre-rename routes: the roster tab was "Agents" (and "delegates"
                    before that). Keep both redirecting so bookmarks survive. */}
                <Route path="/agents" element={<Navigate to="/models" replace />} />
                <Route path="/delegates" element={<Navigate to="/models" replace />} />
                <Route path="/personas" element={<Personas />} />
                <Route path="/roles" element={<Roles />} />
                <Route path="/roundtable" element={<Roundtable />} />
                <Route path="/projects" element={<Projects />} />
                <Route path="/graph" element={<Graph />} />
                <Route path="/editor" element={<Editor />} />
                <Route path="/settings" element={<Settings />} />
                <Route path="*" element={<Navigate to="/chat" replace />} />
              </Routes>
            </ErrorBoundary>
          </main>
        </div>
      </div>
      <SilentBoundary><SetupWizard open={wizardOpen} onClose={() => setWizardOpen(false)} /></SilentBoundary>
      <Toast />
    </SessionProvider>
  );
}
