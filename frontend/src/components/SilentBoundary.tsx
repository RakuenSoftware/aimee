import { Component, type ErrorInfo, type ReactNode } from 'react';

/* A fail-safe error boundary for ancillary chrome (the setup chip, the tutorial
 * card, the setup wizard). Those mount OUTSIDE the page ErrorBoundary in App, so
 * without this a render error in one of them would throw past the root and unmount
 * the whole shell — the exact failure the page ErrorBoundary exists to prevent.
 * Unlike that boundary, this one renders NOTHING on error (these are optional
 * overlays; silently dropping one is far better than taking down the app) and logs
 * for diagnosis. */
export default class SilentBoundary extends Component<{ children: ReactNode }, { failed: boolean }> {
  state = { failed: false };

  static getDerivedStateFromError() {
    return { failed: true };
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('SilentBoundary caught an error in optional UI', error, info);
  }

  render() {
    return this.state.failed ? null : this.props.children;
  }
}
