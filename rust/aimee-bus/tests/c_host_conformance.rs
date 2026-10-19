//! Cross-language interop: this Rust client against the single in-source C
//! host, across a process boundary. The C host runs inside
//! `bus-conformance-host` together with an internal C client that acks
//! notifications, serves an echo kind, and reports cancels.
//!
//! Ignored by a plain `cargo test` because it needs the built harness;
//! `scripts/test_bus_rust_conformance.sh` builds it and runs this with
//! `--ignored`, so in CI it cannot be skipped silently.

use aimee_bus::wire::{F_REPLY, KIND_CAPABILITY_ABSENT};
use aimee_bus::{BusError, Client, Event};
use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::{Duration, Instant};

// Kinds must match src/tests/bus_conformance_host.c.
const KIND_NOTIFY: u32 = 1000;
const KIND_ACK: u32 = 1001;
const KIND_ECHO: u32 = 1002;
const KIND_NOSERVER: u32 = 1003;
const KIND_CANCEL_ACK: u32 = 1004;

struct Harness {
    child: Child,
    dir: PathBuf,
}

impl Drop for Harness {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

fn connect(path: &Path) -> std::io::Result<OwnedFd> {
    // SAFETY: plain socket(2)/connect(2) on a zero-initialised sockaddr_un.
    unsafe {
        let raw = libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC, 0);
        if raw < 0 {
            return Err(std::io::Error::last_os_error());
        }
        let fd = OwnedFd::from_raw_fd(raw);
        let mut addr: libc::sockaddr_un = std::mem::zeroed();
        addr.sun_family = libc::AF_UNIX as libc::sa_family_t;
        for (d, s) in addr.sun_path.iter_mut().zip(path.as_os_str().as_bytes()) {
            *d = *s as libc::c_char;
        }
        let len = std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t;
        if libc::connect(
            fd.as_raw_fd(),
            &addr as *const _ as *const libc::sockaddr,
            len,
        ) != 0
        {
            return Err(std::io::Error::last_os_error());
        }
        Ok(fd)
    }
}

fn poll_for(c: &Client, what: &str, pred: impl Fn(&Event) -> bool) -> Event {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        match c.poll() {
            Ok(Some(ev)) if pred(&ev) => return ev,
            Ok(Some(_)) => {}
            Ok(None) => std::thread::sleep(Duration::from_millis(2)),
            Err(e) => panic!("poll ({what}): {e}"),
        }
    }
    panic!("timed out waiting for {what}");
}

fn start_harness() -> (Harness, PathBuf) {
    let harness = std::env::var("BUS_CONFORMANCE_HOST")
        .expect("BUS_CONFORMANCE_HOST must name the built bus-conformance-host");
    // A short directory keeps the socket path under sun_path's limit.
    let base = std::env::var_os("TMPDIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| "/tmp".into());
    let dir = base.join(format!("aimee-bus-rs-{}", std::process::id()));
    std::fs::create_dir_all(&dir).expect("create socket directory");
    let sock = dir.join("bus.sock");
    let child = Command::new(&harness)
        .arg(&sock)
        .arg("15000")
        .spawn()
        .expect("start harness");
    let h = Harness { child, dir };
    let deadline = Instant::now() + Duration::from_secs(5);
    while !sock.exists() {
        assert!(Instant::now() < deadline, "harness socket did not appear");
        std::thread::sleep(Duration::from_millis(10));
    }
    (h, sock)
}

#[test]
#[ignore = "needs the C bus-conformance-host; run by scripts/test_bus_rust_conformance.sh"]
fn rust_client_against_c_host() {
    let (_harness, sock) = start_harness();

    let fd = connect(&sock).expect("connect");
    let c = Client::attach(fd.as_fd()).expect("attach");
    let a = c.attachment();
    println!(
        "attached: handle={} slot={} cap={}",
        a.handle, a.slot_size, a.queue_capacity
    );

    // 1. Rust -> C -> Rust: a notification the C client acks.
    c.publish(KIND_NOTIFY, b"rust-says-hi").unwrap();
    let ack = poll_for(&c, "ack", |e| e.frame.event_kind == KIND_ACK);
    assert_eq!(ack.payload, b"rust-says-hi");

    // 2. Request/reply across the language boundary.
    c.request(KIND_ECHO, 0xABCDEF, b"echo-me").unwrap();
    let r = poll_for(&c, "echo reply", |e| {
        e.frame.event_kind == KIND_ECHO && e.frame.hdr_flags & F_REPLY != 0
    });
    assert_eq!(
        (r.frame.correlation_id, r.payload.as_slice()),
        (0xABCDEF, &b"echo-me"[..])
    );

    // 3. A kind with no server draws a synthesized capability_absent reply.
    c.request(KIND_NOSERVER, 0x999, &[]).unwrap();
    let ca = poll_for(&c, "capability_absent", |e| {
        e.frame.event_kind == KIND_CAPABILITY_ABSENT
    });
    assert_eq!(ca.frame.correlation_id, 0x999);
    assert_ne!(ca.frame.hdr_flags & F_REPLY, 0);

    // 4. Cancel reaches the C server, which acks it.
    c.request(KIND_ECHO, 0x5151, b"to-cancel").unwrap();
    c.cancel(KIND_ECHO, 0x5151).unwrap();
    poll_for(&c, "cancel ack", |e| e.frame.event_kind == KIND_CANCEL_ACK);

    // 5. Oversized and uncorrelated emits are refused locally, not sent.
    assert!(matches!(
        c.publish(KIND_NOTIFY, &vec![0u8; a.inline_budget as usize + 1]),
        Err(BusError::Payload)
    ));
    assert!(matches!(
        c.request(KIND_ECHO, 0, b"x"),
        Err(BusError::Protocol(_))
    ));

    // 6. Backpressure: with no consumer draining fast enough, a burst ends in
    // WouldBlock rather than an overwrite. The host drains it afterwards.
    let mut accepted = 0;
    let blocked = loop {
        match c.publish(KIND_NOTIFY, b"burst") {
            Ok(()) => accepted += 1,
            Err(BusError::WouldBlock) => break true,
            Err(e) => panic!("burst: {e}"),
        }
        if accepted > 100_000 {
            break false;
        }
    };
    assert!(blocked, "the outbound ring never reported backpressure");
    assert!(accepted >= 1);
    while poll_for_opt(&c) {}

    // 7. Reaped-client recovery: drop this client, attach a fresh one into
    // the freed slot, and use it.
    drop(c);
    drop(fd);
    let mut fd2 = None;
    for _ in 0..200 {
        if let Ok(f) = connect(&sock) {
            fd2 = Some(f);
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let fd2 = fd2.expect("reconnect after reap");
    let c2 = Client::attach(fd2.as_fd()).expect("attach after reap");
    c2.request(KIND_ECHO, 0x7777, b"after-recovery").unwrap();
    let r2 = poll_for(&c2, "echo after recovery", |e| {
        e.frame.event_kind == KIND_ECHO
            && e.frame.hdr_flags & F_REPLY != 0
            && e.frame.correlation_id == 0x7777
    });
    assert_eq!(r2.payload, b"after-recovery");
    assert_eq!(c2.dropped_frames(), 0);
}

/// Drain whatever is pending for a short while; true while events arrive.
fn poll_for_opt(c: &Client) -> bool {
    let deadline = Instant::now() + Duration::from_millis(200);
    while Instant::now() < deadline {
        if let Ok(Some(_)) = c.poll() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    false
}

#[test]
fn missing_socket_fails_immediately() {
    let missing =
        std::env::temp_dir().join(format!("aimee-bus-missing-{}.sock", std::process::id()));
    let stop = std::sync::atomic::AtomicBool::new(true);
    match Client::connect(&missing, 1, 7, &stop) {
        Err(BusError::Io(e)) => assert_eq!(e.kind(), std::io::ErrorKind::NotFound),
        other => panic!("a missing socket must fail immediately, got {other:?}"),
    }
}
