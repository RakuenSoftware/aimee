//! Client behaviour against a scripted attach peer (tests/common): the attach
//! handshake and every refusal, emit and poll, backpressure, epoch and
//! liveness, concurrency, and arena reads with lease release (D12).

mod common;

use aimee_bus::wire::*;
use aimee_bus::{AttachStatus, BusError, Client};
use common::*;
use std::os::fd::AsFd;
use std::sync::Arc;
use std::time::Duration;

fn attach_with(
    host: &FakeHost,
    reply: &[u8],
    fds: Vec<std::os::fd::RawFd>,
) -> Result<Client, BusError> {
    let (client_sock, host_sock) = socketpair();
    let reply = reply.to_vec();
    let t = std::thread::spawn(move || {
        read_request(&host_sock);
        send_reply(&host_sock, &reply, &fds);
        host_sock
    });
    let r = Client::attach_as(client_sock.as_fd(), 1, 7);
    drop(t.join().unwrap());
    let _ = host;
    r
}

fn frame(flags: u16, kind: u32, corr: u64) -> Frame {
    Frame {
        hdr_flags: flags,
        wire_version: WIRE_VERSION,
        event_kind: kind,
        correlation_id: corr,
        ..Frame::default()
    }
}

fn inline(flags: u16, kind: u32, corr: u64, len: usize) -> Frame {
    Frame {
        hdr_flags: flags | F_INLINE,
        payload_ref: HDR_LEN as u64,
        payload_len: len as u32,
        ..frame(flags, kind, corr)
    }
}

// ---- attach -------------------------------------------------------------

#[test]
fn attach_sends_the_request_and_maps_the_regions() {
    let host = FakeHost::new(Geometry::default());
    let (client_sock, host_sock) = socketpair();
    let fds = host.fds();
    let reply = host.reply(0, 5);
    let t = std::thread::spawn(move || {
        let req = read_request(&host_sock);
        send_reply(&host_sock, &reply, &fds);
        req
    });
    let c = Client::attach_as(client_sock.as_fd(), 1, 42).expect("attach");
    let (min, max, class, principal) = t.join().unwrap();
    assert_eq!(
        (min, max, class, principal),
        (3, 3, 1, 42),
        "the request carries the version range and claims"
    );
    let a = c.attachment();
    assert_eq!(
        (
            a.handle,
            a.slot_size,
            a.inline_budget,
            a.queue_capacity,
            a.arena_size
        ),
        (5, 256, 192, 8, 64 * 1024)
    );
    assert!(
        c.arena_readable(),
        "the fake host advertises the lease view"
    );
    assert!(!c.epoch_changed() && !c.control_lost());
}

#[test]
fn every_refusal_status_is_reported_distinctly() {
    let host = FakeHost::new(Geometry::default());
    for (wire, want) in [
        (1, AttachStatus::DeniedPolicy),
        (2, AttachStatus::DeniedVersion),
        (3, AttachStatus::DeniedNoSlot),
        (4, AttachStatus::Protocol),
        (99, AttachStatus::Unknown(99)),
    ] {
        match attach_with(&host, &host.reply(wire, 0), vec![]) {
            Err(BusError::Denied(s)) => {
                assert_eq!(s, want);
                assert!(!s.to_string().is_empty());
            }
            other => panic!("status {wire}: {other:?}"),
        }
    }
}

#[test]
fn malformed_replies_are_refused() {
    let host = FakeHost::new(Geometry::default());
    let fds = host.fds().to_vec();
    let mut bad_magic = host.reply(0, 1);
    bad_magic[0] ^= 0xff;
    let mut bad_version = host.reply(0, 1);
    bad_version[12..16].copy_from_slice(&2u32.to_le_bytes());
    let mut zero_slot = host.reply(0, 1);
    zero_slot[16..20].copy_from_slice(&0u32.to_le_bytes());
    let mut inline_past_slot = host.reply(0, 1);
    inline_past_slot[20..24].copy_from_slice(&257u32.to_le_bytes());
    let mut disagrees = host.reply(0, 1);
    disagrees[20..24].copy_from_slice(&100u32.to_le_bytes());
    let short = host.reply(0, 1)[..40].to_vec();

    let cases: Vec<(&str, Vec<u8>, Vec<std::os::fd::RawFd>)> = vec![
        ("bad magic", bad_magic.to_vec(), fds.clone()),
        (
            "unsupported wire version",
            bad_version.to_vec(),
            fds.clone(),
        ),
        ("zero slot", zero_slot.to_vec(), fds.clone()),
        ("inline past slot", inline_past_slot.to_vec(), fds.clone()),
        (
            "reply disagrees with control",
            disagrees.to_vec(),
            fds.clone(),
        ),
        ("short reply", short, fds.clone()),
        (
            "two descriptors",
            host.reply(0, 1).to_vec(),
            fds[..2].to_vec(),
        ),
        ("no descriptors", host.reply(0, 1).to_vec(), vec![]),
    ];
    for (name, reply, f) in cases {
        match attach_with(&host, &reply, f) {
            Err(BusError::Protocol(_)) | Err(BusError::Region(_)) => {}
            other => panic!("{name}: accepted or wrong error: {other:?}"),
        }
    }
}

#[test]
fn corrupt_regions_are_refused() {
    type Poke = fn(&FakeHost);
    let pokes: [(&str, Poke); 13] = [
        ("control magic", |h| {
            h.control
                .u32(0)
                .store(0, std::sync::atomic::Ordering::Relaxed)
        }),
        ("spec version", |h| {
            h.control
                .u32(4)
                .store(9, std::sync::atomic::Ordering::Relaxed)
        }),
        ("layout version", |h| {
            h.control
                .u32(8)
                .store(9, std::sync::atomic::Ordering::Relaxed)
        }),
        ("view slot count", |h| {
            h.control
                .u32(28)
                .store(5, std::sync::atomic::Ordering::Relaxed)
        }),
        ("view offset", |h| {
            h.control
                .u64(56)
                .store(64, std::sync::atomic::Ordering::Relaxed)
        }),
        ("qpair magic", |h| {
            h.qpair
                .u32(0)
                .store(0, std::sync::atomic::Ordering::Relaxed)
        }),
        ("qpair offsets equal", |h| {
            h.qpair
                .u32(28)
                .store(h.in_off as u32, std::sync::atomic::Ordering::Relaxed)
        }),
        ("ring magic", |h| {
            h.qpair
                .u32(h.in_off)
                .store(0, std::sync::atomic::Ordering::Relaxed)
        }),
        ("ring capacity", |h| {
            h.qpair
                .u32(h.out_off + 8)
                .store(3, std::sync::atomic::Ordering::Relaxed)
        }),
        ("ring mask", |h| {
            h.qpair
                .u32(h.out_off + 12)
                .store(1, std::sync::atomic::Ordering::Relaxed)
        }),
        ("arena magic", |h| {
            h.arena
                .u32(0)
                .store(0, std::sync::atomic::Ordering::Relaxed)
        }),
        ("arena size smaller than announced", |h| {
            h.arena
                .u64(8)
                .store(h.g.arena - 1, std::sync::atomic::Ordering::Relaxed)
        }),
        ("arena size larger than announced", |h| {
            h.arena
                .u64(8)
                .store(h.g.arena + 1, std::sync::atomic::Ordering::Relaxed)
        }),
    ];
    for (name, poke) in pokes {
        let host = FakeHost::new(Geometry::default());
        poke(&host);
        let r = attach_with(&host, &host.reply(0, 1), host.fds().to_vec());
        assert!(matches!(r, Err(BusError::Region(_))), "{name}: {r:?}");
    }
}

#[test]
fn a_region_shorter_than_its_geometry_is_refused() {
    let host = FakeHost::new(Geometry::default());
    // Shrink the queue-pair memfd under the client's announced geometry.
    // SAFETY: test-only ftruncate on our own memfd.
    unsafe { libc::ftruncate(std::os::fd::AsRawFd::as_raw_fd(&host.qpair.fd), 128) };
    let r = attach_with(&host, &host.reply(0, 1), host.fds().to_vec());
    assert!(matches!(r, Err(BusError::Io(_))), "{r:?}");
}

#[test]
fn an_arena_shorter_than_announced_is_refused() {
    // Reading a lease span past the end of the memfd would fault, so a short
    // arena is refused at attach rather than on the first arena event.
    let host = FakeHost::new(Geometry::default());
    let short = 16 + host.g.arena as i64 - 1;
    // SAFETY: test-only ftruncate on our own memfd.
    unsafe { libc::ftruncate(std::os::fd::AsRawFd::as_raw_fd(&host.arena.fd), short) };
    let r = attach_with(&host, &host.reply(0, 1), host.fds().to_vec());
    assert!(matches!(r, Err(BusError::Io(_))), "{r:?}");
}

#[test]
fn an_older_host_without_the_lease_view_still_attaches() {
    let host = FakeHost::new(Geometry {
        with_view: false,
        ..Geometry::default()
    });
    let c = attach(&host);
    assert!(!c.arena_readable());
}

// ---- emit ---------------------------------------------------------------

#[test]
fn emitted_frames_are_exact() {
    let host = FakeHost::new(Geometry::default());
    let c = attach(&host);
    c.publish(300, b"note").unwrap();
    c.request(301, 7, b"ask").unwrap();
    c.request_fragment(301, 8, b"part", true).unwrap();
    c.reply(302, 9, b"").unwrap();
    c.reply_fragment(302, 10, b"more", true).unwrap();
    c.cancel(301, 7).unwrap();
    let out = host.outbound();
    let want: [(u16, u32, u64, &[u8]); 6] = [
        (F_NOTIFICATION | F_INLINE, 300, 0, b"note"),
        (F_REQUEST | F_INLINE, 301, 7, b"ask"),
        (F_REQUEST | F_INLINE | F_MORE, 301, 8, b"part"),
        (F_REPLY, 302, 9, b""),
        (F_REPLY | F_INLINE | F_MORE, 302, 10, b"more"),
        (F_CANCEL, 301, 7, b""),
    ];
    for (flags, kind, corr, payload) in want {
        let (f, p) = out.pop().expect("frame");
        assert_eq!(
            (f.hdr_flags, f.event_kind, f.correlation_id),
            (flags, kind, corr)
        );
        assert_eq!(f.src_handle, 3, "the client stamps its handle");
        assert_eq!(p, payload);
    }
    assert!(out.pop().is_none());
}

#[test]
fn invalid_emits_are_refused_locally() {
    let host = FakeHost::new(Geometry::default());
    let c = attach(&host);
    assert!(
        matches!(c.publish(300, &[0u8; 193]), Err(BusError::Payload)),
        "past the inline budget"
    );
    assert!(
        matches!(c.request(300, 0, b"x"), Err(BusError::Protocol(_))),
        "request without correlation"
    );
    assert!(
        matches!(c.reply(300, 0, b"x"), Err(BusError::Protocol(_))),
        "reply without correlation"
    );
    assert!(
        matches!(c.cancel(300, 0), Err(BusError::Protocol(_))),
        "cancel without correlation"
    );
    assert!(
        matches!(
            c.request_fragment(300, 1, b"", true),
            Err(BusError::Protocol(_))
        ),
        "empty fragment"
    );
    assert_eq!(host.outbound().count(), 0, "nothing reached the ring");
    assert!(
        c.publish(300, &[0u8; 192]).is_ok(),
        "exactly the budget fits"
    );
}

#[test]
fn a_full_outbound_ring_is_would_block_never_an_overwrite() {
    let host = FakeHost::new(Geometry {
        cap: 4,
        ..Geometry::default()
    });
    let c = attach(&host);
    for i in 0..4u8 {
        c.publish(300, &[i]).unwrap();
    }
    assert!(matches!(c.publish(300, b"x"), Err(BusError::WouldBlock)));
    for i in 0..4u8 {
        assert_eq!(
            host.outbound().pop().unwrap().1,
            vec![i],
            "FIFO, nothing overwritten"
        );
    }
    c.publish(300, b"after").unwrap();
}

#[test]
fn the_inline_budget_bounded_by_the_slot_is_reported() {
    let host = FakeHost::new(Geometry {
        slot: 256,
        inline: 256,
        ..Geometry::default()
    });
    let c = attach(&host);
    assert_eq!(c.inline_budget(), 256);
    assert_eq!(
        c.max_inline_payload(),
        192,
        "a slot also carries the 64-byte header"
    );
    assert!(matches!(
        c.publish(300, &[0u8; 193]),
        Err(BusError::Payload)
    ));
}

#[test]
fn concurrent_emitters_never_lose_or_duplicate() {
    let host = FakeHost::new(Geometry {
        cap: 64,
        ..Geometry::default()
    });
    let c = Arc::new(attach(&host));
    let mut seen = std::collections::HashSet::new();
    let threads: Vec<_> = (0..4u8)
        .map(|t| {
            let c = Arc::clone(&c);
            std::thread::spawn(move || {
                for i in 0..200u8 {
                    loop {
                        match c.publish(300, &[t, i]) {
                            Ok(()) => break,
                            Err(BusError::WouldBlock) => std::thread::yield_now(),
                            Err(e) => panic!("{e}"),
                        }
                    }
                }
            })
        })
        .collect();
    let out = host.outbound();
    while seen.len() < 800 {
        if let Some((_, p)) = out.pop() {
            assert!(seen.insert(p.clone()), "duplicate {p:?}");
        } else if threads.iter().all(|t| t.is_finished()) && out.count() == 0 {
            break;
        }
    }
    for t in threads {
        t.join().unwrap();
    }
    while let Some((_, p)) = out.pop() {
        assert!(seen.insert(p.clone()), "duplicate {p:?}");
    }
    assert_eq!(seen.len(), 800, "every emit arrived exactly once");
}

// ---- poll ---------------------------------------------------------------

#[test]
fn poll_returns_inline_events_in_order() {
    let host = FakeHost::new(Geometry::default());
    let c = attach(&host);
    assert!(c.poll().unwrap().is_none());
    assert!(
        host.inbound()
            .push(inline(F_NOTIFICATION, 300, 0, 3), b"one")
    );
    assert!(host.inbound().push(inline(F_REPLY, 301, 5, 3), b"two"));
    let a = c.poll().unwrap().unwrap();
    let b = c.poll().unwrap().unwrap();
    assert_eq!(
        (a.frame.event_kind, a.payload.as_slice()),
        (300, &b"one"[..])
    );
    assert_eq!(
        (b.frame.correlation_id, b.payload.as_slice()),
        (5, &b"two"[..])
    );
    assert!(c.poll().unwrap().is_none());
}

#[test]
fn malformed_and_misplaced_frames_are_dropped_and_counted() {
    let host = FakeHost::new(Geometry::default());
    let c = attach(&host);
    let mut garbage = vec![0u8; 256];
    garbage[0] = 0xff;
    host.inbound().push_raw(&garbage);
    // A payload reference that runs past the slot.
    let mut raw = vec![0u8; 256];
    let past = Frame {
        payload_ref: 200,
        ..inline(F_NOTIFICATION, 300, 0, 100)
    };
    // Encode bypassing placement (encode only validates flags): it is the
    // receiver's placement check that must refuse it.
    past.encode(&mut raw).unwrap();
    host.inbound().push_raw(&raw);
    host.inbound()
        .push(inline(F_NOTIFICATION, 300, 0, 2), b"ok");
    let ev = c.poll().unwrap().expect("the good frame");
    assert_eq!(ev.payload, b"ok");
    assert_eq!(c.dropped_frames(), 2);
}

#[test]
fn a_host_restart_fails_closed() {
    let host = FakeHost::new(Geometry::default());
    let c = attach(&host);
    host.inbound().push(inline(F_NOTIFICATION, 300, 0, 1), b"x");
    host.bump_epoch();
    assert!(c.epoch_changed());
    assert!(
        matches!(c.poll(), Err(BusError::Epoch)),
        "a stale mapping is not read"
    );
    assert!(
        matches!(c.publish(300, b"x"), Err(BusError::Epoch)),
        "or written"
    );
}

#[test]
fn heartbeat_and_control_lost_are_shared_state() {
    let host = FakeHost::new(Geometry::default());
    let c = Arc::new(attach(&host));
    c.heartbeat(1234);
    assert_eq!(host.client_heartbeat(), 1234);
    host.set_control_lost();
    assert!(c.control_lost());
    let before = host.client_heartbeat();
    let guard = c.keep_alive(Duration::from_millis(5));
    std::thread::sleep(Duration::from_millis(50));
    assert_ne!(
        host.client_heartbeat(),
        before,
        "keep-alive advances the heartbeat"
    );
    drop(guard);
}

// ---- arena (D12) --------------------------------------------------------

fn arena_frame(lease: u32, generation: u32, len: u32) -> Frame {
    Frame {
        hdr_flags: F_NOTIFICATION | F_ARENA,
        wire_version: WIRE_VERSION,
        event_kind: 300,
        payload_ref: u64::from(lease),
        payload_len: len,
        generation,
        ..Frame::default()
    }
}

fn pop_release(host: &FakeHost) -> Option<(u32, u32)> {
    let (f, p) = host.outbound().pop()?;
    assert_eq!(f.event_kind, KIND_LEASE_RELEASE);
    assert_eq!(f.hdr_flags, F_NOTIFICATION | F_CONTROL | F_INLINE);
    assert_eq!(p.len(), LEASE_RELEASE_LEN);
    Some((
        u32::from_le_bytes(p[0..4].try_into().unwrap()),
        u32::from_le_bytes(p[4..8].try_into().unwrap()),
    ))
}

#[test]
fn an_arena_payload_is_copied_out_and_its_ref_returned() {
    let host = FakeHost::new(Geometry::default());
    let c = attach(&host);
    let bytes: Vec<u8> = (0..5000u32).map(|i| (i * 7 + 3) as u8).collect();
    host.write_arena(4096, &bytes);
    host.set_lease(17, 2, 5120, 4096);
    host.inbound().push(arena_frame(17, 2, 5000), b"");
    let ev = c.poll().unwrap().expect("arena event");
    assert_eq!(ev.payload, bytes, "the payload is the arena span's bytes");
    assert_eq!(
        pop_release(&host),
        Some((17, 2)),
        "the ref went back at once"
    );
    assert_eq!(c.pending_releases(), 0);
    assert!(pop_release(&host).is_none(), "exactly once");
}

#[test]
fn an_unresolvable_arena_frame_still_returns_its_ref() {
    let host = FakeHost::new(Geometry::default());
    let c = attach(&host);
    let cases = [
        ("stale generation", (3, 2, 64u32, 0u64), (3, 1, 32u32)),
        ("longer than the span", (4, 1, 64, 0), (4, 1, 65)),
        ("past the arena", (5, 1, 64, 64 * 1024 - 16), (5, 1, 32)),
        ("retired entry", (6, 0, 64, 0), (6, 1, 32)),
        ("id past the table", (7, 1, 64, 0), (5000, 1, 32)),
    ];
    for (name, (lease, lgen, span, off), (fl, fg, flen)) in cases {
        host.set_lease(lease, lgen, span, off);
        host.inbound().push(arena_frame(fl, fg, flen), b"");
        let ev = c.poll().unwrap().expect(name);
        assert!(ev.payload.is_empty(), "{name}: nothing readable");
        assert_eq!(
            pop_release(&host),
            Some((fl, fg)),
            "{name}: the ref is still owed"
        );
    }
}

#[test]
fn an_older_host_gets_no_arena_reads_and_no_releases() {
    let host = FakeHost::new(Geometry {
        with_view: false,
        ..Geometry::default()
    });
    let c = attach(&host);
    host.inbound().push(arena_frame(1, 1, 32), b"");
    let ev = c.poll().unwrap().expect("event");
    assert!(ev.payload.is_empty());
    assert_eq!(
        host.outbound().count(),
        0,
        "a host that cannot consume releases is sent none"
    );
}

#[test]
fn the_release_backlog_applies_backpressure_and_drains() {
    let host = FakeHost::new(Geometry {
        cap: 2,
        ..Geometry::default()
    });
    let c = attach(&host);
    // Fill the outbound ring so releases cannot leave.
    c.publish(300, b"a").unwrap();
    c.publish(300, b"b").unwrap();
    host.set_lease(1, 1, 64, 0);
    // Take MAX_PENDING_RELEASES arena events; each owes a ref that cannot be sent.
    for _ in 0..aimee_bus::client::MAX_PENDING_RELEASES {
        while !host.inbound().push(arena_frame(1, 1, 32), b"") {
            assert!(c.poll().unwrap().is_some());
        }
        let _ = c.poll().unwrap();
    }
    while c.poll().unwrap().is_some() {}
    assert_eq!(
        c.pending_releases(),
        aimee_bus::client::MAX_PENDING_RELEASES
    );
    assert!(host.inbound().push(arena_frame(1, 1, 32), b""));
    assert!(
        c.poll().unwrap().is_none(),
        "a full backlog leaves the arena event unread"
    );
    assert_eq!(host.inbound().count(), 1, "it stays on the ring");
    assert!(matches!(c.flush_releases(), Err(BusError::WouldBlock)));

    // Drain the outbound ring as the host would; the backlog flushes.
    let mut released = 0;
    loop {
        while host.outbound().pop().is_some() {
            released += 1;
        }
        match c.flush_releases() {
            Ok(()) => break,
            Err(BusError::WouldBlock) => continue,
            Err(e) => panic!("{e}"),
        }
    }
    while host.outbound().pop().is_some() {
        released += 1;
    }
    assert_eq!(c.pending_releases(), 0);
    assert!(released >= aimee_bus::client::MAX_PENDING_RELEASES);
    assert!(
        c.poll().unwrap().is_some(),
        "the waiting arena event is taken now"
    );
}

/// Arena placement is not only for notifications: the host routes arena
/// requests to a kind's server and arena replies back (route_arena_request,
/// route_arena_reply). A module serving one reads the payload the same way
/// and owes the same ref.
#[test]
fn arena_requests_and_replies_resolve_and_release_like_notifications() {
    let host = FakeHost::new(Geometry::default());
    let c = attach(&host);
    for (i, pattern) in [F_REQUEST, F_REPLY].into_iter().enumerate() {
        let lease = 30 + i as u32;
        let bytes = vec![0x40 + i as u8; 700];
        host.write_arena(8192 * (i + 1), &bytes);
        host.set_lease(lease, 5, 1024, 8192 * (i as u64 + 1));
        let mut f = arena_frame(lease, 5, 700);
        f.hdr_flags = pattern | F_ARENA;
        f.correlation_id = 900 + i as u64;
        host.inbound().push(f, b"");
        let ev = c.poll().unwrap().expect("arena event");
        assert_eq!(ev.frame.correlation_id, 900 + i as u64);
        assert_eq!(ev.frame.hdr_flags & (F_REQUEST | F_REPLY), pattern);
        assert_eq!(ev.payload, bytes);
        assert_eq!(pop_release(&host), Some((lease, 5)));
    }
}
