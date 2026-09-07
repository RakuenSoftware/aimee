import pathlib,subprocess
p=pathlib.Path('/opt/validation042');root=p/'repair-source'
subprocess.run(['cp','-a',str(p/'source'),str(root)],check=True)
subprocess.run(['cp',str(p/'module_stage_adapters.repair.c'),str(root/'src/server/module_stage_adapters.c')],check=True)
s=(root/'Dockerfile.server').read_text();s=s[s.index('FROM debian:bookworm-slim'):s.index('# --- webchat frontend:')]
s=s[:s.index('COPY --from=module-go-build')]
s+='RUN sh scripts/fetch-treesitter.sh && make -C src ../aimee-server -j6 AIMEE_TREESITTER=1 GIT_VERSION=v0.4.2-validation-repair\n'
(root/'Dockerfile.memory-repair').write_text(s)
with (p/'private/memory-repair-build.log').open('w') as f:
 rc=subprocess.run(['docker','build','-f','Dockerfile.memory-repair','-t','aimee-memory-repair-build:428a95d','.'],cwd=root,stdout=f,stderr=subprocess.STDOUT).returncode
print('repair-build',rc,flush=True);raise SystemExit(rc)
