#!/usr/bin/env python3
"""Actual pinned SDK + exact owned-daemon crash/restart, with no tool execution.

Only test_integration.sh controls daemon/module PIDs. The fixture reads actual
canonical reservation/source rows and never seeds cache claims, grades or turns.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from test_native_ir_live import api
from test_native_ir_browser_control import prepare_browser_workspace, assert_completed_parent


READ_RECOVERY = r'''
package main
import("context";"encoding/json";"os";"time";"github.com/jackc/pgx/v5")
func main(){
 ctx,cancel:=context.WithTimeout(context.Background(),15*time.Second);defer cancel()
 conn,err:=pgx.Connect(ctx,os.Getenv("AIMEE_STORE_URL"));if err!=nil{panic("native crash PostgreSQL unavailable")};defer conn.Close(ctx)
 var raw string
 err=conn.QueryRow(ctx,`SELECT jsonb_build_object(
 'reservations',coalesce((SELECT jsonb_agg(to_jsonb(r) ORDER BY r.created_at,r.owner_id) FROM session_native_reservations r WHERE r.session_id=s.id AND r.actor=s.principal AND r.harness=$3),'[]'::jsonb),
 'epochs',coalesce((SELECT jsonb_agg(to_jsonb(e)) FROM session_native_cache_epochs e WHERE e.session_id=s.id AND e.actor=s.principal AND e.harness=$3),'[]'::jsonb),
 'fences',coalesce((SELECT jsonb_agg(to_jsonb(f)) FROM session_native_recovery_fences f WHERE f.session_id=s.id AND f.actor=s.principal),'[]'::jsonb),
 'events',coalesce((SELECT jsonb_agg(jsonb_build_object('sequence',e.sequence,'turn_id',e.turn_id,'actor',e.actor,'event',e.payload::jsonb) ORDER BY e.sequence) FROM session_ir_events e WHERE e.session_id=s.id),'[]'::jsonb),
 'grades',(SELECT count(*) FROM interaction_events i WHERE i.session_id=s.id AND i.event_type='task_outcome' AND i.outcome IN ('tests_passed','tests_failed'))
 )::text FROM server_sessions s WHERE s.id=$1 AND s.principal=$2`,os.Args[1],os.Args[2],os.Args[3]).Scan(&raw)
 if err!=nil{panic(err)};if err=json.NewEncoder(os.Stdout).Encode(json.RawMessage(raw));err!=nil{panic(err)}
}
'''


def lifecycle(home, action):
    assert os.environ.get("AIMEE_TEST_NATIVE_LIFECYCLE") == "1", "owned harness required"
    assert action in ("pause-owned-server", "kill-owned-server", "restart-owned-server")
    request = home / "execution-grade-recovery.request"
    acknowledgement = home / "execution-grade-recovery.ack"
    acknowledgement.unlink(missing_ok=True)
    temporary = home / "execution-grade-recovery.request.tmp"
    temporary.write_text(action + "\n")
    temporary.replace(request)
    deadline = time.monotonic() + 90
    while not acknowledgement.exists():
        assert time.monotonic() < deadline, ("owned native lifecycle timeout", action)
        time.sleep(0.05)
    assert acknowledgement.read_text().strip() == action + ":ok", acknowledgement.read_text()
    request.unlink(missing_ok=True)


def leaf_path(reservation):
    wire = reservation["containment_json"]
    assert hashlib.sha256(wire.encode()).hexdigest() == reservation["containment_sha256"]
    manifest = json.loads(wire)
    assert manifest["version"] == 1 and manifest["kind"] == "linux_cgroup_v2_namespace", manifest
    assert manifest["protocol"] == "user-mount-pid-cgroup-network-write-preexec-v3", manifest
    assert manifest["root"] == os.environ["AIMEE_NATIVE_CONTAINMENT_ROOT"], manifest
    leaf = manifest["leaf_name"]
    assert leaf.startswith("aimee-native-") and len(leaf) == len("aimee-native-") + 32
    assert all(c in "0123456789abcdef" for c in leaf[len("aimee-native-"):])
    path = Path(manifest["root"]) / leaf
    current = path.stat()
    assert str(current.st_dev) == str(manifest["leaf_dev"]), manifest
    assert str(current.st_ino) == str(manifest["leaf_inode"]), manifest
    return path


def populated(path):
    values = dict(line.split() for line in (path / "cgroup.events").read_text().splitlines())
    assert values["populated"] in ("0", "1"), values
    return values["populated"] == "1"


def open_boundary(reservation):
    """Retain only the actual test-owned source's exact random kernel leaf."""
    path = leaf_path(reservation)
    descriptor = json.loads(reservation["containment_json"])
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        current = os.fstat(fd)
        assert str(current.st_dev) == str(descriptor["leaf_dev"])
        assert str(current.st_ino) == str(descriptor["leaf_inode"])
        return fd, path, current.st_dev, current.st_ino
    except BaseException:
        os.close(fd)
        raise


def boundary_values(boundary):
    with os.fdopen(os.open("cgroup.events", os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC,
                           dir_fd=boundary[0])) as file:
        return dict(line.split() for line in file.read().splitlines())


def boundary_write(boundary, control, value):
    assert control in ("cgroup.freeze", "cgroup.kill")
    with os.fdopen(os.open(control, os.O_WRONLY | os.O_NOFOLLOW | os.O_CLOEXEC,
                           dir_fd=boundary[0]), "w") as file:
        file.write(value + "\n")


def freeze_boundary(boundary):
    # A real SDK may exit naturally when its daemon pipes close. Freeze the
    # actual admitted SDK tree while its provider request is blocked, modelling
    # a stalled descendant without introducing a surrogate or fabricated claim.
    assert boundary_values(boundary)["populated"] == "1"
    boundary_write(boundary, "cgroup.freeze", "1")
    deadline = time.monotonic() + 5
    while True:
        values = boundary_values(boundary)
        assert values["populated"] == "1", ("actual SDK exited before owned freeze", values)
        if values.get("frozen") == "1":
            return
        assert time.monotonic() < deadline, ("owned SDK freeze timed out", values)
        time.sleep(0.01)


def cleanup_boundary(boundary):
    # Cleanup cannot make the test pass: all production-recovery assertions ran
    # first. No PID, arbitrary leaf name, guessed path or foreign row is used.
    fd, path, device, inode = boundary
    try:
        try:
            current = path.stat()
        except FileNotFoundError:
            return
        assert (current.st_dev, current.st_ino) == (device, inode), "owned leaf was replaced"
        if boundary_values(boundary)["populated"] == "1":
            boundary_write(boundary, "cgroup.kill", "1")
        boundary_write(boundary, "cgroup.freeze", "0")
        deadline = time.monotonic() + 10
        while boundary_values(boundary)["populated"] != "0":
            assert time.monotonic() < deadline, "owned SDK cleanup did not quiesce"
            time.sleep(0.01)
        try:
            current = path.stat()
            assert (current.st_dev, current.st_ino) == (device, inode)
            path.rmdir()
        except FileNotFoundError:
            pass
    except FileNotFoundError:
        pass  # The production owner already removed this exact empty leaf.
    finally:
        os.close(fd)


def cache_binding(binding, epoch):
    return binding if epoch == "0" else hashlib.sha256(
        ("aimee-native-cache-epoch-v1:" + epoch + ":" + binding).encode()).hexdigest()


def assert_recovered(state, old, actor):
    old_turn = old["turn_id"]
    reservations = state["reservations"]
    recovered = [r for r in reservations if r["owner_id"] == old["owner_id"]]
    assert len(recovered) == 1 and recovered[0]["state"] == "reconciled", state
    recovered = recovered[0]
    assert recovered["recovery_epoch"] != old["epoch"] and len(recovered["recovery_epoch"]) == 32, state
    assert recovered["recovered_from"] == "pending:" + old["claim_id"] + ":" + old_turn, state
    events = [e for e in state["events"] if e["turn_id"] == old_turn]
    assert all(e["actor"] == actor for e in events), events
    statuses = [e["event"]["payload"]["status"] for e in events if e["event"]["kind"] == "execution"]
    assert statuses == ["running", "cancelled"], events
    fences = [f for f in state["fences"] if f["turn_id"] == old_turn]
    assert len(fences) == 1 and fences[0]["owner_id"] == old["owner_id"], state
    assert fences[0]["claim_id"] == old["claim_id"] and fences[0]["terminal_sequence"] == recovered["terminal_sequence"], state
    assert state["grades"] == 0, state
    assert not any(e["event"]["kind"] == "native_checkpoint" for e in events), events
    return recovered


def main():
    assert os.environ.get("AIMEE_TEST_NATIVE_REQUIRE_CONTAINMENT") == "1", "explicit required profile only"
    assert os.environ.get("AIMEE_NATIVE_CONTAINMENT_ROOT"), "required containment root unset"
    home = Path(os.environ["AIMEE_HOME"])
    actor = "uid:" + str(os.getuid())
    api("/v1/vault/capability", dict(action="grant", principal=actor))
    workspace = prepare_browser_workspace(api, home, Path(os.environ["AIMEE_IR_DELEGATE_WORKSPACE"]),
                                          client_name="native-crash-browser-client")
    models_path = home / "models.json"
    previous = models_path.read_bytes() if models_path.exists() else None
    active = {"phase": "blocked", "marker": "", "requests": []}
    requested = threading.Event()
    release = threading.Event()
    errors = []
    owned_boundaries = {}

    def remember_boundary(reservation):
        key = (reservation["session_id"], reservation["harness"], reservation["owner_id"])
        if key not in owned_boundaries:
            try:
                owned_boundaries[key] = open_boundary(reservation)
            except FileNotFoundError:
                return None  # Already removed by the actual production owner.
        return owned_boundaries[key]

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert request["model"] == active["marker"], request
                assert self.headers.get("Authorization") == "Bearer native-crash-fixture-only"
                if self.path.endswith("/count_tokens"):
                    body = json.dumps(dict(input_tokens=100)).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                encoded = json.dumps(request)
                assert "NATIVE_CRASH_FIRST" in encoded, encoded
                active["requests"].append(request)
                if active["phase"] == "blocked":
                    requested.set()
                    assert release.wait(180), "owned daemon crash did not release obsolete provider request"
                    # The killed daemon's socket cannot receive a model response.
                    return
                assert "NATIVE_CRASH_FRESH" in encoded, encoded
                assert not any(message.get("role") == "tool" for message in request.get("messages", [])), request
                payload = dict(id="native-crash-fresh-response", model=active["marker"],
                    choices=[dict(index=0, message=dict(role="assistant", content="NATIVE_CRASH_RECOVERED"), finish_reason="stop")],
                    usage=dict(prompt_tokens=100, completion_tokens=10))
                if request.get("stream"):
                    first = dict(id=payload["id"], object="chat.completion.chunk", model=active["marker"],
                                 choices=[dict(index=0, delta=dict(role="assistant", content="NATIVE_CRASH_RECOVERED"), finish_reason=None)])
                    final = dict(id=payload["id"], object="chat.completion.chunk", model=active["marker"],
                                 choices=[dict(index=0, delta={}, finish_reason="stop")], usage=payload["usage"])
                    body = ("data: " + json.dumps(first) + "\n\n" + "data: " + json.dumps(final) + "\n\n").encode()
                    content_type = "text/event-stream"
                else:
                    payload["object"] = "chat.completion"
                    body = json.dumps(payload).encode()
                    content_type = "application/json"
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except Exception as error:
                errors.append(repr(error))
                requested.set()
                self.send_error(500)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    repository = Path(__file__).resolve().parents[2]
    try:
        with tempfile.TemporaryDirectory(prefix="native-crash-probe-", dir=home) as temporary:
            source = Path(temporary) / "main.go"
            source.write_text(READ_RECOVERY)
            probe = Path(temporary) / "read-recovery"
            built = subprocess.run([os.environ.get("GO", "go"), "-C", str(repository / "server-go"),
                                    "build", "-o", str(probe), str(source)], capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, built.stdout + built.stderr

            def read(sid, kind):
                result = subprocess.run([str(probe), sid, actor, kind], capture_output=True, text=True, timeout=20)
                assert result.returncode == 0, result.stderr
                return json.loads(result.stdout)

            for kind in ("codex", "claude"):
                marker = "native-crash-" + kind
                active.update(phase="blocked", marker=marker, requests=[])
                requested.clear()
                release.clear()
                model = dict(name=marker, model=marker, provider="openai", auth_type="api_key",
                    endpoint=f"http://127.0.0.1:{server.server_port}/v1", backend="provider-cli", cli_kind=kind,
                    cli_cmd=os.environ["AIMEE_TEST_NATIVE_" + kind.upper()], roles=["all"], enabled=True,
                    tools_enabled=True, max_tokens=4096, timeout_ms=150000, context_window=131072,
                    max_parallel=1, max_turns=3, cli_idle_timeout_ms=150000)
                models_path.write_text(json.dumps(dict(default_agent=marker, default_delegate=marker,
                                                       fallback_chain=[], models=[model])))
                api("/v1/vault/set_server", dict(agent=marker, cred="api_key", secret="native-crash-fixture-only"))
                sid = api("/v1/sessions/create", dict(client_type="native-crash-integration"))["session_id"]
                attachment = api(f"/v1/sessions/{sid}/attach", dict(surface="webchat", owner=actor))["attach_id"]
                api(f"/v1/sessions/{sid}/primary", dict(agent=marker))
                disconnected = []

                def original_turn():
                    try:
                        result = api("/v1/chat/stream", dict(attach_id=attachment, aimee_session_id=sid,
                            message="NATIVE_CRASH_FIRST: wait for a simple answer; do not call any tools.",
                            cwd=str(workspace), ir_version=1))
                        disconnected.append(result)
                    except Exception as error:
                        disconnected.append(type(error).__name__)

                original = threading.Thread(target=original_turn, daemon=True)
                original.start()
                assert requested.wait(90), (kind, "actual pinned SDK never reached provider")
                assert not errors and len(active["requests"]) == 1, (errors, active)
                before = read(sid, kind)
                assert len(before["reservations"]) == 1, ("required containment used fallback", before)
                old = before["reservations"][0]
                assert old["state"] == "active" and old["epoch"] == "0" and old["claim_id"], old
                old_leaf = leaf_path(old)
                assert populated(old_leaf), old
                state_name = "sessions" if kind == "codex" else "projects"
                old_state = home / ("browser-" + kind) / sid / old["binding_sha256"] / state_name
                assert old_state.is_dir(), old_state
                sentinel = old_state / ".qualification-preserve"
                with sentinel.open("xb") as file:
                    file.write(b"test-owned state retention sentinel; no claim or execution evidence\n")
                sentinel_digest = hashlib.sha256(sentinel.read_bytes()).hexdigest()
                frozen = remember_boundary(old)
                assert frozen is not None
                freeze_boundary(frozen)
                lifecycle(home, "pause-owned-server")
                lifecycle(home, "kill-owned-server")
                # This is deliberately after actual daemon death and before
                # restart/lazy recovery: the SDK descendant is still real.
                assert populated(old_leaf) and boundary_values(frozen).get("frozen") == "1", (kind, "real frozen SDK tree did not survive daemon death")
                assert len(active["requests"]) == 1 and not errors, (errors, active)
                lifecycle(home, "restart-owned-server")
                active["phase"] = "fresh"
                readiness = api("/v1/server/health")
                print("native crash restart readiness:", kind, json.dumps(readiness), flush=True)
                attachment = api(f"/v1/sessions/{sid}/attach", dict(surface="webchat", owner=actor))["attach_id"]
                fresh = api("/v1/chat/stream", dict(attach_id=attachment, aimee_session_id=sid,
                    message="NATIVE_CRASH_FRESH: answer without tools.", cwd=str(workspace), ir_version=1))
                records = [json.loads(line) for line in fresh.splitlines() if line.strip()]
                diagnostic = dict(sdk=kind, raw=fresh[-20000:], ownership=read(sid, kind),
                                  provider_requests=len(active["requests"]), provider_errors=errors,
                                  restart_health=readiness)
                for reservation in diagnostic["ownership"]["reservations"]:
                    remember_boundary(reservation)
                assert records and all(isinstance(record, dict) and isinstance(record.get("event"), dict)
                                       for record in records), diagnostic
                try:
                    assert_completed_parent(records)
                except (AssertionError, KeyError) as error:
                    raise AssertionError(diagnostic) from error
                assert "NATIVE_CRASH_RECOVERED" in fresh and not errors, (fresh, errors)
                deadline = time.monotonic() + 10
                while True:
                    after = read(sid, kind)
                    if len(after["reservations"]) == 2 and any(r["state"] == "finished" for r in after["reservations"]):
                        break
                    assert time.monotonic() < deadline, after
                    time.sleep(0.05)
                recovered = assert_recovered(after, old, actor)
                assert not old_leaf.exists() or not populated(old_leaf), old_leaf
                assert hashlib.sha256(sentinel.read_bytes()).hexdigest() == sentinel_digest
                newest = [r for r in after["reservations"] if r["owner_id"] != old["owner_id"]]
                assert len(newest) == 1 and newest[0]["state"] == "finished", after
                newest = newest[0]
                assert newest["binding_sha256"] == old["binding_sha256"], after
                assert newest["epoch"] == recovered["recovery_epoch"], after
                assert newest["containment_sha256"] != old["containment_sha256"], after
                fresh_boundary = remember_boundary(newest)
                assert fresh_boundary is None or boundary_values(fresh_boundary)["populated"] == "0", after
                new_binding = cache_binding(newest["binding_sha256"], newest["epoch"])
                assert new_binding != old["binding_sha256"]
                fresh_state = home / ("browser-" + kind) / sid / new_binding / state_name
                assert fresh_state.is_dir() and not (fresh_state / sentinel.name).exists(), fresh_state
                assert len(active["requests"]) == 2 and not errors, (errors, active)
                assert not any(block.get("type") in ("tool_use", "tool_result") for event in after["events"]
                               for block in event["event"].get("payload", {}).get("blocks", [])), after
                assert not any(event["event"]["kind"] in ("tool_result", "child_execution") for event in after["events"]), after
                release.set()
                original.join(10)
                assert not original.is_alive(), "killed daemon stream did not disconnect"
                api(f"/v1/sessions/{sid}/detach", dict(attach_id=attachment))
                print("native crash recovery:", kind,
                      "actual frozen SDK survives owned daemon SIGKILL; lazy restart cancels/fences once, empties original tree, rotates epoch, retains old files and freshly adopts history without tools/grades", flush=True)
            from test_native_ir_interruption_live import run_interruption
            run_interruption(home, workspace, probe, remember_boundary)
    finally:
        primary_failure = sys.exc_info()[0] is not None
        release.set()
        cleanup_errors = []
        for boundary in owned_boundaries.values():
            try:
                cleanup_boundary(boundary)
            except Exception as error:
                cleanup_errors.append(repr(error))
        if cleanup_errors:
            print("native crash owned cleanup failures:", cleanup_errors, flush=True)
        server.shutdown()
        server.server_close()
        thread.join()
        if previous is None:
            models_path.unlink(missing_ok=True)
        else:
            models_path.write_bytes(previous)
        if cleanup_errors and not primary_failure:
            raise AssertionError(cleanup_errors)


if __name__ == "__main__":
    main()
