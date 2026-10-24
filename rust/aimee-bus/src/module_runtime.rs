//! A module process: attach to a daemon's local bus and serve stages.
//!
//! The same contract the C (`module_runtime.c`) and Go
//! (`server-go/bus/module_runtime.go`) runtimes implement: authenticated
//! attach, AMOD request reassembly and reply fragmentation, absolute
//! deadlines, cancellation, at most 16 concurrent handlers, heartbeats, and a
//! clean exit when the host epoch changes.

use crate::client::{Client, Event};
use crate::error::BusError;
use crate::module_protocol::*;
use crate::sys::monotonic_ns;
use crate::wire::{F_CANCEL, F_MORE, F_REQUEST};
use std::collections::HashMap;
use std::fmt;
use std::io::Write;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::time::Duration;

/// Concurrent handlers, and concurrent request assemblies, per process.
pub const MODULE_MAX_IN_FLIGHT: usize = 16;
const IDLE_MIN: Duration = Duration::from_millis(1);
const IDLE_MAX: Duration = Duration::from_millis(10);

/// Binds one event kind to the stage id carried in its AMOD envelope.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ModuleStage {
    pub event_kind: u32,
    pub stage_id: u32,
}

/// Transport-owned state a handler can see.
#[derive(Debug, Clone)]
pub struct Invocation {
    pub stage_id: u32,
    /// Absolute `CLOCK_MONOTONIC` nanoseconds; zero means none.
    pub deadline_ns: u64,
    pub trace_id: u64,
    pub principal_class: u32,
    /// The requesting principal.
    pub principal_ref: u32,
    cancelled: Arc<AtomicBool>,
}

impl Invocation {
    /// A bus cancellation, or an expired deadline. Handlers should check it
    /// around bounded units of work.
    pub fn cancelled(&self) -> bool {
        if self.cancelled.load(Ordering::Acquire) {
            return true;
        }
        let now = monotonic_ns();
        self.deadline_ns != 0 && now != 0 && now >= self.deadline_ns
    }

    /// Clamp a stage-owned limit to the transport deadline.
    pub fn remaining(&self, limit: Duration) -> Duration {
        let now = monotonic_ns();
        if self.deadline_ns == 0 || now == 0 {
            return limit;
        }
        limit.min(Duration::from_nanos(self.deadline_ns.saturating_sub(now)))
    }
}

/// Implements one or more stages. A non-OK status never carries a body; any
/// body returned with one is logged as the failure detail and dropped.
pub type Handler = Arc<dyn Fn(&Invocation, &[u8]) -> (Vec<u8>, ModuleStatus) + Send + Sync>;

/// Runs once after attach, before serving (for example to drop privileges
/// that would otherwise stop the host authenticating the executable).
pub type AfterAttach = Box<dyn FnOnce() -> Result<(), String> + Send>;

/// Process identity and stage table, as the module inventory materialises it.
pub struct ModuleProcessConfig {
    pub socket_path: PathBuf,
    pub module_name: String,
    pub principal_class: u32,
    pub principal_ref: u32,
    pub stages: Vec<ModuleStage>,
    /// `None` answers every valid request with `CapabilityAbsent`.
    pub handler: Option<Handler>,
    /// Idle poll ceiling, 1–10 ms. Zero keeps the 10 ms default.
    pub max_idle_poll: Duration,
    pub after_attach: Option<AfterAttach>,
}

impl fmt::Debug for ModuleProcessConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ModuleProcessConfig")
            .field("socket_path", &self.socket_path)
            .field("module_name", &self.module_name)
            .field("principal_class", &self.principal_class)
            .field("principal_ref", &self.principal_ref)
            .field("stages", &self.stages)
            .field("handler", &self.handler.is_some())
            .field("max_idle_poll", &self.max_idle_poll)
            .finish()
    }
}

/// A cloneable stop signal, safe to set from a signal handler's thread.
#[derive(Debug, Clone, Default)]
pub struct Shutdown(Arc<AtomicBool>);

impl Shutdown {
    pub fn new() -> Shutdown {
        Shutdown::default()
    }
    pub fn trigger(&self) {
        self.0.store(true, Ordering::Release);
    }
    pub fn is_set(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }
    pub(crate) fn flag(&self) -> &AtomicBool {
        &self.0
    }
}

/// The narrow bus surface the runtime needs, so the protocol logic can be
/// tested without mappings. [`Client`] is the real implementation.
pub trait ModuleBus {
    fn poll(&self) -> Result<Option<Event>, BusError>;
    fn reply_fragment(
        &self,
        kind: u32,
        correlation: u64,
        payload: &[u8],
        more: bool,
    ) -> Result<(), BusError>;
    fn heartbeat(&self, now: u64);
    fn epoch_changed(&self) -> bool;
    /// Largest payload one inline reply fragment may carry.
    fn max_inline_payload(&self) -> u32;
}

impl ModuleBus for Client {
    fn poll(&self) -> Result<Option<Event>, BusError> {
        Client::poll(self)
    }
    fn reply_fragment(
        &self,
        kind: u32,
        correlation: u64,
        payload: &[u8],
        more: bool,
    ) -> Result<(), BusError> {
        Client::reply_fragment(self, kind, correlation, payload, more)
    }
    fn heartbeat(&self, now: u64) {
        Client::heartbeat(self, now)
    }
    fn epoch_changed(&self) -> bool {
        Client::epoch_changed(self)
    }
    fn max_inline_payload(&self) -> u32 {
        Client::max_inline_payload(self)
    }
}

fn validate(config: &ModuleProcessConfig) -> Result<HashMap<u32, u32>, BusError> {
    if !config.max_idle_poll.is_zero() && !(IDLE_MIN..=IDLE_MAX).contains(&config.max_idle_poll) {
        return Err(BusError::Config(
            "max_idle_poll must be between 1ms and 10ms",
        ));
    }
    if config.socket_path.as_os_str().is_empty()
        || config.module_name.is_empty()
        || config.module_name.contains('\0')
        || config.principal_class == 0
        || config.principal_ref == 0
        || config.stages.is_empty()
    {
        return Err(BusError::Config(
            "socket, name, principal and at least one stage are required",
        ));
    }
    let mut stages = HashMap::new();
    for s in &config.stages {
        if s.event_kind == 0 || s.stage_id == 0 {
            return Err(BusError::Config("stage kinds and ids must be non-zero"));
        }
        if stages.insert(s.event_kind, s.stage_id).is_some() {
            return Err(BusError::Config("an event kind is bound to two stages"));
        }
    }
    Ok(stages)
}

/// Attach to the daemon's bus and serve until `shutdown` is set or the host
/// epoch changes. Both are clean exits.
pub fn run_module_process(
    mut config: ModuleProcessConfig,
    shutdown: &Shutdown,
) -> Result<(), BusError> {
    let stages = validate(&config)?;
    let Some(client) = Client::connect(
        &config.socket_path,
        config.principal_class,
        config.principal_ref,
        shutdown.flag(),
    )?
    else {
        return Ok(());
    };
    if let Some(hook) = config.after_attach.take() {
        hook().map_err(|e| {
            log(format_args!(
                "{} post-attach hardening failed: {e}",
                config.module_name
            ));
            BusError::Config("post-attach hardening failed")
        })?;
    }
    run_module_client(&config, &stages, &client, shutdown)
}

type CallKey = (u32, u64); // (requesting principal, correlation)

struct Assembly {
    event_kind: u32,
    message: ModuleMessage,
    body: Vec<u8>,
}

struct Work {
    event_kind: u32,
    correlation: u64,
    invocation: Invocation,
}

struct Done {
    key: CallKey,
    body: Vec<u8>,
    status: ModuleStatus,
}

enum Flow {
    Continue,
    Stop,
}

/// Serve over an already attached bus. Public for runtimes that attach
/// through their own path. Whatever ends the loop (shutdown, host restart,
/// or an error), admitted handlers are cancelled and drained, not abandoned.
pub fn run_module_client<B: ModuleBus>(
    config: &ModuleProcessConfig,
    stages: &HashMap<u32, u32>,
    bus: &B,
    shutdown: &Shutdown,
) -> Result<(), BusError> {
    let mut jobs: HashMap<CallKey, Work> = HashMap::new();
    let (tx, rx) = mpsc::channel::<Done>();
    let result = serve(config, stages, bus, shutdown, &mut jobs, &tx, &rx);
    for work in jobs.values() {
        work.invocation.cancelled.store(true, Ordering::Release);
    }
    while !jobs.is_empty() {
        // Every admitted job reports exactly once (see Report), and the
        // runtime still holds a sender, so this cannot miss a result.
        match rx.recv() {
            Ok(done) => {
                jobs.remove(&done.key);
            }
            Err(_) => break,
        }
    }
    result
}

#[allow(clippy::too_many_arguments)]
fn serve<B: ModuleBus>(
    config: &ModuleProcessConfig,
    stages: &HashMap<u32, u32>,
    bus: &B,
    shutdown: &Shutdown,
    jobs: &mut HashMap<CallKey, Work>,
    tx: &mpsc::Sender<Done>,
    rx: &mpsc::Receiver<Done>,
) -> Result<(), BusError> {
    let mut assemblies: HashMap<CallKey, Assembly> = HashMap::new();
    let idle_max = if config.max_idle_poll.is_zero() {
        IDLE_MAX
    } else {
        config.max_idle_poll
    };
    let mut idle = IDLE_MIN;

    let finish = |jobs: &mut HashMap<CallKey, Work>, done: Done| -> Result<Flow, BusError> {
        let Some(work) = jobs.remove(&done.key) else {
            return Ok(Flow::Continue);
        };
        reply_result(
            bus,
            shutdown,
            work.event_kind,
            work.correlation,
            work.invocation.stage_id,
            work.invocation.trace_id,
            done.status,
            done.body,
        )
    };

    loop {
        while let Ok(done) = rx.try_recv() {
            if let Flow::Stop = finish(jobs, done)? {
                return Ok(());
            }
        }
        if shutdown.is_set() {
            return Ok(());
        }
        if bus.epoch_changed() {
            return Ok(());
        }
        let now = monotonic_ns();
        if now != 0 {
            bus.heartbeat(now);
        }

        let event = match bus.poll() {
            Ok(Some(ev)) => ev,
            Ok(None) => {
                // A completed handler is local work: reply now rather than
                // after the next idle interval.
                match rx.recv_timeout(idle) {
                    Ok(done) => {
                        if let Flow::Stop = finish(jobs, done)? {
                            return Ok(());
                        }
                        idle = IDLE_MIN;
                    }
                    Err(RecvTimeoutError::Timeout) => idle = (idle * 2).min(idle_max),
                    Err(RecvTimeoutError::Disconnected) => unreachable!("runtime holds a sender"),
                }
                continue;
            }
            Err(BusError::Epoch) => return Ok(()),
            Err(e) => return Err(e),
        };
        idle = IDLE_MIN;

        let f = event.frame;
        let key = (f.principal_ref, f.correlation_id);
        if f.hdr_flags & F_CANCEL != 0 {
            if let Some(work) = jobs.get(&key) {
                work.invocation.cancelled.store(true, Ordering::Release);
            }
            assemblies.remove(&key);
            continue;
        }
        if f.hdr_flags & F_REQUEST == 0 {
            continue;
        }

        let expected = stages.get(&f.event_kind).copied().unwrap_or(0);
        let reply = |stage: u32, trace: u64, status: ModuleStatus| {
            reply_result(
                bus,
                shutdown,
                f.event_kind,
                f.correlation_id,
                stage,
                trace,
                status,
                Vec::new(),
            )
        };
        let decoded = ModuleMessage::decode(&event.payload);
        let mut invalid = match decoded {
            Ok(m) => {
                expected == 0
                    || m.operation != Operation::Invoke
                    || m.stage_id != expected
                    || jobs.contains_key(&key)
            }
            Err(_) => true,
        };
        if let (false, Ok(m), Some(a)) = (invalid, decoded, assemblies.get(&key)) {
            invalid = a.event_kind != f.event_kind
                || a.message.stage_id != m.stage_id
                || a.message.deadline_ns != m.deadline_ns
                || a.message.trace_id != m.trace_id;
        }
        let message = match (invalid, decoded) {
            (false, Ok(m)) => m,
            _ => {
                assemblies.remove(&key);
                if let Flow::Stop = reply(expected.max(1), 0, ModuleStatus::InvalidRequest)? {
                    return Ok(());
                }
                continue;
            }
        };
        if !assemblies.contains_key(&key) {
            if assemblies.len() >= MODULE_MAX_IN_FLIGHT {
                if let Flow::Stop = reply(expected, message.trace_id, ModuleStatus::Internal)? {
                    return Ok(());
                }
                continue;
            }
            assemblies.insert(
                key,
                Assembly {
                    event_kind: f.event_kind,
                    message,
                    body: Vec::new(),
                },
            );
        }
        let assembly = assemblies.get_mut(&key).expect("inserted above");
        let part = message.body(&event.payload);
        if assembly.body.len() + part.len() > MODULE_MESSAGE_MAX_BODY as usize {
            assemblies.remove(&key);
            if let Flow::Stop = reply(expected, message.trace_id, ModuleStatus::Internal)? {
                return Ok(());
            }
            continue;
        }
        assembly.body.extend_from_slice(part);
        if f.hdr_flags & F_MORE != 0 {
            continue;
        }
        let assembly = assemblies.remove(&key).expect("present");

        let status = if message.deadline_expired(now) {
            Some(ModuleStatus::DeadlineExceeded)
        } else if config.handler.is_none() {
            Some(ModuleStatus::CapabilityAbsent)
        } else if jobs.len() >= MODULE_MAX_IN_FLIGHT {
            Some(ModuleStatus::Internal)
        } else {
            None
        };
        if let Some(status) = status {
            if let Flow::Stop = reply(expected, message.trace_id, status)? {
                return Ok(());
            }
            continue;
        }

        let invocation = Invocation {
            stage_id: expected,
            deadline_ns: message.deadline_ns,
            trace_id: message.trace_id,
            // The host stamps principal_ref from the admitted slot. Zero is
            // reserved for host-origin traffic: external module grants cannot
            // enroll principal zero. Request bodies never supply this class.
            principal_class: u32::from(f.principal_ref != 0),
            principal_ref: f.principal_ref,
            cancelled: Arc::new(AtomicBool::new(false)),
        };
        jobs.insert(
            key,
            Work {
                event_kind: f.event_kind,
                correlation: f.correlation_id,
                invocation: invocation.clone(),
            },
        );
        let handler = Arc::clone(config.handler.as_ref().expect("checked above"));
        let report = Report {
            tx: tx.clone(),
            key,
            sent: false,
        };
        let name = config.module_name.clone();
        let kind = f.event_kind;
        let spawned = std::thread::Builder::new()
            .name(format!("{name}-stage"))
            .spawn(move || {
                let body = assembly.body;
                let (out, status) = run_handler(&handler, &invocation, &body, &name, kind);
                report.send(out, status);
            });
        if spawned.is_err() {
            jobs.remove(&key);
            if let Flow::Stop = reply(expected, message.trace_id, ModuleStatus::Internal)? {
                return Ok(());
            }
        }
    }
}

/// Diagnostics go to stderr, best-effort. `eprintln!` panics when stderr is
/// gone (a closed supervisor pipe), which in a handler thread would lose the
/// reply; a failed write here is ignored instead.
fn log(args: fmt::Arguments<'_>) {
    let _ = writeln!(std::io::stderr(), "aimee-bus: {args}");
}

/// Sends a handler's result exactly once. If the handler thread unwinds
/// before reporting, the drop sends `Internal`, so the caller is answered
/// and the slot and shutdown drain are not held forever.
struct Report {
    tx: mpsc::Sender<Done>,
    key: CallKey,
    sent: bool,
}

impl Report {
    fn send(mut self, body: Vec<u8>, status: ModuleStatus) {
        self.sent = true;
        let _ = self.tx.send(Done {
            key: self.key,
            body,
            status,
        });
    }
}

impl Drop for Report {
    fn drop(&mut self) {
        if !self.sent {
            let _ = self.tx.send(Done {
                key: self.key,
                body: Vec::new(),
                status: ModuleStatus::Internal,
            });
        }
    }
}

fn run_handler(
    handler: &Handler,
    inv: &Invocation,
    body: &[u8],
    name: &str,
    kind: u32,
) -> (Vec<u8>, ModuleStatus) {
    let (mut out, mut status) = match catch_unwind(AssertUnwindSafe(|| handler(inv, body))) {
        Ok(result) => result,
        Err(_) => {
            log(format_args!(
                "{name} stage {} (kind {kind}) panicked",
                inv.stage_id
            ));
            (Vec::new(), ModuleStatus::Internal)
        }
    };
    if out.len() > MODULE_MESSAGE_MAX_BODY as usize {
        status = ModuleStatus::Internal;
        out.clear();
    } else if status != ModuleStatus::Ok {
        // A non-OK reply carries no body, so say what the handler said on the
        // way past: otherwise a stage that refuses every request looks the
        // same as one that is slow.
        let detail = if out.is_empty() {
            "no detail".to_string()
        } else {
            let s = String::from_utf8_lossy(&out[..out.len().min(300)]).into_owned();
            if out.len() > 300 { s + "..." } else { s }
        };
        log(format_args!(
            "{name} stage {} (kind {kind}) failed with {status:?}: {detail}",
            inv.stage_id
        ));
        out.clear();
    }
    if inv.cancelled.load(Ordering::Acquire) {
        return (Vec::new(), ModuleStatus::Cancelled);
    }
    let now = monotonic_ns();
    if inv.deadline_ns != 0 && now != 0 && now >= inv.deadline_ns {
        return (Vec::new(), ModuleStatus::DeadlineExceeded);
    }
    (out, status)
}

#[allow(clippy::too_many_arguments)]
fn reply_result<B: ModuleBus>(
    bus: &B,
    shutdown: &Shutdown,
    kind: u32,
    correlation: u64,
    stage_id: u32,
    trace_id: u64,
    mut status: ModuleStatus,
    mut body: Vec<u8>,
) -> Result<Flow, BusError> {
    if body.len() > MODULE_MESSAGE_MAX_BODY as usize {
        status = ModuleStatus::Internal;
    }
    if status != ModuleStatus::Ok {
        body.clear();
    }
    let budget = bus.max_inline_payload() as usize;
    if budget <= MODULE_MESSAGE_HEADER_LEN {
        return Err(BusError::Config(
            "inline budget cannot carry an AMOD header",
        ));
    }
    let chunk = budget - MODULE_MESSAGE_HEADER_LEN;
    let mut offset = 0;
    let mut first = true;
    while first || offset < body.len() {
        first = false;
        let part = (body.len() - offset).min(chunk);
        let more = offset + part < body.len();
        let mut payload = vec![0u8; MODULE_MESSAGE_HEADER_LEN + part];
        ModuleMessage {
            operation: Operation::Result,
            status,
            stage_id,
            flags: 0,
            body_len: part as u32,
            deadline_ns: 0,
            trace_id,
        }
        .encode(&mut payload)
        .map_err(|_| BusError::Protocol("AMOD result refused by its own encoder"))?;
        payload[MODULE_MESSAGE_HEADER_LEN..].copy_from_slice(&body[offset..offset + part]);
        loop {
            match bus.reply_fragment(kind, correlation, &payload, more) {
                Ok(()) => break,
                Err(BusError::WouldBlock) => {
                    if shutdown.is_set() {
                        return Ok(Flow::Stop);
                    }
                    std::thread::sleep(IDLE_MIN);
                    if bus.epoch_changed() {
                        return Ok(Flow::Stop);
                    }
                    let now = monotonic_ns();
                    if now != 0 {
                        bus.heartbeat(now);
                    }
                }
                Err(BusError::Epoch) => return Ok(Flow::Stop),
                Err(e) => return Err(e),
            }
        }
        offset += part;
    }
    Ok(Flow::Continue)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::{F_INLINE, F_REPLY, Frame, HDR_LEN, WIRE_VERSION};
    use std::collections::VecDeque;
    use std::sync::Mutex;
    use std::time::Instant;

    const KIND: u32 = 5889;
    const STAGE: u32 = 1;

    /// (kind, correlation, payload, more) of one emitted reply fragment.
    type Sent = (u32, u64, Vec<u8>, bool);

    #[derive(Default)]
    struct FakeBus {
        inbox: Mutex<VecDeque<Event>>,
        outbox: Mutex<Vec<Sent>>,
        block: Mutex<u32>,
    }

    impl ModuleBus for FakeBus {
        fn poll(&self) -> Result<Option<Event>, BusError> {
            Ok(self.inbox.lock().unwrap().pop_front())
        }
        fn reply_fragment(
            &self,
            kind: u32,
            corr: u64,
            payload: &[u8],
            more: bool,
        ) -> Result<(), BusError> {
            let mut b = self.block.lock().unwrap();
            if *b > 0 {
                *b -= 1;
                return Err(BusError::WouldBlock);
            }
            self.outbox
                .lock()
                .unwrap()
                .push((kind, corr, payload.to_vec(), more));
            Ok(())
        }
        fn heartbeat(&self, _: u64) {}
        fn epoch_changed(&self) -> bool {
            false
        }
        fn max_inline_payload(&self) -> u32 {
            100
        }
    }

    fn frame(flags: u16, kind: u32, corr: u64, len: usize) -> Frame {
        Frame {
            hdr_flags: flags | if len > 0 { F_INLINE } else { 0 },
            wire_version: WIRE_VERSION,
            event_kind: kind,
            principal_ref: 90,
            correlation_id: corr,
            payload_ref: if len > 0 { HDR_LEN as u64 } else { 0 },
            payload_len: len as u32,
            ..Frame::default()
        }
    }

    /// Queue an AMOD invoke, fragmented to the fake's 60-byte body chunks.
    fn push_invoke(bus: &FakeBus, kind: u32, stage: u32, corr: u64, deadline: u64, body: &[u8]) {
        let chunks: Vec<&[u8]> = if body.is_empty() {
            vec![&[][..]]
        } else {
            body.chunks(60).collect()
        };
        for (i, part) in chunks.iter().enumerate() {
            let mut payload = vec![0u8; MODULE_MESSAGE_HEADER_LEN + part.len()];
            ModuleMessage {
                operation: Operation::Invoke,
                status: ModuleStatus::Ok,
                stage_id: stage,
                flags: 0,
                body_len: part.len() as u32,
                deadline_ns: deadline,
                trace_id: 77,
            }
            .encode(&mut payload)
            .unwrap();
            payload[MODULE_MESSAGE_HEADER_LEN..].copy_from_slice(part);
            let more = if i + 1 < chunks.len() { F_MORE } else { 0 };
            let f = frame(F_REQUEST | more, kind, corr, payload.len());
            bus.inbox
                .lock()
                .unwrap()
                .push_back(Event { frame: f, payload });
        }
    }

    /// Reassemble the replies for one correlation: (status, body).
    fn result_for(bus: &FakeBus, corr: u64) -> Option<(ModuleStatus, Vec<u8>)> {
        let out = bus.outbox.lock().unwrap();
        let mut body = Vec::new();
        for (_, c, payload, more) in out.iter().filter(|r| r.1 == corr) {
            assert_eq!(*c, corr);
            let m = ModuleMessage::decode(payload).unwrap();
            assert_eq!(m.operation, Operation::Result);
            body.extend_from_slice(m.body(payload));
            if !more {
                return Some((m.status, body));
            }
        }
        None
    }

    fn serve(bus: Arc<FakeBus>, handler: Option<Handler>, until: &[u64]) {
        let config = ModuleProcessConfig {
            socket_path: "/unused".into(),
            module_name: "test".into(),
            principal_class: 1,
            principal_ref: 7,
            stages: vec![ModuleStage {
                event_kind: KIND,
                stage_id: STAGE,
            }],
            handler,
            max_idle_poll: Duration::ZERO,
            after_attach: None,
        };
        let stages = validate(&config).unwrap();
        let shutdown = Shutdown::new();
        let (b, s) = (Arc::clone(&bus), shutdown.clone());
        let t = std::thread::spawn(move || run_module_client(&config, &stages, &*b, &s));
        let deadline = Instant::now() + Duration::from_secs(10);
        while !until.iter().all(|c| result_for(&bus, *c).is_some()) {
            assert!(Instant::now() < deadline, "timed out waiting for replies");
            std::thread::sleep(Duration::from_millis(1));
        }
        shutdown.trigger();
        t.join().unwrap().unwrap();
    }

    #[test]
    fn invocation_preserves_attested_host_and_module_origins() {
        let bus = Arc::new(FakeBus::default());
        push_invoke(&bus, KIND, STAGE, 1, 0, &[b'x'; 181]);
        for event in bus.inbox.lock().unwrap().iter_mut() {
            event.frame.principal_ref = 0;
        }
        // A module body claiming host identity cannot replace frame metadata.
        push_invoke(
            &bus,
            KIND,
            STAGE,
            2,
            0,
            br#"{"principal_class":0,"principal_ref":0}"#,
        );
        let seen = Arc::new(Mutex::new(Vec::new()));
        let captured = Arc::clone(&seen);
        let handler: Handler = Arc::new(move |inv, _| {
            captured
                .lock()
                .unwrap()
                .push((inv.principal_class, inv.principal_ref));
            (Vec::new(), ModuleStatus::Ok)
        });
        serve(bus, Some(handler), &[1, 2]);
        let mut origins = seen.lock().unwrap().clone();
        origins.sort_unstable();
        assert_eq!(origins, vec![(0, 0), (1, 90)]);
    }

    fn echo() -> Handler {
        Arc::new(|inv: &Invocation, req: &[u8]| {
            if req == b"cancel" {
                while !inv.cancelled() {
                    std::thread::sleep(Duration::from_millis(1));
                }
            }
            if req == b"panic" {
                panic!("handler panic is contained");
            }
            if req == b"refuse" {
                return (b"reason".to_vec(), ModuleStatus::InvalidRequest);
            }
            (req.to_vec(), ModuleStatus::Ok)
        })
    }

    #[test]
    fn fragmented_echo_and_failures() {
        let bus = Arc::new(FakeBus::default());
        let big: Vec<u8> = (0..1000u32).map(|i| (i * 131 + 17) as u8).collect();
        push_invoke(&bus, KIND, STAGE, 1, 0, &big);
        push_invoke(&bus, KIND, STAGE, 2, 1, b"late");
        push_invoke(&bus, KIND, 9, 3, 0, b"wrong stage");
        push_invoke(&bus, 4242, STAGE, 4, 0, b"unknown kind");
        push_invoke(&bus, KIND, STAGE, 5, 0, b"panic");
        push_invoke(&bus, KIND, STAGE, 6, 0, b"refuse");
        push_invoke(&bus, KIND, STAGE, 7, 0, b"");
        *bus.block.lock().unwrap() = 3; // the first replies meet a full ring
        serve(Arc::clone(&bus), Some(echo()), &[1, 2, 3, 4, 5, 6, 7]);
        assert_eq!(result_for(&bus, 1), Some((ModuleStatus::Ok, big)));
        assert_eq!(
            result_for(&bus, 2),
            Some((ModuleStatus::DeadlineExceeded, vec![]))
        );
        assert_eq!(
            result_for(&bus, 3),
            Some((ModuleStatus::InvalidRequest, vec![]))
        );
        assert_eq!(
            result_for(&bus, 4),
            Some((ModuleStatus::InvalidRequest, vec![]))
        );
        assert_eq!(result_for(&bus, 5), Some((ModuleStatus::Internal, vec![])));
        assert_eq!(
            result_for(&bus, 6),
            Some((ModuleStatus::InvalidRequest, vec![]))
        );
        assert_eq!(result_for(&bus, 7), Some((ModuleStatus::Ok, vec![])));
        // Every fragment but the last carries F_MORE, and none exceeds the budget.
        let out = bus.outbox.lock().unwrap();
        let frags: Vec<_> = out.iter().filter(|r| r.1 == 1).collect();
        assert!(frags.len() > 1 && frags.iter().all(|r| r.2.len() <= 100));
        assert!(frags[..frags.len() - 1].iter().all(|r| r.3) && !frags.last().unwrap().3);
    }

    #[test]
    fn cancel_reaches_the_handler() {
        let bus = Arc::new(FakeBus::default());
        push_invoke(&bus, KIND, STAGE, 11, 0, b"cancel");
        let b2 = Arc::clone(&bus);
        let canceller = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(30));
            let f = frame(F_CANCEL, KIND, 11, 0);
            b2.inbox.lock().unwrap().push_back(Event {
                frame: f,
                payload: vec![],
            });
        });
        serve(Arc::clone(&bus), Some(echo()), &[11]);
        canceller.join().unwrap();
        assert_eq!(
            result_for(&bus, 11),
            Some((ModuleStatus::Cancelled, vec![]))
        );
    }

    #[test]
    fn no_handler_is_capability_absent() {
        let bus = Arc::new(FakeBus::default());
        push_invoke(&bus, KIND, STAGE, 21, 0, b"x");
        serve(Arc::clone(&bus), None, &[21]);
        assert_eq!(
            result_for(&bus, 21),
            Some((ModuleStatus::CapabilityAbsent, vec![]))
        );
    }

    #[test]
    fn replies_and_notifications_are_ignored() {
        let bus = Arc::new(FakeBus::default());
        let f = frame(F_REPLY, KIND, 31, 0);
        bus.inbox.lock().unwrap().push_back(Event {
            frame: f,
            payload: vec![],
        });
        push_invoke(&bus, KIND, STAGE, 32, 0, b"y");
        serve(Arc::clone(&bus), Some(echo()), &[32]);
        assert!(result_for(&bus, 31).is_none());
    }

    #[test]
    fn shutdown_cancels_and_drains_running_handlers() {
        let bus = Arc::new(FakeBus::default());
        push_invoke(&bus, KIND, STAGE, 41, 0, b"cancel");
        let config = ModuleProcessConfig {
            socket_path: "/unused".into(),
            module_name: "test".into(),
            principal_class: 1,
            principal_ref: 7,
            stages: vec![ModuleStage {
                event_kind: KIND,
                stage_id: STAGE,
            }],
            handler: Some(echo()),
            max_idle_poll: Duration::ZERO,
            after_attach: None,
        };
        let stages = validate(&config).unwrap();
        let shutdown = Shutdown::new();
        let (b, s) = (Arc::clone(&bus), shutdown.clone());
        let t = std::thread::spawn(move || run_module_client(&config, &stages, &*b, &s));
        // Let the handler start and block waiting for cancellation.
        let start = Instant::now();
        while !bus.inbox.lock().unwrap().is_empty() {
            assert!(start.elapsed() < Duration::from_secs(5));
            std::thread::sleep(Duration::from_millis(1));
        }
        std::thread::sleep(Duration::from_millis(20));
        shutdown.trigger();
        t.join().unwrap().unwrap();
        // Drained without replying: the process is going away.
        assert!(result_for(&bus, 41).is_none());
    }

    #[test]
    fn config_is_validated() {
        let base = || ModuleProcessConfig {
            socket_path: "/s".into(),
            module_name: "m".into(),
            principal_class: 1,
            principal_ref: 7,
            stages: vec![ModuleStage {
                event_kind: KIND,
                stage_id: STAGE,
            }],
            handler: None,
            max_idle_poll: Duration::ZERO,
            after_attach: None,
        };
        assert!(validate(&base()).is_ok());
        let nul = ModuleProcessConfig {
            module_name: "bad\0name".into(),
            ..base()
        };
        assert!(validate(&nul).is_err());
        assert!(
            validate(&ModuleProcessConfig {
                principal_ref: 0,
                ..base()
            })
            .is_err()
        );
        assert!(
            validate(&ModuleProcessConfig {
                stages: vec![],
                ..base()
            })
            .is_err()
        );
        assert!(
            validate(&ModuleProcessConfig {
                max_idle_poll: Duration::from_millis(11),
                ..base()
            })
            .is_err()
        );
        let dup = vec![
            ModuleStage {
                event_kind: KIND,
                stage_id: 1,
            },
            ModuleStage {
                event_kind: KIND,
                stage_id: 2,
            },
        ];
        assert!(
            validate(&ModuleProcessConfig {
                stages: dup,
                ..base()
            })
            .is_err()
        );
    }

    /// A co-located producer can place a request in the arena; the client
    /// hands the runtime the resolved payload (D12) with F_ARENA set and no
    /// F_INLINE. The runtime must serve it exactly as an inline request.
    #[test]
    fn an_arena_placed_request_is_served_like_an_inline_one() {
        let bus = Arc::new(FakeBus::default());
        let body = b"from the arena".to_vec();
        let mut payload = vec![0u8; MODULE_MESSAGE_HEADER_LEN + body.len()];
        ModuleMessage {
            operation: Operation::Invoke,
            status: ModuleStatus::Ok,
            stage_id: STAGE,
            flags: 0,
            body_len: body.len() as u32,
            deadline_ns: 0,
            trace_id: 77,
        }
        .encode(&mut payload)
        .unwrap();
        payload[MODULE_MESSAGE_HEADER_LEN..].copy_from_slice(&body);
        let f = Frame {
            hdr_flags: F_REQUEST | crate::wire::F_ARENA,
            wire_version: WIRE_VERSION,
            event_kind: KIND,
            principal_ref: 90,
            correlation_id: 41,
            payload_ref: 3,
            payload_len: payload.len() as u32,
            generation: 2,
            ..Frame::default()
        };
        bus.inbox
            .lock()
            .unwrap()
            .push_back(Event { frame: f, payload });
        serve(Arc::clone(&bus), Some(echo()), &[41]);
        assert_eq!(result_for(&bus, 41), Some((ModuleStatus::Ok, body)));
    }
}
