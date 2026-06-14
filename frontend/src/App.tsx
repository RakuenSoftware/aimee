import { useEffect, useState } from 'react';
import { Routes, Route, Navigate } from 'react-router-dom';
import { AppShell, Toast } from '@rakuensoftware/smoothgui';
import type { NavItem } from '@rakuensoftware/smoothgui';
import Chat from './pages/Chat';
import Dashboard from './pages/Dashboard';
import Workflows from './pages/Workflows';

const NAV_ITEMS: NavItem[] = [
  { label: 'Chat', icon: '💬', route: '/chat', section: 'Main' },
  { label: 'Dashboard', icon: '📊', route: '/dashboard', section: 'Main' },
  { label: 'Workflows', icon: '🔀', route: '/workflows', section: 'Main' },
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
      <AppShell
        appName="aimee"
        appNameShort="ai"
        navItems={NAV_ITEMS}
        topBarContent={<LogoutButton />}
      >
        <Routes>
          <Route path="/chat" element={<Chat />} />
          <Route path="/dashboard" element={<Dashboard />} />
          <Route path="/workflows" element={<Workflows />} />
          <Route path="*" element={<Navigate to="/chat" replace />} />
        </Routes>
      </AppShell>
      <Toast />
    </>
  );
}
