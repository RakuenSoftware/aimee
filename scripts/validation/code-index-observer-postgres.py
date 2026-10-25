#!/usr/bin/env python3
"""Qualify actual C source mutations/observer locks against owned PostgreSQL.

Uses real libpq (no SQL emulation). Does not contact installed services. Native
publisher/provider admission requires the separate private bridge integration.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [
    'src/tests/test_index_observer_postgres.c',
    'src/tests/support/index_observer_pg_test_adapter.c',
    'src/modules/kb/c/index_observer.c', 'src/modules/kb/c/code_index.c',
    'src/modules/kb/c/canonical_index.c', 'src/modules/kb/c/code_project_lifecycle.c',
    'src/modules/kb/c/kb_store_init.c', 'src/modules/kb/c/db_schema.c',
    'src/modules/kb/c/cross_repo_resolver.c', 'src/modules/kb/c/entity_edges.c',
    'src/modules/kb/c/kb_runtime_state.c', 'src/modules/kb/c/kb_audit_worm.c',
    'src/modules/kb/c/css_graph.c', 'src/extractors.c', 'src/extractors_extra.c',
    'src/extractors_new_langs.c', 'src/code_treesitter.c', 'src/util.c',
    'src/text.c', 'src/index.c', 'src/log.c', 'src/modules/kb/c/kb_store_hardening.c',
]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--postgres-bin', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    cache = args.directory.resolve()
    cache.mkdir(mode=0o700)
    pg = args.postgres_bin.resolve()
    prefix = pg.parent
    log = (cache/'commands.log').open('w')
    checks = []
    started = False
    result = 'failed'
    def run(name, command, **kw):
        print(name, flush=True)
        begin = time.monotonic()
        process = subprocess.run([str(c) for c in command], cwd=ROOT, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=180, **kw)
        log.flush()
        checks.append({'check':name,'exit_code':process.returncode,
                       'seconds':time.monotonic()-begin})
        process.check_returncode()
    tracked = SOURCES + ['src/modules/kb/c/index_observer_internal.h',
                        'src/modules/kb/include/aimee/kb/index_observer.h',
                        'src/modules/kb/c/schema.sql',
                        'scripts/validation/code-index-observer-postgres.py']
    def hashes():
        return {path:hashlib.sha256((ROOT/path).read_bytes()).hexdigest() for path in tracked}
    before = hashes()
    try:
        (cache/'socket').mkdir(mode=0o700)
        run('Initialize owned PostgreSQL', [pg/'initdb','-D',cache/'pg','-U','observer_owner',
            '--auth-local=trust','--auth-host=reject','--no-locale','--encoding=UTF8'])
        with (cache/'pg/postgresql.conf').open('a') as config:
            config.write("\nlisten_addresses=''\nunix_socket_directories='"+str(cache/'socket').replace("'","''")+
                         "'\nunix_socket_permissions=0700\nmax_connections=30\nshared_buffers=16MB\nfsync=on\nsynchronous_commit=on\n")
        run('Start owned PostgreSQL', [pg/'pg_ctl','-D',cache/'pg','-l',cache/'postgres.log','-w','start'])
        started = True
        admin = dict(os.environ, PGHOST=str(cache/'socket'), PGUSER='observer_owner', PGDATABASE='postgres')
        schema = (ROOT/'src/modules/kb/c/schema.sql').read_text().replace('__EMBED_DIM__','4')
        (cache/'schema.sql').write_text(schema)
        run('Apply real source-owner schema', [pg/'psql','-X','-v','ON_ERROR_STOP=1','-f',cache/'schema.sql'],env=admin)
        setup = """CREATE TABLE observer_events(sequence bigserial PRIMARY KEY,project text NOT NULL,state text NOT NULL,body text NOT NULL);
CREATE UNIQUE INDEX IF NOT EXISTS idx_ee_unique_triple ON entity_edges(source,relation,target);
CREATE ROLE observer_runtime LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOBYPASSRLS;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
GRANT USAGE ON SCHEMA public TO observer_runtime;
GRANT SELECT,INSERT,UPDATE,DELETE ON ALL TABLES IN SCHEMA public TO observer_runtime;
GRANT USAGE,SELECT,UPDATE ON ALL SEQUENCES IN SCHEMA public TO observer_runtime;
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO observer_runtime;
"""
        (cache/'setup.sql').write_text(setup)
        run('Install restricted fixture runtime role', [pg/'psql','-X','-v','ON_ERROR_STOP=1','-f',cache/'setup.sql'],env=admin)
        files = subprocess.check_output(['rg','--files'], cwd=ROOT, text=True).splitlines()
        directories = {str(Path(f).parent) for f in files if f.endswith('.h')}
        directories |= {f.split('/include/')[0]+'/include' for f in files if '/include/' in f}
        flags = ['-I'+str(ROOT/d) for d in sorted(directories)]
        flags += ['-I'+str(prefix/'include'), '-I'+str(ROOT/'src')]
        command = ['cc','-O1','-g','-flto','-ffunction-sections','-fdata-sections',
                   '-DAIMEE_DISABLE_KB_STORE_SQLITE_SHIM','-Wall','-Wextra','-Werror',
                   *flags,*SOURCES,'-Wl,--gc-sections','-L'+str(prefix/'lib'),
                   '-Wl,-rpath,'+str(prefix/'lib'),'-lpq','-lpthread','-lcrypto','-lm',
                   '-o',str(cache/'observer-test')]
        (cache/'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
        run('Build actual C source owner and real libpq fixture',command)
        socket = str(cache/'socket').replace('\\','\\\\').replace("'","\\'")
        env = dict(os.environ, AIMEE_INDEX_OBSERVER_TEST_DSN=f"host='{socket}' user=observer_runtime dbname=postgres connect_timeout=5")
        run('Actual PostgreSQL two-writer/crash/mutation qualification',[cache/'observer-test'],env=env)
        result = 'passed'
    finally:
        stopped = False
        if started:
            stop = subprocess.run([str(pg/'pg_ctl'),'-D',str(cache/'pg'),'-m','fast','-w','stop'],
                                  stdout=log,stderr=subprocess.STDOUT,timeout=30)
            stopped = stop.returncode == 0
        after = hashes()
        (cache/'result.json').write_text(json.dumps({'result':result,'checks':checks,
            'source_before':before,'source_after':after,'source_unchanged':before==after,
            'owned_postgres_stopped':stopped,'scope':'real PostgreSQL generic observer/source owner; native bridge provider qualified separately'},indent=2)+'\n')
        log.close()

if __name__ == '__main__':
    main()
