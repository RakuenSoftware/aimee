#!/usr/bin/env python3
"""Run benchmark arms on rented vast.ai GPUs, several at a time.

WHAT RUNS WHERE, and why it is split this way. The rented box runs ONLY
llama-server. The corpus, the client, the prompt, the scorer and the prediction
files all stay local, and `run_llamacpp.py --base-url` drives the remote server
over HTTP. So the GPU is the single variable between a rented arm and a local
one. Shipping the harness to the instance would have changed the client, the
corpus ordering and the scorer host at the same time, which is the mistake
articles 03 and 04 are about.

WHAT THIS MAY NOT DO. One arm runs start to finish on one instance. Splitting a
corpus across two machines gives each shard a different note history, and defect
40 measures that at 47% of outputs changing on identical inputs. Sharding is a
different configuration, not a faster way to get the same answer.

CALIBRATION IS A PRECONDITION. Every configuration in this project reproduces
itself and no two configurations agree; process count alone is worth 0.0105 F1.
A rented GPU is a different configuration and cross-card comparability has never
been measured here. Run --calibrate first, which re-runs an arm that is already
banked locally and paired-bootstraps against it. Until that lands inside the
interval, rented arms are comparable to each other and NOT to the local field.

BILLING. An instance is destroyed as soon as its arm finishes or fails. The pool
never holds an idle rented GPU. Cost is reported per arm.

The API key is read from VAST_API_KEY and is never written to disk.
"""
import argparse, atexit, json, os, signal, subprocess, sys, threading, time
import urllib.error, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRATCH = os.path.join(ROOT, ".scratch")
API = "https://console.vast.ai/api/v0"
IMAGE = "ghcr.io/ggml-org/llama.cpp:server-cuda"
KEY = os.environ.get("VAST_API_KEY", "")


def api(path, method="GET", body=None, params=None):
    url = "%s/%s" % (API, path.lstrip("/"))
    if params:
        url += "?" + urllib.parse.urlencode(params)
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": "Bearer " + KEY, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=90) as r:
        return json.loads(r.read())


def find_offers(gpu, maxprice, n, interruptible=False):
    q = {"rentable": {"eq": True}, "num_gpus": {"eq": 1},
         "gpu_name": {"in": [gpu]}, "disk_space": {"gte": 60},
         "inet_down": {"gte": 400}, "reliability2": {"gte": 0.97},
         "dph_total": {"lte": maxprice},
         "type": "bid" if interruptible else "on-demand",
         "order": [["dph_total", "asc"]], "limit": max(n * 4, 20)}
    return api("bundles/", params={"q": json.dumps(q)}).get("offers", [])


def launch(offer_id, repo, ctx, cache_ram, bid=None, draft=None):
    """bid=None rents on-demand; a float rents interruptible at that price.

    Bid ABOVE min_bid rather than at it. Bidding the floor is what gets an
    instance outbid first, and a preemption costs the whole arm: an arm cannot be
    resumed on another machine without becoming a blend of two configurations,
    which is the hazard defect 40 and article 04 are about."""
    args = ["-hf", repo, "--host", "0.0.0.0", "--port", "8080",
            "-c", str(ctx), "-np", "1", "--cache-ram", str(cache_ram),
            "--no-webui", "--no-mmproj", "-ngl", "99"]
    if draft:
        # Speculative decoding roughly doubles throughput on this task at an
        # accuracy cost bounded, over six paired 10k arms, at +/-0.004 F1. That
        # is five times smaller than the +/-0.019 cross-card term already
        # accepted for every rented arm, so refusing it here while accepting
        # that one would be a habit rather than a principle.
        #
        # WITHIN A CONTROLLED PAIR IT MUST MATCH ON BOTH SIDES. Google's 26B
        # q4_0 build publishes no draft and unsloth's does, so that pair runs
        # without on both arms: enabling it on one side would put a known 26%
        # output change inside the comparison the pair exists to make.
        args += ["-hfd", draft]
    body = {"client_id": "me", "image": IMAGE, "disk": 60, "runtype": "args",
            "env": {"-p 8080:8080": "1"}, "args": args}
    if bid is not None:
        body["price"] = round(bid, 4)
    r = api("asks/%d/" % offer_id, method="PUT", body=body)
    if not r.get("success"):
        raise RuntimeError("launch refused: %s" % json.dumps(r)[:200])
    return r["new_contract"]


def endpoint(cid):
    """Wait for llama-server. Abandon a host ONLY on evidence that it is dead.

    There is no timeout here and there should not be one. Four were tried and all
    four were wrong in the same way: 420s, 900s, 600s-from-container-start, and a
    3600s cap. Each abandoned hosts that were working, because the time a host
    needs depends on model size and its bandwidth, neither known in advance, and
    the larger the model the more certain the false positive. A clock cannot tell
    a slow host from a dead one.

    What CAN tell them apart is the host saying so. These are the only conditions
    that end the wait unsuccessfully:

      - the instance reports `exited`
      - the container reports a docker daemon failure
      - the instance disappears from the API entirely

    Anything else is a host still working, and it gets as long as it takes.
    """
    ep = None
    while True:
        try:
            r = api("instances/%d/" % cid)
        except Exception:
            time.sleep(20); continue          # API blip is not evidence of death
        d = r.get("instances") or {}
        if not d:
            raise RuntimeError("instance %d no longer exists" % cid)
        status = d.get("actual_status")
        sm = d.get("status_msg") or ""
        if status == "exited":
            raise RuntimeError("instance exited: %s" % sm[:120].replace("\n", " "))
        if "Error response from daemon" in sm or "failed to set up container" in sm:
            raise RuntimeError("container failed: %s" % sm[:90].replace("\n", " "))
        p = ((d.get("ports") or {}).get("8080/tcp") or [{}])[0].get("HostPort")
        if p and d.get("public_ipaddr"):
            ep = "%s:%s" % (d["public_ipaddr"].strip(), p)
            try:
                urllib.request.urlopen("http://%s/health" % ep, timeout=10).read()
                return ep
            except Exception:
                pass                          # still loading weights; keep waiting
        time.sleep(20)


def verify(ep, repo, want_fam=None):
    """Defect 30: a server answering with someone else's weights. Same guard as
    shard_run.sh, including the VERIFY_FAM escape for publishers who do not name
    files after their repo."""
    props = json.loads(urllib.request.urlopen("http://%s/props" % ep, timeout=20).read())
    loaded = (props.get("model_path") or "").split("/")[-1]
    fam = want_fam or os.path.basename(repo.split(":")[0]).replace("-GGUF", "").replace("-gguf", "")
    quant = repo.split(":")[1] if ":" in repo else ""
    if fam not in loaded or (quant and quant not in loaded):
        raise RuntimeError("loaded %r, expected %s / %s" % (loaded, fam, quant))
    return loaded


def billed_rate(cid):
    """The rate actually charged, which is NOT the offer's dph_total.

    The offer price excludes storage and, on an interruptible rental, your bid
    premium. Logging the offer price understated the real burn by roughly 3x
    across a ten-instance fleet and produced a cost estimate that was wrong in
    the reassuring direction. Read it back from the instance instead.
    """
    try:
        d = (api("instances/%d/" % cid).get("instances")) or {}
        return float(d.get("dph_total") or 0.0)
    except Exception:
        return 0.0


def alive(cid):
    """False once the instance is gone or exited. A preempted interruptible
    instance disappears mid-arm and the client would otherwise hang on a dead
    endpoint until its 3600s timeout."""
    try:
        d = (api("instances/%d/" % cid).get("instances")) or {}
    except Exception:
        return True          # transient API error is not evidence of preemption
    return d.get("actual_status") not in ("exited", None) or bool(d.get("ports"))


def run_client(cmd, cid, log, label):
    """Run the client, watching for preemption. Returns (ok, preempted)."""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    preempted = False
    while proc.poll() is None:
        for _ in range(6):
            time.sleep(10)
            if proc.poll() is not None:
                break
        if proc.poll() is None and not alive(cid):
            log("    %s: instance %s vanished mid-arm (preempted); killing client" % (label, cid))
            preempted = True
            proc.kill()
            break
    out, err = proc.communicate()
    return (proc.returncode == 0 and not preempted), preempted


def destroy(cid):
    with LIVE_LOCK:
        LIVE.discard(cid)
    try:
        api("instances/%d/" % cid, method="DELETE", body={})
    except Exception as e:
        print("  WARN could not destroy %s: %s" % (cid, e), file=sys.stderr)


LIVE = set()
LIVE_LOCK = threading.Lock()

# Physical machines already claimed by this pool. Several vast OFFERS can belong
# to one machine, so placing arms concurrently by walking the price-ordered offer
# list double-books a single GPU: the first container takes it and the rest fail
# with "failed to set up container". Five of six arms died that way before this
# existed. Offer id is not identity; machine_id is.
CLAIMED = set()
CLAIM_LOCK = threading.Lock()


def claim(machine_id):
    with CLAIM_LOCK:
        if machine_id in CLAIMED:
            return False
        CLAIMED.add(machine_id)
        return True


def release(machine_id):
    with CLAIM_LOCK:
        CLAIMED.discard(machine_id)


def _reap(*_a):
    """Destroy every instance this pool created, then exit.

    Killing a pool with SIGTERM leaves its worker threads mid-run_arm, so the
    finally block that destroys the instance never executes and the rental keeps
    billing with nothing attached to it. That happened repeatedly: ten instances
    were found alive after their pools were gone, four of them orphans. This is
    the same defect as the orphaned llama clients on the local host, one layer up
    and with a meter attached.
    """
    with LIVE_LOCK:
        ids = sorted(LIVE)
    for cid in ids:
        try:
            api("instances/%d/" % cid, method="DELETE", body={})
            print("reaped instance %d" % cid, file=sys.stderr)
        except Exception:
            pass
    os._exit(0)


class Preempted(Exception):
    pass


def remaining_gold(gold_path, pred_path, scratch, tag):
    """Write a gold file of the notes a partial arm has not answered yet.

    Resume is done by narrowing the CORPUS, not by changing the client.
    run_llamacpp.py opens its output with "w" and has no resume path, and giving
    it one would be an instrument change for a billing problem.
    """
    done = set()
    if os.path.exists(pred_path):
        for l in open(pred_path):
            try:
                done.add(json.loads(l)["id"])
            except Exception:
                pass            # a torn final line from a killed process
    rows = [l for l in open(gold_path)
            if json.loads(l)["id"] not in done]
    out = os.path.join(scratch, "resume-%s.jsonl" % tag)
    with open(out, "w") as fh:
        fh.writelines(rows)
    return out, len(done), len(rows)


RUNNING = set()
RUNNING_LOCK = threading.Lock()


def claim_arm(label):
    """One client per arm, ever, across every pool on this machine.

    run_llamacpp.py opens its output with "w". Two clients on one arm therefore
    truncate each other continuously and the row count stops advancing while both
    burn rented GPU time. Four arms spent an hour doing this before it was
    noticed, because every endpoint was healthy and every client was alive; the
    only symptom was a counter that would not move.

    Duplicates arise two ways: a second pool whose job list overlaps the first,
    and a requeue that hands a job out while a worker still holds it. A lockfile
    keyed on the arm label covers both, including across separate pool processes.
    """
    lf = os.path.join(SCRATCH, "arm.%s.lock" % label.replace("/", "_"))
    try:
        fd = os.open(lf, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        os.write(fd, str(os.getpid()).encode())
        os.close(fd)
        return lf
    except FileExistsError:
        try:
            owner = int(open(lf).read().strip() or 0)
            os.kill(owner, 0)            # owner alive: genuinely taken
            return None
        except (ValueError, ProcessLookupError, PermissionError, OSError):
            os.remove(lf)                # stale lock from a dead pool
            return claim_arm(label)


def live_rows(pred):
    """STALE FILE GUARD.

    A prediction file on disk proves nothing about whether an arm is running. A
    46-row leftover from a previous attempt was read and reported as a live
    throughput figure with no process attached to it. Callers that report rates
    must confirm a client exists; this helper exists so the check has a name.
    """
    import subprocess
    if not os.path.exists(pred):
        return 0, False
    n = sum(1 for _ in open(pred))
    ps = subprocess.run(["ps", "-eo", "args"], capture_output=True, text=True).stdout
    return n, (os.path.basename(pred).replace(".pred.jsonl", "") in ps)


def run_arm(job, gpu, maxprice, outdir, log):
    """One instance, one model, and every prompt variant the job asks for.

    Batching matters: the image pull is billable dead time and is per-instance,
    so one instance per short arm pays it repeatedly. Variants of one model share
    a loaded server, which also makes the pair internally clean -- same card,
    same weights, same session, prompt as the only difference."""
    label, repo = job["label"], job["repo"]
    gold = job.get("gold", "data/corpora/v5/gold_small.jsonl")
    want = sum(1 for _ in open(os.path.join(ROOT, gold)))
    variants = job.get("prompts", ["live"])
    todo = [v for v in variants
            if not (os.path.exists(os.path.join(outdir, "%s.%s.pred.jsonl" % (label, v)))
                    and sum(1 for _ in open(os.path.join(outdir, "%s.%s.pred.jsonl" % (label, v)))) >= want)]
    if not todo:
        log("SKIP %s (all variants banked)" % label); return
    lock = claim_arm(label)
    if lock is None:
        log("SKIP %s (another client already holds this arm)" % label); return
    cid = None
    t0 = time.time()
    try:
        offers = find_offers(gpu, maxprice, 1, interruptible=job.get("_bid", False))
        if not offers:
            log("FAIL %s: no offer under %.3f" % (label, maxprice)); return
        ep = None
        # Try more offers than there are workers. Every worker queries the same
        # ordered list, so the cheapest offers collide and the losers get an
        # HTTP 400 from the ask being gone. That is a race, not a bad job: the
        # arm should walk down the list rather than fail.
        for off in offers[:24]:         # re-place on a taken ask, a dead host, or a busy machine
            mid = off.get("machine_id")
            if mid is not None and not claim(mid):
                continue                # another arm in this pool already has that GPU
            bid = None
            if job.get("_bid"):
                # 35% over the floor: enough to survive ordinary competition and
                # still far under on-demand.
                bid = max(off.get("min_bid") or off["dph_total"], 0.005) * 1.35
            try:
                cid = launch(off["id"], repo, job.get("ctx", 8192), job.get("cache_ram", 1024),
                             bid, job.get("draft"))
            except (urllib.error.HTTPError, RuntimeError) as e:
                log("    offer %s unavailable (%s); next offer" % (off["id"], e))
                release(off.get("machine_id"))
                cid = None
                continue
            with LIVE_LOCK:
                LIVE.add(cid)
            log("--- %s on %s %s contract %s (offer $%.4f/hr; billed rate read after start)"
                % (label, gpu, off["id"], cid, off["dph_total"]))
            try:
                ep = endpoint(cid)
                break
            except RuntimeError as e:
                log("    host %s unusable (%s); destroying and re-placing" % (off["id"], e))
                destroy(cid); release(off.get("machine_id")); cid = None
        if ep is None:
            log("FAIL %s: no host served within the pull timeout" % label); return
        rate = billed_rate(cid) or off["dph_total"]
        log("    billed $%.4f/hr (offer said $%.4f)" % (rate, off["dph_total"]))
        loaded = verify(ep, repo, job.get("verify_fam"))
        log("    healthy, loaded %s; %d variant(s): %s" % (loaded, len(todo), ",".join(todo)))
        # run_llamacpp.py REQUIRES an explicit thinking flag and has no default,
        # deliberately: an arm that silently inherits the wrong one is not
        # comparable to anything. Every banked arm in this project ran
        # --thinking, so that is what a job gets unless it says otherwise.
        for v in todo:
            pred = os.path.join(outdir, "%s.%s.pred.jsonl" % (label, v))
            use_gold = os.path.join(ROOT, gold)
            frag = pred + ".part"
            if job.get("resumable") and os.path.exists(frag):
                use_gold, ndone, nleft = remaining_gold(
                    os.path.join(ROOT, gold), frag, SCRATCH, "%s.%s" % (label, v))
                log("    resuming %s/%s: %d done, %d remaining" % (label, v, ndone, nleft))
            cmd = [sys.executable, os.path.join(ROOT, "harness/run_llamacpp.py"),
                   "--base-url", "http://" + ep, "--model", "%s.%s" % (label, v),
                   "--gold", use_gold, "--out", pred,
                   "--thinking" if job.get("thinking", True) else "--no-thinking",
                   "--max-tokens", str(job.get("max_tokens", 8192)),
                   "--prompt-version", v,
                   "--concurrency", "1"] + job.get("extra", [])
            # RESUME, and when it is allowed. Each note is an independent
            # request, so a prediction made on one machine is valid whatever made
            # the next one. What a mixed arm costs is the cross-card term, which
            # this project has now measured: rented 3090 against local 5080, same
            # config, +0.0057 F1 with 95% CI [-0.0136, +0.0251], tp and fn
            # identical and the whole difference in fp.
            #
            # So resume is fine when the arm's own tolerance is WIDER than that:
            # parse rates, reasoning present or absent, a model that scores 0.16
            # against one that scores 0.60. It is NOT fine for the paired ladders,
            # where the effects being chased (quant steps 0.0065-0.015, the MTP
            # null at +/-0.004) are smaller than the cross-card term, so a mixed
            # arm injects an uncontrolled shift of comparable size and unknown
            # sign. It is never fine for byte-identity or throughput work.
            #
            # Jobs declare this. The default is to discard, because the expensive
            # mistake is the silent one.
            # DO NOT delete the prediction file here. run_llamacpp.py opens its
            # output with "w", so a fresh client truncates it anyway, and the
            # delete turns every re-placement into permanent data loss: killing
            # four duplicate clients cost four arms' accumulated rows because the
            # pool deleted each file before restarting it. Rows that are already
            # committed survive; rows that are not do not.
            src_pred = pred + ".part"
            if os.path.exists(pred) and not os.path.exists(src_pred):
                os.replace(pred, src_pred)      # keep what exists, resume onto it
            # RETRY ON THE SAME INSTANCE. The weights are already downloaded and
            # that download is the expensive part of an arm: destroying an
            # instance because a client failed pays 6-21 GiB again on a fresh
            # host. A night of doing that on suspicion cost about $16 and
            # produced one complete arm. An instance is dropped ONLY on hard
            # evidence it is dead (exited, daemon error, vanished), never because
            # the client had a bad run.
            ok, preempted = run_client(cmd, cid, log, "%s/%s" % (label, v))
            if not ok and not preempted and alive(cid):
                log("    %s/%s client failed but the instance is alive; retrying on it" % (label, v))
                ok, preempted = run_client(cmd, cid, log, "%s/%s" % (label, v))
            if not ok:
                if preempted and job.get("resumable"):
                    log("    %s/%s preempted; %d rows kept for resume" % (
                        label, v, sum(1 for _ in open(pred)) if os.path.exists(pred) else 0))
                    if os.path.exists(pred):
                        os.replace(pred, src_pred)
                    raise Preempted(v)
                if os.path.exists(pred):
                    os.remove(pred)
                    log("    discarded partial %s (job is not resumable)" % os.path.basename(pred))
                log("FAIL %s/%s%s" % (label, v, " (preempted)" if preempted else ""))
                if preempted:
                    raise Preempted(v)
                continue
            if os.path.exists(src_pred):
                # stitch the earlier fragment back on, then drop it
                with open(pred, "a") as fh:
                    fh.write(open(src_pred).read())
                os.remove(src_pred)
                log("    %s/%s stitched with an earlier fragment (MIXED HARDWARE)" % (label, v))
            rows = [json.loads(l) for l in open(pred)]
            reasoned = sum(1 for x in rows if (x.get("reasoning_chars") or 0) > 0)
            pk = sum(1 for x in rows if x.get("parse_ok"))
            log("OK   %s/%s rows=%d reasoned=%d/%d parse_ok=%d" % (
                label, v, len(rows), reasoned, len(rows), pk))
        cost = rate * (time.time() - t0) / 3600.0
        log("DONE %s wall=%dm cost=$%.3f" % (label, (time.time() - t0) / 60, cost))
    except Preempted:
        log("REQUEUE %s after preemption" % label)
        if cid:
            destroy(cid)
        return "requeue"
    except Exception as e:
        log("FAIL %s: %s" % (label, e))
    finally:
        if cid:
            destroy(cid)
        try:
            os.remove(lock)
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", required=True, help="JSON file: list of {label,repo,gold,...}")
    ap.add_argument("--out", default="results/vast")
    ap.add_argument("--gpu", default="RTX 3090")
    ap.add_argument("--max-price", type=float, default=0.14)
    # Default: place EVERY arm at once. Arms are independent -- separate
    # instances, separate models, separate output files -- so throttling them
    # only makes the batch take longer for no benefit. The old default of 4 left
    # half an eight-arm list queued while the fleet was described as running.
    #
    # The real constraints are not this number. They are offer availability at
    # the price cap, the budget (burn is roughly arms x $0.05/hr), and the local
    # client processes, one per arm, which are HTTP-bound and cheap but not free.
    # 0 means "as many as there are jobs".
    ap.add_argument("--parallel", type=int, default=0,
                    help="max arms in flight; 0 (default) means one per job")
    ap.add_argument("--interruptible", action="store_true",
                    help="rent by bid. Roughly 5x cheaper on this fleet. An arm "
                         "that is preempted is DISCARDED and requeued whole, "
                         "never resumed, because a resumed arm would blend two "
                         "configurations.")
    ap.add_argument("--max-retries", type=int, default=2)
    a = ap.parse_args()
    if not KEY:
        print("VAST_API_KEY is not set", file=sys.stderr); return 2
    outdir = os.path.join(ROOT, a.out)
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(SCRATCH, exist_ok=True)
    lock = threading.Lock()
    logf = open(os.path.join(outdir, "vast.log"), "a")

    def log(m):
        line = "[%s] %s" % (time.strftime("%H:%M:%SZ", time.gmtime()), m)
        with lock:
            print(line); logf.write(line + "\n"); logf.flush()

    # DETACH INTO OUR OWN SESSION FIRST.
    #
    # The pool is normally started with `nohup setsid ... &` from a shell. When
    # that shell is itself killed -- which happens routinely, a tool timeout is
    # enough -- the signal goes to the whole process group. Before this, the
    # pool's own SIGTERM handler would then fire, destroy every instance it had
    # just rented, and exit cleanly. A stray signal aimed at a shell silently
    # tore down a fleet, and it looked like the pool had crashed.
    #
    # Becoming a session leader means group-directed signals no longer reach us.
    # A deliberate `kill <pid>` still does, which is the behaviour we want: reap
    # on purpose, survive by accident.
    try:
        os.setsid()
    except OSError:
        pass                    # already a session leader; nothing to do
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, _reap)
    signal.signal(signal.SIGINT, _reap)
    # Write the pid so a caller can stop this pool deliberately without pgrep,
    # which has repeatedly matched the caller's own command line.
    pidfile = os.path.join(SCRATCH, "vast_pool.%d.pid" % os.getpid())
    os.makedirs(SCRATCH, exist_ok=True)
    with open(pidfile, "w") as fh:
        fh.write("%d %s\n" % (os.getpid(), a.jobs))
    atexit.register(lambda: os.path.exists(pidfile) and os.remove(pidfile))
    jobs = json.load(open(a.jobs))
    for j in jobs:
        j["_bid"] = a.interruptible
        j["_tries"] = 0
    log("=== %d arms, %s, cap $%.3f/hr each" % (len(jobs), a.gpu, a.max_price))
    q = list(jobs)
    qlock = threading.Lock()

    def worker():
        while True:
            with qlock:
                if not q: return
                job = q.pop(0)
            r = run_arm(job, a.gpu, a.max_price, outdir, log)
            if r == "requeue" and job["_tries"] < a.max_retries:
                job["_tries"] += 1
                with qlock:
                    q.append(job)

    width = len(jobs) if a.parallel <= 0 else min(a.parallel, len(jobs))
    log("    placing %d arm(s) concurrently" % width)
    ts = [threading.Thread(target=worker) for _ in range(width)]
    for t in ts: t.start()
    for t in ts: t.join()
    log("=== pool drained")
    return 0


if __name__ == "__main__":
    sys.exit(main())
