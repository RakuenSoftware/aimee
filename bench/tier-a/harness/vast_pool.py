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
import argparse, json, os, subprocess, sys, threading, time
import urllib.error, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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


def launch(offer_id, repo, ctx, cache_ram, bid=None):
    """bid=None rents on-demand; a float rents interruptible at that price.

    Bid ABOVE min_bid rather than at it. Bidding the floor is what gets an
    instance outbid first, and a preemption costs the whole arm: an arm cannot be
    resumed on another machine without becoming a blend of two configurations,
    which is the hazard defect 40 and article 04 are about."""
    args = ["-hf", repo, "--host", "0.0.0.0", "--port", "8080",
            "-c", str(ctx), "-np", "1", "--cache-ram", str(cache_ram),
            "--no-webui", "--no-mmproj", "-ngl", "99"]
    body = {"client_id": "me", "image": IMAGE, "disk": 60, "runtype": "args",
            "env": {"-p 8080:8080": "1"}, "args": args}
    if bid is not None:
        body["price"] = round(bid, 4)
    r = api("asks/%d/" % offer_id, method="PUT", body=body)
    if not r.get("success"):
        raise RuntimeError("launch refused: %s" % json.dumps(r)[:200])
    return r["new_contract"]


def endpoint(cid, timeout=420):
    """Wait for the mapped port, then for llama-server to answer /health.

    The timeout is short on purpose. Advertised inet_down does not predict
    registry throughput: one host pulled this image in 84 seconds and another,
    advertising comparable bandwidth, sat on 'Retrying in 2 seconds' for fifteen
    billable minutes. A slow pull is a host to replace, not a wait to sit out.
    """
    t0 = time.time()
    ep = None
    while time.time() - t0 < timeout:
        try:
            d = (api("instances/%d/" % cid).get("instances")) or {}
        except Exception:
            time.sleep(15); continue
        if d.get("actual_status") == "exited":
            raise RuntimeError("instance exited: %s" % (d.get("status_msg") or "")[:120])
        p = ((d.get("ports") or {}).get("8080/tcp") or [{}])[0].get("HostPort")
        if p and d.get("public_ipaddr"):
            ep = "%s:%s" % (d["public_ipaddr"].strip(), p)
            try:
                urllib.request.urlopen("http://%s/health" % ep, timeout=10).read()
                return ep
            except Exception:
                pass
        time.sleep(15)
    raise RuntimeError("never became healthy (last endpoint %s)" % ep)


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
    try:
        api("instances/%d/" % cid, method="DELETE", body={})
    except Exception as e:
        print("  WARN could not destroy %s: %s" % (cid, e), file=sys.stderr)


class Preempted(Exception):
    pass


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
    cid = None
    t0 = time.time()
    try:
        offers = find_offers(gpu, maxprice, 1, interruptible=job.get("_bid", False))
        if not offers:
            log("FAIL %s: no offer under %.3f" % (label, maxprice)); return
        ep = None
        for off in offers[:3]:          # re-place on a slow puller, do not wait it out
            bid = None
            if job.get("_bid"):
                # 35% over the floor: enough to survive ordinary competition and
                # still far under on-demand.
                bid = max(off.get("min_bid") or off["dph_total"], 0.005) * 1.35
            cid = launch(off["id"], repo, job.get("ctx", 8192), job.get("cache_ram", 1024), bid)
            log("--- %s on %s %s $%.3f/hr contract %s" % (label, gpu, off["id"], off["dph_total"], cid))
            try:
                ep = endpoint(cid)
                break
            except RuntimeError as e:
                log("    host %s unusable (%s); destroying and re-placing" % (off["id"], e))
                destroy(cid); cid = None
        if ep is None:
            log("FAIL %s: no host served within the pull timeout" % label); return
        loaded = verify(ep, repo, job.get("verify_fam"))
        log("    healthy, loaded %s; %d variant(s): %s" % (loaded, len(todo), ",".join(todo)))
        # run_llamacpp.py REQUIRES an explicit thinking flag and has no default,
        # deliberately: an arm that silently inherits the wrong one is not
        # comparable to anything. Every banked arm in this project ran
        # --thinking, so that is what a job gets unless it says otherwise.
        for v in todo:
            pred = os.path.join(outdir, "%s.%s.pred.jsonl" % (label, v))
            cmd = [sys.executable, os.path.join(ROOT, "harness/run_llamacpp.py"),
                   "--base-url", "http://" + ep, "--model", "%s.%s" % (label, v),
                   "--gold", os.path.join(ROOT, gold), "--out", pred,
                   "--thinking" if job.get("thinking", True) else "--no-thinking",
                   "--max-tokens", str(job.get("max_tokens", 8192)),
                   "--prompt-version", v,
                   "--concurrency", "1"] + job.get("extra", [])
            ok, preempted = run_client(cmd, cid, log, "%s/%s" % (label, v))
            if not ok:
                # A partial arm is never kept. run_llamacpp.py opens its output
                # with "w" so a rerun truncates anyway, but leaving a short file
                # on disk invites a later reader to score it as if it were an arm.
                if os.path.exists(pred):
                    os.remove(pred)
                    log("    discarded partial %s" % os.path.basename(pred))
                log("FAIL %s/%s%s" % (label, v, " (preempted)" if preempted else ""))
                if preempted:
                    raise Preempted(v)
                continue
            rows = [json.loads(l) for l in open(pred)]
            reasoned = sum(1 for x in rows if (x.get("reasoning_chars") or 0) > 0)
            pk = sum(1 for x in rows if x.get("parse_ok"))
            log("OK   %s/%s rows=%d reasoned=%d/%d parse_ok=%d" % (
                label, v, len(rows), reasoned, len(rows), pk))
        cost = off["dph_total"] * (time.time() - t0) / 3600.0
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", required=True, help="JSON file: list of {label,repo,gold,...}")
    ap.add_argument("--out", default="results/vast")
    ap.add_argument("--gpu", default="RTX 3090")
    ap.add_argument("--max-price", type=float, default=0.14)
    ap.add_argument("--parallel", type=int, default=4)
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
    lock = threading.Lock()
    logf = open(os.path.join(outdir, "vast.log"), "a")

    def log(m):
        line = "[%s] %s" % (time.strftime("%H:%M:%SZ", time.gmtime()), m)
        with lock:
            print(line); logf.write(line + "\n"); logf.flush()

    jobs = json.load(open(a.jobs))
    for j in jobs:
        j["_bid"] = a.interruptible
        j["_tries"] = 0
    log("=== %d arms, %s, up to %d at once, cap $%.3f/hr each"
        % (len(jobs), a.gpu, a.parallel, a.max_price))
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

    ts = [threading.Thread(target=worker) for _ in range(min(a.parallel, len(jobs)))]
    for t in ts: t.start()
    for t in ts: t.join()
    log("=== pool drained")
    return 0


if __name__ == "__main__":
    sys.exit(main())
