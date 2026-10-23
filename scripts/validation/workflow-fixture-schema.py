#!/usr/bin/env python3
"""Read-only, independent PostgreSQL metadata oracle for the workflow rig."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.parse

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z_0-9]{0,62}\Z")
SQL = """SELECT jsonb_build_object('schema',current_schema(),'columns',
 COALESCE(jsonb_agg(jsonb_build_array(table_name,column_name,ordinal_position,
 data_type,udt_name,is_nullable,column_default) ORDER BY table_name,ordinal_position),'[]'::jsonb))
FROM information_schema.columns WHERE table_schema=current_schema();
"""


def connection(environ):
    """Match the SQL owner's URI search_path/options contract; never print DSNs."""
    parts = []
    settings = []
    for name in ("AIMEE_STORE_URL", "AIMEE_STORE_MIGRATION_URL"):
        uri = urllib.parse.urlsplit(environ[name])
        if uri.scheme not in ("postgres", "postgresql"):
            raise ValueError("fixture schema oracle requires PostgreSQL URIs")
        query = urllib.parse.parse_qsl(uri.query, keep_blank_values=True)
        setting = {}
        for key in ("search_path", "options"):
            values = [value for candidate, value in query if candidate == key]
            if len(values) > 1:
                raise ValueError("fixture schema settings are ambiguous")
            setting[key] = values[0] if values else ""
        settings.append(setting)
        parts.append((uri, query))
    if settings[0] != settings[1]:
        raise ValueError("fixture runtime and migration schema settings disagree")
    path = settings[0]["search_path"]
    configured = environ.get("AIMEE_STORE_SCHEMA", "")
    for value in (path, configured):
        if value and not IDENTIFIER.fullmatch(value):
            raise ValueError("fixture schema oracle requires one plain schema identifier")
    if path and configured and path.lower() != configured.lower():
        raise ValueError("fixture schema environment disagrees with its URI")
    options = settings[0]["options"]
    if "search_path" in options.lower():
        raise ValueError("fixture schema must use URI search_path rather than options")
    # libpq rejects pgx's URI search_path parameter. Preserve its exact meaning
    # using PGOPTIONS, and verify the server's resolved schema below. Without a
    # URI setting, do not invent 'public': the actual role default remains in force.
    uri, query = parts[1]
    query = [(key, value) for key, value in query if key not in ("search_path", "options")]
    env = dict(environ)
    env["PGDATABASE"] = urllib.parse.urlunsplit(uri._replace(query=urllib.parse.urlencode(query)))
    env["PGCONNECT_TIMEOUT"] = "5"
    env["PGOPTIONS"] = options + (f" -c search_path={path}" if path else "") + " -c default_transaction_read_only=on"
    return env, (path or configured).lower() or None


def admit_snapshot(value, expected):
    if not isinstance(value, dict) or not isinstance(value.get("schema"), str) or not value["schema"]:
        raise ValueError("fixture schema oracle omitted the resolved schema")
    if expected is not None and value["schema"] != expected:
        raise ValueError("fixture schema oracle resolved a different schema")
    columns = value.get("columns")
    if not isinstance(columns, list) or not columns or any(not isinstance(row, list) or len(row) != 7 or not isinstance(row[0], str) or not row[0] for row in columns):
        raise ValueError("fixture schema oracle returned no real column definitions")
    tables = {row[0] for row in columns}
    if not {"lifecycle_work_item", "session_ir_events"}.issubset(tables):
        raise ValueError("fixture schema oracle did not observe the migrated store")
    return value


def main():
    env, expected = connection(os.environ)
    executable = env.get("AIMEE_PSQL_BIN", "psql")
    result = subprocess.run([executable, "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1"],
                            input=SQL, text=True, capture_output=True, env=env, timeout=20)
    if result.returncode:
        raise ValueError("fixture schema oracle SQL read failed")
    value = admit_snapshot(json.loads(result.stdout), expected)
    output = Path(sys.argv[1])
    output.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n")
    output.chmod(0o600)


if __name__ == "__main__":
    try:
        main()
    except (KeyError, ValueError, OSError, subprocess.SubprocessError):
        # URI parsing and database diagnostics may contain credentials.
        print("workflow fixture schema oracle refused", file=sys.stderr)
        sys.exit(1)
