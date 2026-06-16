# aimee css-render: isolated headless-Chromium sidecar for the #4-full
# computed-style oracle. Renders UNTRUSTED markup, so it runs as its own
# container reached only over HTTP (proposal §8) — never inside aimee-kb/server.
#
# The official Playwright image bundles Chromium + matching system deps.
FROM mcr.microsoft.com/playwright:v1.49.0-jammy

ENV CSS_RENDER_PORT=8780
# The base image already bundles the browser binaries (at PLAYWRIGHT_BROWSERS_PATH);
# installing the client must NOT re-download Chromium.
ENV PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1
WORKDIR /opt/css-render

# Pin the JS client matching the base image's bundled browsers.
RUN npm install --omit=dev playwright@1.49.0

# snapshot.js (shared) + render.js (HTTP server, default) + oneshot.js (stdin->
# stdout, the on-demand per-render mode: `docker run --rm -i <img> node oneshot.js`).
COPY snapshot.js render.js oneshot.js /opt/css-render/

EXPOSE 8780
# Drop privileges — the base image ships a non-root `pwuser`.
USER pwuser
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD node -e "require('http').get('http://127.0.0.1:'+(process.env.CSS_RENDER_PORT||8780)+'/health',r=>process.exit(r.statusCode===200?0:1)).on('error',()=>process.exit(1))"
# Default = long-running HTTP sidecar. Override for one-shot: node oneshot.js.
CMD ["node", "/opt/css-render/render.js"]
