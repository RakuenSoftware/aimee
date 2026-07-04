import { useEffect, useState } from 'react';
import { Routes, Route, Navigate, NavLink } from 'react-router-dom';
import { loadSession, login, type SessionInfo } from './api';
import ConsoleDashboard from './pages/ConsoleDashboard';
import Accounts from './pages/Accounts';
import Governance from './pages/Governance';

// ConsoleApp is the shell for the aimee-kb web console: a session gate wrapping a
// nav + the Dashboard / Accounts / Governance surfaces. S0 ships the shell and
// the session/login flow; the pages are filled in S1 (dashboard), S3 (accounts),
// and S5 (governance).
export default function ConsoleApp() {
  const [session, setSession] = useState<SessionInfo | null | undefined>(undefined);

  useEffect(() => {
    loadSession().then(setSession).catch(() => setSession(null));
  }, []);

  if (session === undefined) return <div className="kbc-loading">Loading…</div>;
  if (session === null) return <LoginGate onLogin={setSession} />;

  return (
    <div className="kbc-shell">
      <nav className="kbc-nav">
        <span className="kbc-brand">aimee-kb console</span>
        {session.break_glass && <span className="kbc-breakglass">break-glass session</span>}
        <NavLink to="/dashboard">Dashboard</NavLink>
        <NavLink to="/accounts">Accounts</NavLink>
        <NavLink to="/governance">Governance</NavLink>
      </nav>
      <main className="kbc-main">
        <Routes>
          <Route path="/dashboard" element={<ConsoleDashboard />} />
          <Route path="/accounts" element={<Accounts />} />
          <Route path="/governance" element={<Governance />} />
          <Route path="*" element={<Navigate to="/dashboard" replace />} />
        </Routes>
      </main>
    </div>
  );
}

function LoginGate({ onLogin }: { onLogin: (s: SessionInfo) => void }) {
  const [idToken, setIdToken] = useState('');
  const [breakGlass, setBreakGlass] = useState('');
  const [err, setErr] = useState('');

  async function submit(payload: { id_token?: string; break_glass_bearer?: string }) {
    setErr('');
    try {
      onLogin(await login(payload));
    } catch (e) {
      setErr(String(e));
    }
  }

  return (
    <div className="kbc-login">
      <h1>aimee-kb console</h1>
      <p>Sign in with your OIDC identity token, or use break-glass recovery if enabled.</p>
      <label>
        OIDC id_token
        <textarea value={idToken} onChange={(e) => setIdToken(e.target.value)} rows={3} />
      </label>
      <button onClick={() => submit({ id_token: idToken })} disabled={!idToken}>
        Sign in
      </button>
      <details className="kbc-breakglass-form">
        <summary>Break-glass recovery</summary>
        <label>
          Console-admin bearer
          <input type="password" value={breakGlass} onChange={(e) => setBreakGlass(e.target.value)} />
        </label>
        <button onClick={() => submit({ break_glass_bearer: breakGlass })} disabled={!breakGlass}>
          Break-glass sign in
        </button>
      </details>
      {err && <p className="kbc-error">{err}</p>}
    </div>
  );
}
