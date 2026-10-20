//! Conformance caller: the Rust module caller against the C host and a real
//! module process, across process boundaries.
//!
//! `unit-test-module-runtime`, given `AIMEE_TEST_MODULE_CALLER`, admits this
//! binary by executable path and peer credentials as principal class 1 ref
//! 91, granted to request kinds 5889 (served: an echo stage that waits on
//! "cancel" until cancelled) and 5890 (granted, never served), and spawns it
//! once the module is up. It exits 0 only if every outcome below matches the
//! C core caller's on the same host. The module behind kind 5889 is whichever
//! the harness runs: the C runtime in-process, or an external C, Go or Rust
//! module process.

use aimee_bus::Client;
use aimee_bus::module_caller::{Call, CallError, ModuleCaller};
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

const TEST_KIND: u32 = 5889;
const EMPTY_KIND: u32 = 5890;
/// Not in this caller's grant.
const DENIED_KIND: u32 = 5891;
const TEST_STAGE: u32 = 1;
const PRINCIPAL_CLASS: u32 = 1;
const PRINCIPAL_REF: u32 = 91;
const LARGE_BODY: usize = 128 * 1024 + 37;
/// Bound on every call that should complete, so a broken host cannot hang CI.
const CALL_TIMEOUT: Duration = Duration::from_secs(20);

fn fail(what: &str, detail: impl std::fmt::Display) -> ! {
    eprintln!("module caller check: {what}: {detail}");
    std::process::exit(1);
}

fn expect_ok(what: &str, got: Result<Vec<u8>, CallError>, want: &[u8]) {
    match got {
        Ok(body) if body == want => println!("module caller check: {what}: ok"),
        Ok(body) => fail(
            what,
            format!("body of {} bytes, want {}", body.len(), want.len()),
        ),
        Err(e) => fail(what, e),
    }
}

fn expect_err(what: &str, got: Result<Vec<u8>, CallError>, want: &str) {
    match got {
        Err(e) if e.name() == want => println!("module caller check: {what}: {want}"),
        Err(e) => fail(what, format!("{e}, want {want}")),
        Ok(body) => fail(
            what,
            format!("succeeded with {} bytes, want {want}", body.len()),
        ),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: {} DAEMON_MODULE_BUS_SOCKET", args[0]);
        std::process::exit(2);
    }
    let stop = AtomicBool::new(false);
    let client = match Client::connect(Path::new(&args[1]), PRINCIPAL_CLASS, PRINCIPAL_REF, &stop) {
        Ok(Some(c)) => c,
        Ok(None) => fail("attach", "stopped"),
        Err(e) => fail("attach", e),
    };
    let caller = ModuleCaller::new(&client);
    let call = |request: &'static [u8], trace: u64| {
        Call::new(TEST_KIND, TEST_STAGE, request)
            .trace_id(trace)
            .timeout(CALL_TIMEOUT)
    };

    expect_ok(
        "echo",
        caller.call(&call(b"rust-caller", 2001)),
        b"rust-caller",
    );
    expect_ok("empty request", caller.call(&call(b"", 2002)), b"");

    let large: Vec<u8> = (0..LARGE_BODY as u32)
        .map(|i| (i.wrapping_mul(131).wrapping_add(17)) as u8)
        .collect();
    let large_call = |trace| {
        Call::new(TEST_KIND, TEST_STAGE, &large)
            .trace_id(trace)
            .timeout(CALL_TIMEOUT)
    };
    expect_ok(
        "fragmented request and reply",
        caller.call(&large_call(2003)),
        &large,
    );
    match caller.call(&large_call(2004).response_limit(64)) {
        Err(CallError::ResponseTooLarge { len }) if len as usize == LARGE_BODY => {
            println!("module caller check: response over limit: RESPONSE_TOO_LARGE ({len})")
        }
        other => fail("response over limit", format!("{other:?}")),
    }
    // Every fragment of that response was drained: the next call is clean.
    expect_ok(
        "after a drained response",
        caller.call(&call(b"clean", 2005)),
        b"clean",
    );

    expect_err(
        "expired deadline",
        caller.call(&Call::new(TEST_KIND, TEST_STAGE, b"late").deadline_ns(1)),
        "DEADLINE_EXCEEDED",
    );

    // "cancel" holds the handler until the call is cancelled, so these two
    // reach the module and must be cancelled there. The module's terminal
    // CANCELLED replies can arrive after the calls returned.
    let started = Instant::now();
    expect_err(
        "deadline in flight",
        caller.call(
            &Call::new(TEST_KIND, TEST_STAGE, b"cancel")
                .trace_id(2006)
                .timeout(Duration::from_millis(100)),
        ),
        "DEADLINE_EXCEEDED",
    );
    if started.elapsed() < Duration::from_millis(100) {
        fail("deadline in flight", "returned before its deadline");
    }
    let flag = Arc::new(AtomicBool::new(false));
    let setter = flag.clone();
    let t = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(20));
        setter.store(true, Ordering::Release);
    });
    expect_err(
        "cancellation",
        caller.call_cancellable(&call(b"cancel", 2007), &|| flag.load(Ordering::Acquire)),
        "CANCELLED",
    );
    t.join().expect("cancel thread");

    // The cancel must reach the module, not just end the call here. The host
    // retires a cancelled correlation, so the module's late reply never comes
    // back to observe; its handler slots are what show it. A "cancel" handler
    // stops only when cancelled (its deadline is CALL_TIMEOUT away) and a
    // module runs at most 16 at once, so if these cancels were lost the slots
    // would stay taken and the echo after them would be refused.
    for i in 0..20u64 {
        let started = Instant::now();
        expect_err(
            "cancel delivery",
            caller.call_cancellable(&call(b"cancel", 2100 + i), &|| {
                started.elapsed() > Duration::from_millis(5)
            }),
            "CANCELLED",
        );
    }
    std::thread::sleep(Duration::from_millis(50));
    expect_ok(
        "cancels reached the module (its handler slots are free)",
        caller.call(&call(b"after", 2008)),
        b"after",
    );

    expect_err(
        "granted kind with no server",
        caller.call(
            &Call::new(EMPTY_KIND, 2, b"")
                .trace_id(2009)
                .timeout(CALL_TIMEOUT),
        ),
        "CAPABILITY_ABSENT",
    );
    let started = Instant::now();
    expect_err(
        "kind outside the grant",
        caller.call(
            &Call::new(DENIED_KIND, 3, b"x")
                .trace_id(2010)
                .timeout(CALL_TIMEOUT),
        ),
        "CAPABILITY_DENIED",
    );
    if started.elapsed() >= CALL_TIMEOUT {
        fail("kind outside the grant", "waited out its deadline");
    }

    // Shared across threads, calls take turns on the one reply stream and
    // each gets its own reply.
    std::thread::scope(|s| {
        for i in 0..4u8 {
            let caller = &caller;
            s.spawn(move || {
                for j in 0..10u8 {
                    let body = vec![b'a' + i; 1 + usize::from(j) * 97];
                    let got = caller.call(
                        &Call::new(TEST_KIND, TEST_STAGE, &body)
                            .trace_id(3000 + u64::from(i) * 100 + u64::from(j))
                            .timeout(CALL_TIMEOUT),
                    );
                    match got {
                        Ok(b) if b == body => {}
                        other => fail("shared caller", format!("thread {i} call {j}: {other:?}")),
                    }
                }
            });
        }
    });
    println!("module caller check: shared caller: ok");
    println!("module caller check: passed");
}
