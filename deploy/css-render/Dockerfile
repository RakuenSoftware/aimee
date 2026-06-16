# aimee css-render: isolated headless-Chromium sidecar for the #4-full
# computed-style oracle. Renders UNTRUSTED markup, so it runs as its own
# container reached only over HTTP (proposal §8) — never inside aimee-kb/server.
#
# The official Playwright image bundles Chromium + matching system deps.
FROM mcr.microsoft.com/playwright:v1.49.0-jammy

ENV CSS_RENDER_PORT=8780
WORKDIR /opt/css-render

# Playwright is preinstalled in the base image; pin the matching client.
RUN npm install --omit=dev playwright@1.49.0

COPY render.js /opt/css-render/render.js

EXPOSE 8780
# Drop privileges — the base image ships a non-root `pwuser`.
USER pwuser
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD node -e "require('http').get('http://127.0.0.1:'+(process.env.CSS_RENDER_PORT||8780)+'/health',r=>process.exit(r.statusCode===200?0:1)).on('error',()=>process.exit(1))"
CMD ["node", "/opt/css-render/render.js"]
