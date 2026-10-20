#!/usr/bin/env python3
"""Actual daemon -> configured private Docker tests -> durable grade -> reuse.

This extends test_integration.sh's disposable server/PostgreSQL journey. The
provider is deterministic; tool dispatch, snapshots, Docker, grading, publication
and subsequent model request assembly are the production owners.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from test_native_ir_live import api, Local
from test_native_ir_browser_control import assert_completed_parent


# A PostgreSQL probe uses the integration's existing Go/pgx dependency. Its
# write faults delete only fixture-derived grades/experience/pending rows;
# canonical history/proof/jobs are never edited, fabricated or seeded.
READ_GRADES = r'''
package main
import (
 "context"
 "encoding/json"
 "os"
 "time"
 "github.com/jackc/pgx/v5"
)
func main() {
 ctx,cancel:=context.WithTimeout(context.Background(),15*time.Second);defer cancel()
 conn,err:=pgx.Connect(ctx,os.Getenv("AIMEE_STORE_URL"));if err!=nil{panic("integration PostgreSQL unavailable")};defer conn.Close(ctx)
 if len(os.Args)==4 && os.Args[3]=="drop-projection" {
  result,err:=conn.Exec(ctx,`DELETE FROM interaction_events i USING server_sessions s
  WHERE i.session_id=$1 AND s.id=i.session_id AND s.principal=$2 AND i.event_type='task_outcome'
  AND i.outcome IN ('tests_passed','tests_failed')`,os.Args[1],os.Args[2]);if err!=nil{panic(err)}
  if err=json.NewEncoder(os.Stdout).Encode(map[string]int64{"removed":result.RowsAffected()});err!=nil{panic(err)};return
 }
 if len(os.Args)==4 && os.Args[3]=="drop-delegate-projection" {
  tx,err:=conn.Begin(ctx);if err!=nil{panic(err)};defer tx.Rollback(ctx)
  _,err=tx.Exec(ctx,`DELETE FROM delegate_learnings l USING server_sessions s WHERE l.session_id=$1 AND s.id=l.session_id AND s.principal=$2`,os.Args[1],os.Args[2]);if err!=nil{panic(err)}
  result,err:=tx.Exec(ctx,`DELETE FROM interaction_events i USING server_sessions s WHERE i.session_id=$1 AND s.id=i.session_id AND s.principal=$2 AND (i.event_type='delegate_experience' OR (i.event_type='task_outcome' AND i.outcome IN ('tests_passed','tests_failed')))`,os.Args[1],os.Args[2]);if err!=nil{panic(err)}
  if err=tx.Commit(ctx);err!=nil{panic(err)}
  if err=json.NewEncoder(os.Stdout).Encode(map[string]int64{"removed":result.RowsAffected()});err!=nil{panic(err)};return
 }
 if len(os.Args)==4 && os.Args[3]=="experiences" {
  rows,err:=conn.Query(ctx,`SELECT jsonb_build_object('experience',d.payload::jsonb,'parent',p.payload::jsonb,'job_id',j.id::text,
   'pending',jsonb_build_object('evidence',l.evidence_json::jsonb,'auto_applied',l.auto_applied,'review_status',l.review_status,'confidence',l.confidence))::text
   FROM interaction_events d JOIN server_sessions s ON s.id=d.session_id AND s.principal=$2
   JOIN server_sessions ps ON ps.id=$1 AND ps.principal=$2
   JOIN session_ir_events p ON p.session_id=ps.id AND p.actor=$2 AND p.sequence::text=d.payload::jsonb->'parent_source'->>'sequence'
    AND p.turn_id=d.payload::jsonb->'parent_source'->>'turn_id' AND p.event_id=d.payload::jsonb->'parent_source'->>'event_id'
   JOIN agent_jobs j ON j.id::text=d.payload::jsonb->'child'->'learning_binding'->>'job_id'
   JOIN delegate_learnings l ON l.session_id=d.session_id AND l.evidence_json::jsonb=d.payload::jsonb
   WHERE d.event_type='delegate_experience' AND d.actor='system' AND d.payload::jsonb->'parent_source'->>'session_id'=$1 ORDER BY d.id`,os.Args[1],os.Args[2]);if err!=nil{panic(err)};defer rows.Close()
  out:=[]json.RawMessage{};for rows.Next(){var raw string;if err=rows.Scan(&raw);err!=nil{panic(err)};out=append(out,json.RawMessage(raw))};if err=rows.Err();err!=nil{panic(err)}
  if err=json.NewEncoder(os.Stdout).Encode(out);err!=nil{panic(err)};return
 }
 rows,err:=conn.Query(ctx,`SELECT i.payload,e.payload FROM interaction_events i
 JOIN server_sessions s ON s.id=i.session_id AND s.principal=$2
 JOIN session_ir_events e ON e.session_id=i.session_id AND e.sequence::text=i.payload::jsonb->'source'->>'sequence'
 AND e.event_id=i.payload::jsonb->'source'->>'event_id' AND e.turn_id=i.payload::jsonb->'source'->>'turn_id' AND e.actor=$2
 WHERE i.session_id=$1 AND i.event_type='task_outcome' AND i.outcome IN ('tests_passed','tests_failed') ORDER BY i.id`,os.Args[1],os.Args[2]);if err!=nil{panic(err)};defer rows.Close()
 out:=[]map[string]json.RawMessage{}
 for rows.Next(){var grade,event string;if err=rows.Scan(&grade,&event);err!=nil{panic(err)};out=append(out,map[string]json.RawMessage{"grade":json.RawMessage(grade),"event":json.RawMessage(event)})}
 if err=rows.Err();err!=nil{panic(err)};if err=json.NewEncoder(os.Stdout).Encode(out);err!=nil{panic(err)}
}
'''



def job_ids(value):
    """Read only actual returned job identifiers, including encoded tool results."""
    if isinstance(value, dict):
        for key, item in value.items():
            if key == "job_id" and isinstance(item, (int, str)) and str(item).isdigit():
                yield str(item)
            else:
                yield from job_ids(item)
    elif isinstance(value, list):
        for item in value:
            yield from job_ids(item)
    elif isinstance(value, str):
        try:
            parsed = json.loads(value)
        except (ValueError, TypeError):
            return
        if not isinstance(parsed, str):
            yield from job_ids(parsed)


def assert_delegate_experience(row, parent, actor, project, model, job, qualified):
    experience, event, pending = row["experience"], row["parent"], row["pending"]
    child = experience["child"]
    binding = child["learning_binding"]
    assert binding == dict(version=1, job_id=job, scope_kind="project", scope_id=project,
                           agent=model, provider="openai", model=model), binding
    assert row["job_id"] == job, row
    expected_child = "delegate-" + hashlib.sha256(f"{parent}:{job}:durable".encode()).hexdigest()
    assert child["session_id"] == expected_child and child["status"] == "running" and child["role"] == "diagnose", child
    assert event == dict(kind="child_execution", payload=child), event
    assert experience["principal"] == actor and experience["scope_kind"] == "project" and experience["scope_id"] == project, experience
    assert experience["parent_source"]["session_id"] == parent and experience["parent_source"]["actor"] == actor, experience
    assert experience["source"]["session_id"] == expected_child and experience["source"]["actor"] == actor, experience
    assert experience["source"]["turn_id"] == child["turn_id"] == experience["parent_source"]["turn_id"], experience
    assert experience["observed_status"] == "completed", experience
    assert pending["evidence"] == experience and pending["auto_applied"] == 0 and pending["confidence"] == 0 and pending["review_status"] == "pending", pending
    if qualified:
        assert experience["qualification"] == "configured_tests_only" and experience["verdict"] == "passed", experience
        assert experience["verification_ref"].startswith("sha256:"), experience
        actions = experience["actions"]
        assert experience["action_coverage"] == "bounded_canonical_tool_calls", experience
        assert any(action["name"] == "git_verify" and action["tool_id"] == "delegate-private-tests" for action in actions), actions
        assert all(len(action["input_sha256"]) == 64 and len(action["event_sha256"]) == 64 for action in actions), actions
    else:
        assert experience["qualification"] == "ungraded" and not experience.get("verdict") and not experience.get("verification_ref"), experience
    return experience


def main():
    required = os.environ.get("AIMEE_PRIVATE_TEST_REQUIRE_DOCKER") == "1" or os.environ.get("CI", "").lower() in ("true", "1")
    docker = shutil.which("docker")
    ready = docker and subprocess.run([docker, "version"], capture_output=True, timeout=15).returncode == 0
    if not ready:
        assert not required, "actual daemon grading journey requires Docker in CI"
        print("SKIP: independently graded daemon execution/reuse (real Docker unavailable)")
        return
    assert Path("/var/run/docker.sock").is_socket(), "production private verifier socket unavailable"
    image = "ubuntu:22.04"
    if subprocess.run([docker, "image", "inspect", image], capture_output=True, timeout=15).returncode:
        for attempt in range(3):
            pulled = subprocess.run([docker, "pull", image], timeout=180, capture_output=True)
            if pulled.returncode == 0:
                break
            assert attempt < 2, "configured Docker image could not be pulled"
            time.sleep(2 * (attempt + 1))
    home = Path(os.environ["AIMEE_HOME"])
    repository = Path(__file__).resolve().parents[2]
    actor = f"uid:{os.getuid()}"
    model = "execution-grade-live"
    child_model = "execution-grade-child"
    observed, errors = [], []
    phase = "verify"
    reference = ""
    expected_commit = ""
    state = {"verify_requests": 0, "delegate_requests": 0, "child_requests": 0}
    expected_delegate_job = ""

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert request.get("model") in (model, child_model), request.get("model")
                observed.append(dict(phase=phase, request=request))
                encoded = json.dumps(request, ensure_ascii=False)
                message = dict(role="assistant", content="CONFIGURED_TEST_RECEIPT_OK")
                finish = "stop"
                if request.get("model") == child_model:
                    assert phase in ("delegate_verify", "delegate_ungraded"), phase
                    state["child_requests"] += 1
                    results = [item for item in request.get("messages", []) if item.get("role") == "tool"]
                    if phase == "delegate_verify" and not results:
                        names = {item.get("function", {}).get("name", item.get("name")) for item in request.get("tools", [])}
                        assert "git_verify" in names, ("child verifier absent", sorted(names))
                        message = dict(role="assistant", tool_calls=[dict(id="delegate-private-tests", type="function",
                            function=dict(name="git_verify", arguments=json.dumps({"action": "run", "async": False, "format": "json"})))])
                        finish = "tool_calls"
                    elif phase == "delegate_verify":
                        assert "private-source-proof" in json.dumps(results) and "passed" in json.dumps(results).lower(), results
                    else:
                        assert not results, results
                        message["content"] = "UNVERIFIED_CHILD_OBSERVATION"
                    message["content"] = message.get("content", "")
                elif phase in ("delegate_verify", "delegate_ungraded"):
                    state["delegate_requests"] += 1
                    assert state["delegate_requests"] <= 2, encoded[-5000:]
                    results = [item for item in request.get("messages", []) if item.get("role") == "tool"]
                    if not results:
                        names = {item.get("function", {}).get("name", item.get("name")) for item in request.get("tools", [])}
                        assert "delegate" in names, ("delegate absent from parent catalog", sorted(names))
                        message = dict(role="assistant", tool_calls=[dict(id="actual-delegate", type="function",
                            function=dict(name="delegate", arguments=json.dumps(dict(role="diagnose", persona="engineer",
                                via=child_model, tools=True, prompt="Run git_verify configured tests and report the result." if phase == "delegate_verify" else
                                "Return an observation without running a verifier or claiming verified correctness."))))])
                        finish = "tool_calls"
                    else:
                        assert "job_id" in json.dumps(results), results
                elif phase == "verify":
                    state["verify_requests"] += 1
                    assert state["verify_requests"] <= 2, encoded[-5000:]
                    results = [item for item in request.get("messages", []) if item.get("role") == "tool"]
                    if not results:
                        names = {item.get("function", {}).get("name", item.get("name")) for item in request.get("tools", [])}
                        assert "git_verify" in names, ("configured verifier absent from host catalog", sorted(names))
                        message = dict(role="assistant", tool_calls=[dict(id="real-private-tests", type="function",
                            function=dict(name="git_verify", arguments=json.dumps({"action": "run", "async": False, "format": "json"})))])
                        finish = "tool_calls"
                    else:
                        result = json.dumps(results)
                        assert "private-source-proof" in result and "passed" in result.lower(), result
                elif phase == "reuse":
                    assert reference and reference in encoded, ("durable grade absent from later model request", encoded[-6000:])
                    assert "configured-test-history" in encoded and "configured_tests_only" in encoded, encoded[-6000:]
                    assert expected_commit in encoded, encoded[-6000:]
                    assert "Delegate succeeded in role" not in encoded, encoded[-6000:]
                    if expected_delegate_job:
                        assert 'delegate_experience' in encoded and expected_delegate_job in encoded, encoded[-8000:]
                        assert "bounded_canonical_tool_calls" in encoded and "delegate-private-tests" in encoded, encoded[-8000:]
                elif phase == "foreign_project":
                    assert reference and reference not in encoded, ("receipt crossed project scope", encoded[-6000:])
                    assert "configured-test-history" not in encoded, encoded[-6000:]
                else:
                    raise AssertionError(phase)
                data = json.dumps(dict(id="actual-grade-response", model=request["model"],
                    choices=[dict(index=0, message=message, finish_reason=finish)],
                    usage=dict(prompt_tokens=100, completion_tokens=20))).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            except Exception as error:
                errors.append(repr(error))
                self.send_error(500, "configured-test journey assertion failed")

    provider = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=provider.serve_forever, daemon=True)
    thread.start()
    models_path = home / "models.json"
    previous = models_path.read_bytes() if models_path.exists() else None
    attachments, sessions, configurations = [], [], []
    probe = None
    try:
        roots = []
        for name in ("execution-grade-owner", "execution-grade-foreign"):
            root = home.parent / name
            root.mkdir()
            for arguments in (("init", "-q", "-b", "main"), ("config", "user.name", "Integration Fixture"),
                              ("config", "user.email", "fixture@example.invalid")):
                subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True)
            # This persisted repository identity is outside tracked source, as on
            # ordinary local repositories without a forge remote.
            (root / ".git" / "aimee-project-id").write_text(str(uuid.uuid4()) + "\n")
            (root / ".gitignore").write_text(".aimee/\n")
            (root / "receipt.txt").write_text("CANONICAL_SOURCE\n")
            subprocess.run(["git", "-C", str(root), "add", ".gitignore", "receipt.txt"], check=True)
            subprocess.run(["git", "-C", str(root), "commit", "-qm", "configured source fixture"], check=True)
            registered = api("/v1/workspaces", dict(root=str(root), provider="shared", scan=False))
            assert registered.get("status") == "ok", registered
            roots.append(root)
        expected_commit = subprocess.check_output(["git", "-C", str(roots[0]), "rev-parse", "HEAD"], text=True).strip()
        config = home / "projects" / roots[0].name / "project.yaml"
        config.parent.mkdir(parents=True, exist_ok=True)
        assert not config.exists(), "fixture must not replace another project's verification configuration"
        config.write_text("env_check:\n  - bash\n  - cat\nverify:\n  incremental: false\n  enforce: true\n  steps:\n"
                          "    - name: private-source-proof\n"
                          '      run: test "$(cat receipt.txt)" = CANONICAL_SOURCE && test -d .git && test ! -S /run/aimee/aimee-http.sock\n')
        configurations.append(config)
        models_path.write_text(json.dumps(dict(default_agent=model, default_delegate=child_model, fallback_chain=[], models=[
            dict(name=name, model=name, provider="openai", auth_type="none", backend="http",
                 endpoint=f"http://127.0.0.1:{provider.server_port}/v1", roles=["all"], enabled=True,
                 tools_enabled=True, context_window=131072, max_tokens=4096, timeout_ms=120000, max_turns=4)
            for name in (model, child_model)])))

        def session():
            sid = api("/v1/sessions/create", dict(client_type="configured-test-journey"))["session_id"]
            sessions.append(sid)
            attach = api(f"/v1/sessions/{sid}/attach", dict(surface="webchat", owner=actor))["attach_id"]
            attachments.append(attach)
            api(f"/v1/sessions/{sid}/primary", dict(agent=model))
            return sid, attach

        def turn(sid, attach, cwd, message):
            raw = api("/v1/chat/stream", dict(aimee_session_id=sid, attach_id=attach, cwd=str(cwd),
                message=message, ir_version=1))
            records = [json.loads(line) for line in raw.splitlines() if line.strip()]
            assert not errors, errors
            assert_completed_parent(records)
            return records

        sid, attach = session()
        records = turn(sid, attach, roots[0], "Run the configured private tests and report their result.")
        assert state["verify_requests"] == 2, (state, errors)
        assert any(record["event"]["kind"] == "message" and "real-private-tests" in json.dumps(record) for record in records), records
        with tempfile.TemporaryDirectory(prefix="grade-probe-", dir=home) as temporary:
            source = Path(temporary) / "main.go"
            source.write_text(READ_GRADES)
            probe = Path(temporary) / "read-grades"
            built = subprocess.run([os.environ.get("GO", "go"), "-C", str(repository / "server-go"), "build", "-o", str(probe), str(source)],
                                   capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, built.stdout + built.stderr
            deadline = time.monotonic() + 30
            while True:
                read = subprocess.run([str(probe), sid, actor], capture_output=True, text=True, timeout=20)
                assert read.returncode == 0, read.stderr
                rows = json.loads(read.stdout)
                if rows:
                    break
                assert time.monotonic() < deadline, ("actual canonical completion did not publish a configured-test grade", records[-5:])
                time.sleep(0.1)
            assert len(rows) == 1, rows
            grade, terminal = rows[0]["grade"], rows[0]["event"]
            assert grade["verification"] == "tests_bound_revision" and grade["qualification"] == "configured_tests_only", grade
            assert grade["verdict"] == "passed" and grade["source"]["actor"] == actor and grade["source"]["session_id"] == sid, grade
            assert terminal["kind"] == "execution" and terminal["payload"]["status"] == "completed", terminal
            proof = grade["proof"]
            assert proof["snapshot_commit"] == expected_commit == proof["final_commit"], proof
            assert proof["snapshot_tree"] == proof["final_tree"] and proof["configuration_sha256"] == proof["final_configuration_sha256"], proof
            assert proof["isolation"] == "private_source_snapshot" and proof["scope_kind"] == "project", proof
            assert proof in terminal["payload"].get("verification_proofs", []), ("proof not retained with canonical terminal", terminal)
            assert proof["scope_id"] == (roots[0] / ".git" / "aimee-project-id").read_text().strip(), proof
            assert len(proof["steps"]) == 1 and proof["steps"][0]["name"] == "private-source-proof", proof
            assert proof["steps"][0]["ran"] and proof["steps"][0]["skipped"] is False and proof["steps"][0]["exit_code"] == "0", proof
            reference = grade["verification_ref"]
            assert reference.startswith("sha256:"), reference
            # Fault only the derived projection, keeping the actual immutable
            # terminal and its complete private execution proof. A fresh browser
            # replay must reconstruct the same grade through the production owner.
            removed = subprocess.run([str(probe), sid, actor, "drop-projection"],
                                     capture_output=True, text=True, timeout=20)
            assert removed.returncode == 0 and json.loads(removed.stdout) == {"removed": 1}, removed.stderr
            empty = subprocess.run([str(probe), sid, actor], capture_output=True, text=True, timeout=20)
            assert empty.returncode == 0 and json.loads(empty.stdout) == [], empty.stderr

            def replay():
                connection = Local("localhost")
                try:
                    connection.request("GET", f"/v1/sessions/{sid}/events?format=ir&cursor=0")
                    response = connection.getresponse()
                    assert response.status == 200, response.status
                    target = int(grade["source"]["sequence"])
                    source = None
                    while source is None:
                        line = response.readline().decode()
                        assert line, "canonical replay ended before its original terminal"
                        if line.startswith("data: "):
                            record = json.loads(line[6:])
                            if record.get("sequence") == str(target):
                                source = record
                    assert source["event"] == terminal and source["actor"] == actor, source
                finally:
                    connection.close()

            replay()
            restored = subprocess.run([str(probe), sid, actor], capture_output=True, text=True, timeout=20)
            assert restored.returncode == 0 and json.loads(restored.stdout) == rows, ("durable canonical proof failed to recover exact grade", restored.stdout, restored.stderr)
            replay()
            repeated = subprocess.run([str(probe), sid, actor], capture_output=True, text=True, timeout=20)
            assert repeated.returncode == 0 and json.loads(repeated.stdout) == rows, ("replay duplicated or changed restored grade", repeated.stdout, repeated.stderr)
            phase = "reuse"
            # A fresh session proves reuse comes from the local durable ledger,
            # rather than echoing the previous turn's conversation or tool result.
            reuse_sid, reuse_attach = session()
            turn(reuse_sid, reuse_attach, roots[0], "Summarize the available configured-test evidence.")
            phase = "foreign_project"
            foreign_sid, foreign_attach = session()
            turn(foreign_sid, foreign_attach, roots[1], "Summarize the available configured-test evidence.")

            def probe_json(probe_sid, mode=None):
                read = subprocess.run([str(probe), probe_sid, actor, *([mode] if mode else [])],
                                      capture_output=True, text=True, timeout=20)
                assert read.returncode == 0, read.stderr
                return json.loads(read.stdout)

            def delegate_attempt(qualified):
                nonlocal phase
                phase = "delegate_verify" if qualified else "delegate_ungraded"
                state["delegate_requests"] = 0
                child_before = state["child_requests"]
                parent_sid, parent_attach = session()
                returned = turn(parent_sid, parent_attach, roots[0], "Launch the requested diagnostic delegate.")
                jobs = set(job_ids(returned))
                assert len(jobs) == 1, returned
                job = jobs.pop()
                deadline = time.monotonic() + 120
                while True:
                    result = api("/v1/delegate/status", dict(job_id=int(job), full_result=True))
                    if result.get("job_status") in ("done", "failed", "partial", "cancelled"):
                        break
                    assert time.monotonic() < deadline, result
                    time.sleep(0.1)
                assert result["job_status"] == "done", result
                assert not errors and state["child_requests"] == child_before + (2 if qualified else 1), (errors, state)
                while True:
                    experiences = probe_json(parent_sid, "experiences")
                    if experiences:
                        break
                    assert time.monotonic() < deadline, ("actual delegated execution published no source-bound experience", result)
                    time.sleep(0.1)
                assert len(experiences) == 1, experiences
                experience = assert_delegate_experience(experiences[0], parent_sid, actor, proof["scope_id"], child_model, job, qualified)
                child_sid = experience["source"]["session_id"]
                child_grades = probe_json(child_sid)
                if qualified:
                    assert len(child_grades) == 1, child_grades
                    child_grade, child_terminal = child_grades[0]["grade"], child_grades[0]["event"]
                    assert child_grade["source"] == experience["source"] and child_grade["verification_ref"] == experience["verification_ref"], child_grades
                    assert child_grade["proof"] in child_terminal["payload"]["verification_proofs"], child_grades
                    assert child_grade["proof"]["scope_id"] == proof["scope_id"] and child_grade["proof"]["snapshot_commit"] == expected_commit, child_grades
                else:
                    assert child_grades == [], ("successful child execution without verifier was graded", child_grades)
                return parent_sid, experience, experiences, child_grades

            delegate_sid, experience, original_experiences, child_grades = delegate_attempt(True)
            reference = experience["verification_ref"]
            expected_delegate_job = experience["child"]["learning_binding"]["job_id"]
            child_sid = experience["source"]["session_id"]
            assert probe_json(child_sid, "drop-delegate-projection") == {"removed": 2}
            assert probe_json(child_sid) == [] and probe_json(delegate_sid, "experiences") == []

            def replay_parent_binding():
                connection = Local("localhost")
                try:
                    # Only the parent is requested; recovery must resolve the
                    # exact child receipt through its protected running binding.
                    connection.request("GET", f"/v1/sessions/{delegate_sid}/events?format=ir&cursor=0")
                    response = connection.getresponse()
                    assert response.status == 200, response.status
                    while True:
                        line = response.readline().decode()
                        assert line, "parent replay ended before delegate binding"
                        if line.startswith("data: "):
                            record = json.loads(line[6:])
                            if record.get("sequence") == experience["parent_source"]["sequence"]:
                                assert record["event"] == original_experiences[0]["parent"] and record["actor"] == actor, record
                                break
                finally:
                    connection.close()

            for _ in range(2):
                replay_parent_binding()
                assert probe_json(child_sid) == child_grades, "parent-only replay did not restore exactly one original grade"
                assert probe_json(delegate_sid, "experiences") == original_experiences, "parent-only replay changed or duplicated experience/pending evidence"
            phase = "reuse"
            reuse_sid, reuse_attach = session()
            turn(reuse_sid, reuse_attach, roots[0], "Summarize the available delegate configured-test evidence.")
            phase = "foreign_project"
            foreign_sid, foreign_attach = session()
            turn(foreign_sid, foreign_attach, roots[1], "Summarize the available delegate configured-test evidence.")
            delegate_attempt(False)
        assert subprocess.check_output(["git", "-C", str(roots[0]), "status", "--porcelain"], text=True) == ""
        assert not errors, errors
        print("execution grade: actual daemon primary/delegate -> private Docker tests -> canonical proof -> grade/experience/pending -> parent-only recovery -> fresh-session scoped reuse; no-verifier delegate remains ungraded")
    finally:
        cleanup_errors = []
        for sid, attach in zip(sessions, attachments):
            try:
                api(f"/v1/sessions/{sid}/detach", {"attach_id": attach})
            except Exception as error:
                cleanup_errors.append(repr(error))
        for config in configurations:
            config.unlink(missing_ok=True)
        if previous is None:
            models_path.unlink(missing_ok=True)
        else:
            models_path.write_bytes(previous)
        provider.shutdown()
        thread.join(timeout=5)
        provider.server_close()
        if cleanup_errors:
            raise AssertionError(("execution-grade attachment cleanup failed", cleanup_errors))


if __name__ == "__main__":
    main()
