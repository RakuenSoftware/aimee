import { Component, useEffect, useState } from 'react';
import type { ErrorInfo, ReactNode } from 'react';
import { Routes, Route, Navigate, NavLink, useLocation } from 'react-router-dom';
import { Toast } from '@rakuensoftware/smoothgui';
import Chat from './pages/Chat';
import Dashboard from './pages/Dashboard';
import Workflows from './pages/Workflows';
import Projects from './pages/Projects';
import Editor from './pages/Editor';

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
    <>
      <div style={{ display: 'flex', flexDirection: 'column', height: '100vh' }}>
        {/* Top navigation: one tab per tool. */}
        <header
          style={{
            display: 'flex', alignItems: 'stretch', height: '48px', flexShrink: 0,
            background: '#13131f', borderBottom: '1px solid #2a2a3a', padding: '0 14px',
          }}
        >
          <span style={{ color: '#8cf', fontWeight: 700, fontSize: 18, alignSelf: 'center', marginRight: 22 }}>aimee</span>
          <nav style={{ display: 'flex', gap: 2, flex: 1 }}>
            {NAV_ITEMS.map(it => (
              <NavLink
                key={it.route}
                to={it.route}
                style={({ isActive }) => ({
                  display: 'flex', alignItems: 'center', gap: 6, padding: '0 14px',
                  fontSize: 14, textDecoration: 'none', borderBottom: '2px solid transparent',
                  color: isActive ? '#8cf' : '#aab', borderBottomColor: isActive ? '#8cf' : 'transparent',
                  fontWeight: isActive ? 600 : 400,
                })}
              >
                <span aria-hidden>{it.icon}</span> {it.label}
              </NavLink>
            ))}
          </nav>
          <div style={{ alignSelf: 'center' }}><LogoutButton /></div>
        </header>
        {/* Content area: flex:1 + minHeight:0 so pages using height:100% resolve. */}
        <main style={{ flex: 1, minHeight: 0, overflow: 'auto', background: '#fff' }}>
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
      <Toast />
    </>
  );
}
