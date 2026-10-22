//! Calling a module stage: the requesting half of the module contract.
//!
//! The serving half is [`crate::module_runtime`]. This is the Rust
//! counterpart of the C core caller (`src/core/event_bus/module_client.c`)
//! and the Go `ModuleCaller` (`server-go/bus/module_caller.go`), and it keeps
//! the C caller's outcomes and checks, which are the stricter of the two:
//!
//! - the request is an AMOD `Invoke`, fragmented across the inline budget
//!   with `F_MORE` on every fragment but the last;
//! - the reply is reassembled from `Result` fragments, each of which must be
//!   an `F_REPLY` of the requested kind carrying the requested stage and
//!   trace ids, and bounded by both the AMOD body limit and the caller's own
//!   response limit (a response over the caller's limit is still drained to
//!   its last fragment, so the next call starts clean, and reported with its
//!   full length);
//! - a non-OK status is terminal and carries no body;
//! - the host's `CAPABILITY_ABSENT` (no server, or no room to track the
//!   request) and `CAPABILITY_DENIED` (the caller's grant does not cover the
//!   kind) end the call at once rather than waiting out a deadline;
//! - an absolute `CLOCK_MONOTONIC` deadline, and a caller-supplied
//!   cancellation check, both publish `F_CANCEL` for the correlation before
//!   returning, so a module still working learns to stop;
//! - a frame for any other correlation is a stale reply to an earlier call
//!   that already returned, and is drained.
//!
//! One caller has one reply stream, so its calls are serialised: share a
//! `ModuleCaller` across threads and they take turns. Give a caller that must
//! not wait behind another call its own client.

use crate::client::{Client, Event};
use crate::error::BusError;
use crate::module_protocol::*;
use crate::sys::monotonic_ns;
use crate::wire::{
    F_MORE, F_REPLY, KIND_CAPABILITY_ABSENT, KIND_CAPABILITY_DENIED, KIND_MODULE_BASE,
};
use std::fmt;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

const IDLE_MIN: Duration = Duration::from_micros(50);
const IDLE_MAX: Duration = Duration::from_millis(1);
/// How long a late cancel keeps retrying a full outbound ring: the C caller's
/// bound (100 attempts, 1 ms apart).
const CANCEL_ATTEMPTS: u32 = 100;
const CANCEL_RETRY: Duration = Duration::from_millis(1);

/// The narrow bus surface a caller needs, so the protocol logic is testable
/// without mappings. [`Client`] is the real implementation.
pub trait CallerBus {
    fn poll(&self) -> Result<Option<Event>, BusError>;
    fn request_fragment(
        &self,
        kind: u32,
        correlation: u64,
        payload: &[u8],
        more: bool,
    ) -> Result<(), BusError>;
    fn cancel(&self, kind: u32, correlation: u64) -> Result<(), BusError>;
    fn heartbeat(&self, now: u64);
    /// Largest payload one inline request fragment may carry.
    fn max_inline_payload(&self) -> u32;
}

impl CallerBus for Client {
    fn poll(&self) -> Result<Option<Event>, BusError> {
        Client::poll(self)
    }
    fn request_fragment(
        &self,
        kind: u32,
        correlation: u64,
        payload: &[u8],
        more: bool,
    ) -> Result<(), BusError> {
        Client::request_fragment(self, kind, correlation, payload, more)
    }
    fn cancel(&self, kind: u32, correlation: u64) -> Result<(), BusError> {
        Client::cancel(self, kind, correlation)
    }
    fn heartbeat(&self, now: u64) {
        Client::heartbeat(self, now)
    }
    fn max_inline_payload(&self) -> u32 {
        Client::max_inline_payload(self)
    }
}

impl<T: CallerBus + ?Sized> CallerBus for &T {
    fn poll(&self) -> Result<Option<Event>, BusError> {
        (**self).poll()
    }
    fn request_fragment(&self, k: u32, c: u64, p: &[u8], more: bool) -> Result<(), BusError> {
        (**self).request_fragment(k, c, p, more)
    }
    fn cancel(&self, k: u32, c: u64) -> Result<(), BusError> {
        (**self).cancel(k, c)
    }
    fn heartbeat(&self, now: u64) {
        (**self).heartbeat(now)
    }
    fn max_inline_payload(&self) -> u32 {
        (**self).max_inline_payload()
    }
}

impl<T: CallerBus + ?Sized> CallerBus for Arc<T> {
    fn poll(&self) -> Result<Option<Event>, BusError> {
        (**self).poll()
    }
    fn request_fragment(&self, k: u32, c: u64, p: &[u8], more: bool) -> Result<(), BusError> {
        (**self).request_fragment(k, c, p, more)
    }
    fn cancel(&self, k: u32, c: u64) -> Result<(), BusError> {
        (**self).cancel(k, c)
    }
    fn heartbeat(&self, now: u64) {
        (**self).heartbeat(now)
    }
    fn max_inline_payload(&self) -> u32 {
        (**self).max_inline_payload()
    }
}

/// Why a call did not return a response body. The variants are the C
/// caller's `aimee_module_call_result_t`, less `OK`; [`CallError::name`]
/// spells them as `aimee_module_call_result_name` does.
#[derive(Debug)]
pub enum CallError {
    /// No module serves the kind (the host's control frame), or the module
    /// answered `CapabilityAbsent`.
    CapabilityAbsent,
    /// The host refused the request: this caller's grant does not cover the
    /// kind.
    CapabilityDenied,
    /// The caller's cancellation check fired, or the module answered
    /// `Cancelled`.
    Cancelled,
    /// The deadline passed, or the module answered `DeadlineExceeded`.
    DeadlineExceeded,
    /// The module answered `InvalidRequest`.
    InvalidRequest,
    /// The module answered `Internal`.
    Internal,
    /// The complete response was longer than the call's response limit. It
    /// was drained, not kept; `len` is its full length.
    ResponseTooLarge { len: u32 },
    /// The bus failed under the call (a refused send, a host restart).
    Transport(BusError),
    /// The reply broke the AMOD contract.
    Protocol(&'static str),
    /// The call was refused locally before anything was sent.
    InvalidArgument(&'static str),
}

impl CallError {
    /// The contract name, as `aimee_module_call_result_name` spells it.
    pub fn name(&self) -> &'static str {
        match self {
            CallError::CapabilityAbsent => "CAPABILITY_ABSENT",
            CallError::CapabilityDenied => "CAPABILITY_DENIED",
            CallError::Cancelled => "CANCELLED",
            CallError::DeadlineExceeded => "DEADLINE_EXCEEDED",
            CallError::InvalidRequest => "INVALID_REQUEST",
            CallError::Internal => "INTERNAL",
            CallError::ResponseTooLarge { .. } => "RESPONSE_TOO_LARGE",
            CallError::Transport(_) => "TRANSPORT",
            CallError::Protocol(_) => "PROTOCOL",
            CallError::InvalidArgument(_) => "INVALID_ARGUMENT",
        }
    }

    /// The outcome a non-OK module status maps to (the C `status_result`).
    fn from_status(status: ModuleStatus) -> Option<CallError> {
        Some(match status {
            ModuleStatus::Ok => return None,
            ModuleStatus::CapabilityAbsent => CallError::CapabilityAbsent,
            ModuleStatus::Cancelled => CallError::Cancelled,
            ModuleStatus::DeadlineExceeded => CallError::DeadlineExceeded,
            ModuleStatus::InvalidRequest => CallError::InvalidRequest,
            ModuleStatus::Internal => CallError::Internal,
        })
    }
}

impl fmt::Display for CallError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CallError::ResponseTooLarge { len } => {
                write!(f, "module call: RESPONSE_TOO_LARGE ({len} bytes)")
            }
            CallError::Transport(e) => write!(f, "module call: TRANSPORT: {e}"),
            CallError::Protocol(m) => write!(f, "module call: PROTOCOL: {m}"),
            CallError::InvalidArgument(m) => write!(f, "module call: INVALID_ARGUMENT: {m}"),
            other => write!(f, "module call: {}", other.name()),
        }
    }
}

impl std::error::Error for CallError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            CallError::Transport(e) => Some(e),
            _ => None,
        }
    }
}

/// One stage call.
#[derive(Debug, Clone, Copy)]
pub struct Call<'a> {
    pub event_kind: u32,
    pub stage_id: u32,
    pub trace_id: u64,
    /// Absolute `CLOCK_MONOTONIC` nanoseconds; zero means none.
    pub deadline_ns: u64,
    pub request: &'a [u8],
    /// Longest response this call will keep. A longer one is drained and
    /// reported as [`CallError::ResponseTooLarge`].
    pub response_limit: u32,
}

impl<'a> Call<'a> {
    /// A call with no deadline, trace id zero, and the AMOD body limit as its
    /// response limit.
    pub fn new(event_kind: u32, stage_id: u32, request: &'a [u8]) -> Call<'a> {
        Call {
            event_kind,
            stage_id,
            trace_id: 0,
            deadline_ns: 0,
            request,
            response_limit: MODULE_MESSAGE_MAX_BODY,
        }
    }

    pub fn trace_id(mut self, trace_id: u64) -> Call<'a> {
        self.trace_id = trace_id;
        self
    }

    /// An absolute `CLOCK_MONOTONIC` deadline in nanoseconds.
    pub fn deadline_ns(mut self, deadline_ns: u64) -> Call<'a> {
        self.deadline_ns = deadline_ns;
        self
    }

    /// A deadline `timeout` from now.
    pub fn timeout(mut self, timeout: Duration) -> Call<'a> {
        let ns = u64::try_from(timeout.as_nanos()).unwrap_or(u64::MAX);
        self.deadline_ns = monotonic_ns().saturating_add(ns).max(1);
        self
    }

    pub fn response_limit(mut self, limit: u32) -> Call<'a> {
        self.response_limit = limit;
        self
    }
}

/// Issues stage calls over an attached client (see the module docs).
#[derive(Debug)]
pub struct ModuleCaller<B: CallerBus> {
    bus: B,
    next: AtomicU64,
    /// One reply stream: calls take turns.
    lock: Mutex<()>,
}

impl<B: CallerBus> ModuleCaller<B> {
    /// Borrow (`&Client`) or share (`Arc<Client>`) an attached client. The
    /// client should be dedicated to calls: anything else it receives is
    /// drained as stale while a call waits.
    pub fn new(bus: B) -> ModuleCaller<B> {
        ModuleCaller {
            bus,
            next: AtomicU64::new(1),
            lock: Mutex::new(()),
        }
    }

    /// The bus this caller sends on.
    pub fn bus(&self) -> &B {
        &self.bus
    }

    /// Invoke one stage and return its response body.
    pub fn call(&self, call: &Call<'_>) -> Result<Vec<u8>, CallError> {
        self.call_cancellable(call, &|| false)
    }

    /// As [`ModuleCaller::call`], checking `cancelled` throughout. When it
    /// returns true the call publishes a cancel and returns
    /// [`CallError::Cancelled`].
    pub fn call_cancellable(
        &self,
        call: &Call<'_>,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<u8>, CallError> {
        if call.event_kind < KIND_MODULE_BASE {
            return Err(CallError::InvalidArgument("event kind is reserved"));
        }
        if call.stage_id == 0 {
            return Err(CallError::InvalidArgument("stage id must be non-zero"));
        }
        if call.request.len() > MODULE_MESSAGE_MAX_BODY as usize {
            return Err(CallError::InvalidArgument(
                "request exceeds the AMOD body limit",
            ));
        }
        if cancelled() {
            return Err(CallError::Cancelled);
        }
        if expired(call.deadline_ns) {
            return Err(CallError::DeadlineExceeded);
        }
        let _turn = self.lock.lock().unwrap_or_else(|p| p.into_inner());
        let budget = self.bus.max_inline_payload() as usize;
        if budget <= MODULE_MESSAGE_HEADER_LEN {
            return Err(CallError::Transport(BusError::Payload));
        }
        let correlation = self.next_correlation();
        let outcome = self
            .send(call, correlation, budget, cancelled)
            .and_then(|()| self.await_reply(call, correlation, cancelled));
        outcome.map_err(|ended| {
            // The caller is leaving while the module may still be working on
            // a request the host has seen: tell it to stop. Best effort; the
            // reason the call ended is what returns.
            if ended.open {
                self.cancel_request(call.event_kind, correlation);
            }
            ended.error
        })
    }

    fn next_correlation(&self) -> u64 {
        loop {
            let id = self.next.fetch_add(1, Ordering::Relaxed);
            if id != 0 {
                return id;
            }
        }
    }

    fn send(
        &self,
        call: &Call<'_>,
        correlation: u64,
        budget: usize,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<(), Ended> {
        let chunk = budget - MODULE_MESSAGE_HEADER_LEN;
        let mut frame = vec![0u8; budget];
        let mut offset = 0;
        let mut first = true;
        while first || offset < call.request.len() {
            first = false;
            let part = (call.request.len() - offset).min(chunk);
            let more = offset + part < call.request.len();
            ModuleMessage {
                operation: Operation::Invoke,
                status: ModuleStatus::Ok,
                stage_id: call.stage_id,
                flags: 0,
                body_len: part as u32,
                deadline_ns: call.deadline_ns,
                trace_id: call.trace_id,
            }
            .encode(&mut frame)
            .map_err(|_| Ended::closed(CallError::InvalidArgument("request envelope")))?;
            frame[MODULE_MESSAGE_HEADER_LEN..MODULE_MESSAGE_HEADER_LEN + part]
                .copy_from_slice(&call.request[offset..offset + part]);
            let payload = &frame[..MODULE_MESSAGE_HEADER_LEN + part];
            // Anything already sent leaves a partial request at the host.
            let open = offset > 0;
            let mut idle = IDLE_MIN;
            loop {
                if cancelled() {
                    return Err(Ended::new(CallError::Cancelled, open));
                }
                if expired(call.deadline_ns) {
                    return Err(Ended::new(CallError::DeadlineExceeded, open));
                }
                match self
                    .bus
                    .request_fragment(call.event_kind, correlation, payload, more)
                {
                    Ok(()) => break,
                    Err(BusError::WouldBlock) => {
                        std::thread::sleep(idle);
                        idle = (idle * 2).min(IDLE_MAX);
                    }
                    Err(e) => return Err(Ended::new(CallError::Transport(e), open)),
                }
            }
            offset += part;
        }
        Ok(())
    }

    fn await_reply(
        &self,
        call: &Call<'_>,
        correlation: u64,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Vec<u8>, Ended> {
        let mut body: Vec<u8> = Vec::new();
        let mut total: u32 = 0;
        let mut too_large = false;
        let mut idle = IDLE_MIN;
        loop {
            if cancelled() {
                return Err(Ended::new(CallError::Cancelled, true));
            }
            if expired(call.deadline_ns) {
                return Err(Ended::new(CallError::DeadlineExceeded, true));
            }
            let now = monotonic_ns();
            if now != 0 {
                self.bus.heartbeat(now);
            }
            let event = match self.bus.poll() {
                Ok(Some(event)) => event,
                Ok(None) => {
                    std::thread::sleep(idle);
                    idle = (idle * 2).min(IDLE_MAX);
                    continue;
                }
                // The inbound ring is unreadable (the host restarted): a cancel
                // would go nowhere.
                Err(e) => return Err(Ended::closed(CallError::Transport(e))),
            };
            idle = IDLE_MIN;
            let f = &event.frame;
            // A stale reply to an earlier call that already returned, or
            // anything else this dedicated client was sent: drain it.
            if f.correlation_id != correlation {
                continue;
            }
            // The host answers on its reserved kinds, so match them before
            // the kind check below.
            match f.event_kind {
                KIND_CAPABILITY_ABSENT => return Err(Ended::closed(CallError::CapabilityAbsent)),
                KIND_CAPABILITY_DENIED => return Err(Ended::closed(CallError::CapabilityDenied)),
                _ => {}
            }
            if f.hdr_flags & F_REPLY == 0 || f.event_kind != call.event_kind {
                return Err(Ended::closed(CallError::Protocol(
                    "correlated frame is not a reply of the requested kind",
                )));
            }
            let reply = ModuleMessage::decode(&event.payload).map_err(|_| {
                Ended::closed(CallError::Protocol("reply envelope does not decode"))
            })?;
            if reply.operation != Operation::Result
                || reply.stage_id != call.stage_id
                || reply.trace_id != call.trace_id
            {
                return Err(Ended::closed(CallError::Protocol(
                    "reply is not this call's result (operation, stage or trace)",
                )));
            }
            let more = f.hdr_flags & F_MORE != 0;
            if let Some(err) = CallError::from_status(reply.status) {
                if more || reply.body_len != 0 || total != 0 {
                    return Err(Ended::closed(CallError::Protocol(
                        "a non-OK result must be one empty fragment",
                    )));
                }
                // The module's own terminal outcome: nothing left to cancel.
                return Err(Ended::closed(err));
            }
            total = total
                .checked_add(reply.body_len)
                .filter(|t| *t <= MODULE_MESSAGE_MAX_BODY)
                .ok_or(Ended::closed(CallError::Protocol(
                    "reassembled response exceeds the AMOD body limit",
                )))?;
            if total > call.response_limit {
                // Keep draining to the last fragment so the next call starts
                // clean, but stop keeping bytes nobody will receive.
                too_large = true;
                body = Vec::new();
            } else if !too_large {
                body.extend_from_slice(reply.body(&event.payload));
            }
            if more {
                continue;
            }
            return if too_large {
                Err(Ended::closed(CallError::ResponseTooLarge { len: total }))
            } else {
                Ok(body)
            };
        }
    }

    fn cancel_request(&self, kind: u32, correlation: u64) {
        for _ in 0..CANCEL_ATTEMPTS {
            match self.bus.cancel(kind, correlation) {
                Err(BusError::WouldBlock) => std::thread::sleep(CANCEL_RETRY),
                _ => return,
            }
        }
    }
}

/// How a call ended without a body, and whether its correlation is still open
/// at the module (so leaving must cancel it).
struct Ended {
    error: CallError,
    open: bool,
}

impl Ended {
    fn new(error: CallError, open: bool) -> Ended {
        Ended { error, open }
    }
    fn closed(error: CallError) -> Ended {
        Ended { error, open: false }
    }
}

fn expired(deadline_ns: u64) -> bool {
    if deadline_ns == 0 {
        return false;
    }
    let now = monotonic_ns();
    now != 0 && now >= deadline_ns
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::{F_CONTROL, F_INLINE, F_NOTIFICATION, F_REQUEST, Frame};
    use std::collections::VecDeque;
    use std::sync::atomic::AtomicBool;

    const KIND: u32 = 5889;
    const STAGE: u32 = 1;
    const BUDGET: u32 = 100;

    /// What the caller sent, in order.
    #[derive(Debug, Clone, PartialEq, Eq)]
    enum Sent {
        Fragment {
            kind: u32,
            corr: u64,
            payload: Vec<u8>,
            more: bool,
        },
        Cancel {
            kind: u32,
            corr: u64,
        },
    }

    type Respond = Box<dyn Fn(&FakeBus, &Sent) + Send + Sync>;

    /// A scripted bus: `respond` sees each sent fragment and may queue events.
    struct FakeBus {
        budget: u32,
        sent: Mutex<Vec<Sent>>,
        inbound: Mutex<VecDeque<Result<Option<Event>, BusError>>>,
        /// Refuse this many sends with WouldBlock before accepting.
        would_block: Mutex<u32>,
        fail_send: Mutex<Option<fn() -> BusError>>,
        respond: Respond,
        heartbeats: AtomicU64,
    }

    impl FakeBus {
        fn new(respond: impl Fn(&FakeBus, &Sent) + Send + Sync + 'static) -> FakeBus {
            FakeBus {
                budget: BUDGET,
                sent: Mutex::new(Vec::new()),
                inbound: Mutex::new(VecDeque::new()),
                would_block: Mutex::new(0),
                fail_send: Mutex::new(None),
                respond: Box::new(respond),
                heartbeats: AtomicU64::new(0),
            }
        }
        fn push(&self, frame: Frame, payload: Vec<u8>) {
            self.inbound
                .lock()
                .unwrap()
                .push_back(Ok(Some(Event { frame, payload })));
        }
        fn push_err(&self, e: BusError) {
            self.inbound.lock().unwrap().push_back(Err(e));
        }
        fn sent(&self) -> Vec<Sent> {
            self.sent.lock().unwrap().clone()
        }
        fn record(&self, s: Sent) {
            self.sent.lock().unwrap().push(s.clone());
            (self.respond)(self, &s);
        }
    }

    impl CallerBus for FakeBus {
        fn poll(&self) -> Result<Option<Event>, BusError> {
            self.inbound.lock().unwrap().pop_front().unwrap_or(Ok(None))
        }
        fn request_fragment(
            &self,
            kind: u32,
            corr: u64,
            payload: &[u8],
            more: bool,
        ) -> Result<(), BusError> {
            assert!(
                payload.len() <= self.budget as usize,
                "fragment over budget"
            );
            if let Some(f) = *self.fail_send.lock().unwrap() {
                return Err(f());
            }
            {
                let mut wb = self.would_block.lock().unwrap();
                if *wb > 0 {
                    *wb -= 1;
                    return Err(BusError::WouldBlock);
                }
            }
            self.record(Sent::Fragment {
                kind,
                corr,
                payload: payload.to_vec(),
                more,
            });
            Ok(())
        }
        fn cancel(&self, kind: u32, corr: u64) -> Result<(), BusError> {
            self.record(Sent::Cancel { kind, corr });
            Ok(())
        }
        fn heartbeat(&self, _: u64) {
            self.heartbeats.fetch_add(1, Ordering::Relaxed);
        }
        fn max_inline_payload(&self) -> u32 {
            self.budget
        }
    }

    fn result_payload(status: ModuleStatus, stage: u32, trace: u64, body: &[u8]) -> Vec<u8> {
        let mut p = vec![0u8; MODULE_MESSAGE_HEADER_LEN + body.len()];
        ModuleMessage {
            operation: Operation::Result,
            status,
            stage_id: stage,
            flags: 0,
            body_len: body.len() as u32,
            deadline_ns: 0,
            trace_id: trace,
        }
        .encode(&mut p)
        .unwrap();
        p[MODULE_MESSAGE_HEADER_LEN..].copy_from_slice(body);
        p
    }

    fn reply_frame(kind: u32, corr: u64, more: bool, len: usize) -> Frame {
        Frame {
            hdr_flags: F_REPLY | F_INLINE | if more { F_MORE } else { 0 },
            event_kind: kind,
            correlation_id: corr,
            payload_len: len as u32,
            ..Frame::default()
        }
    }

    /// Queue `body` as a reply in fragments of at most `chunk` body bytes.
    fn reply(bus: &FakeBus, corr: u64, trace: u64, body: &[u8], chunk: usize) {
        let mut off = 0;
        loop {
            let part = (body.len() - off).min(chunk);
            let more = off + part < body.len();
            let p = result_payload(ModuleStatus::Ok, STAGE, trace, &body[off..off + part]);
            bus.push(reply_frame(KIND, corr, more, p.len()), p);
            off += part;
            if !more {
                break;
            }
        }
    }

    /// Reassemble the request the caller sent for `corr`.
    fn request_of(sent: &[Sent], corr: u64) -> (Vec<u8>, Vec<ModuleMessage>) {
        let mut body = Vec::new();
        let mut headers = Vec::new();
        for s in sent {
            if let Sent::Fragment {
                corr: c, payload, ..
            } = s
            {
                if *c == corr {
                    let m = ModuleMessage::decode(payload).unwrap();
                    body.extend_from_slice(m.body(payload));
                    headers.push(m);
                }
            }
        }
        (body, headers)
    }

    /// An echoing module: answers each completed request with its body.
    fn echo() -> FakeBus {
        let pending = Mutex::new(Vec::<u8>::new());
        FakeBus::new(move |bus, s| {
            if let Sent::Fragment {
                corr,
                payload,
                more,
                ..
            } = s
            {
                let m = ModuleMessage::decode(payload).unwrap();
                let mut p = pending.lock().unwrap();
                p.extend_from_slice(m.body(payload));
                if !more {
                    let body = std::mem::take(&mut *p);
                    reply(bus, *corr, m.trace_id, &body, 37);
                }
            }
        })
    }

    #[test]
    fn a_call_round_trips_and_carries_its_identity() {
        let bus = echo();
        let caller = ModuleCaller::new(&bus);
        let call = Call::new(KIND, STAGE, b"hello").trace_id(77);
        assert_eq!(caller.call(&call).unwrap(), b"hello");
        let sent = bus.sent();
        assert_eq!(sent.len(), 1);
        let (body, headers) = request_of(&sent, 1);
        assert_eq!(body, b"hello");
        let h = headers[0];
        assert_eq!(h.operation, Operation::Invoke);
        assert_eq!(h.status, ModuleStatus::Ok);
        assert_eq!((h.stage_id, h.trace_id, h.deadline_ns), (STAGE, 77, 0));
        assert!(bus.heartbeats.load(Ordering::Relaxed) > 0);
    }

    #[test]
    fn requests_fragment_at_the_inline_budget_and_replies_reassemble() {
        let request: Vec<u8> = (0..1000u32).map(|i| (i * 131 + 17) as u8).collect();
        let bus = echo();
        let caller = ModuleCaller::new(&bus);
        assert_eq!(
            caller.call(&Call::new(KIND, STAGE, &request)).unwrap(),
            request
        );
        let sent = bus.sent();
        let chunk = BUDGET as usize - MODULE_MESSAGE_HEADER_LEN;
        assert_eq!(sent.len(), request.len().div_ceil(chunk));
        for (i, s) in sent.iter().enumerate() {
            let Sent::Fragment { payload, more, .. } = s else {
                panic!("unexpected {s:?}")
            };
            assert_eq!(*more, i + 1 < sent.len(), "F_MORE on fragment {i}");
            assert!(payload.len() <= BUDGET as usize);
            // Every fragment is a complete envelope with the same identity.
            let m = ModuleMessage::decode(payload).unwrap();
            assert_eq!(m.stage_id, STAGE);
        }
        assert_eq!(request_of(&sent, 1).0, request);
    }

    #[test]
    fn an_empty_request_is_one_empty_fragment() {
        let bus = echo();
        let caller = ModuleCaller::new(&bus);
        assert_eq!(
            caller.call(&Call::new(KIND, STAGE, b"")).unwrap(),
            Vec::<u8>::new()
        );
        let sent = bus.sent();
        assert_eq!(sent.len(), 1);
        let Sent::Fragment { payload, more, .. } = &sent[0] else {
            panic!()
        };
        assert!(!more);
        assert_eq!(payload.len(), MODULE_MESSAGE_HEADER_LEN);
    }

    #[test]
    fn a_request_exactly_one_chunk_long_is_one_fragment() {
        let chunk = BUDGET as usize - MODULE_MESSAGE_HEADER_LEN;
        let request = vec![9u8; chunk];
        let bus = echo();
        let caller = ModuleCaller::new(&bus);
        assert_eq!(
            caller.call(&Call::new(KIND, STAGE, &request)).unwrap(),
            request
        );
        assert_eq!(bus.sent().len(), 1);
        let request = vec![9u8; chunk + 1];
        assert_eq!(
            caller.call(&Call::new(KIND, STAGE, &request)).unwrap(),
            request
        );
        assert_eq!(bus.sent().len(), 3);
    }

    #[test]
    fn correlations_are_fresh_per_call_and_never_zero() {
        let bus = echo();
        let caller = ModuleCaller::new(&bus);
        caller.next.store(u64::MAX, Ordering::Relaxed);
        for _ in 0..3 {
            caller.call(&Call::new(KIND, STAGE, b"x")).unwrap();
        }
        let corrs: Vec<u64> = bus
            .sent()
            .iter()
            .map(|s| match s {
                Sent::Fragment { corr, .. } => *corr,
                Sent::Cancel { corr, .. } => *corr,
            })
            .collect();
        assert_eq!(corrs, vec![u64::MAX, 1, 2]);
    }

    #[test]
    fn local_refusals_send_nothing() {
        let bus = echo();
        let caller = ModuleCaller::new(&bus);
        let big = vec![0u8; MODULE_MESSAGE_MAX_BODY as usize + 1];
        for (call, name) in [
            (
                Call::new(KIND_MODULE_BASE - 1, STAGE, b""),
                "INVALID_ARGUMENT",
            ),
            (Call::new(KIND, 0, b""), "INVALID_ARGUMENT"),
            (Call::new(KIND, STAGE, &big), "INVALID_ARGUMENT"),
            (
                Call::new(KIND, STAGE, b"").deadline_ns(1),
                "DEADLINE_EXCEEDED",
            ),
        ] {
            assert_eq!(caller.call(&call).unwrap_err().name(), name);
        }
        let err = caller
            .call_cancellable(&Call::new(KIND, STAGE, b""), &|| true)
            .unwrap_err();
        assert_eq!(err.name(), "CANCELLED");
        assert!(bus.sent().is_empty());
    }

    #[test]
    fn a_budget_that_cannot_hold_an_envelope_is_transport() {
        let mut bus = echo();
        bus.budget = MODULE_MESSAGE_HEADER_LEN as u32;
        let caller = ModuleCaller::new(&bus);
        assert!(matches!(
            caller.call(&Call::new(KIND, STAGE, b"x")),
            Err(CallError::Transport(BusError::Payload))
        ));
        assert!(bus.sent().is_empty());
    }

    #[test]
    fn host_refusals_end_the_call_at_once_without_a_cancel() {
        for (kind, name) in [
            (KIND_CAPABILITY_ABSENT, "CAPABILITY_ABSENT"),
            (KIND_CAPABILITY_DENIED, "CAPABILITY_DENIED"),
        ] {
            let bus = FakeBus::new(move |bus, s| {
                if let Sent::Fragment { corr, .. } = s {
                    bus.push(
                        Frame {
                            hdr_flags: F_NOTIFICATION | F_CONTROL,
                            event_kind: kind,
                            correlation_id: *corr,
                            ..Frame::default()
                        },
                        Vec::new(),
                    );
                }
            });
            let caller = ModuleCaller::new(&bus);
            // A generous deadline: the refusal must not wait it out.
            let started = std::time::Instant::now();
            let err = caller
                .call(&Call::new(KIND, STAGE, b"x").timeout(Duration::from_secs(30)))
                .unwrap_err();
            assert_eq!(err.name(), name);
            assert!(started.elapsed() < Duration::from_secs(5));
            assert!(
                !bus.sent().iter().any(|s| matches!(s, Sent::Cancel { .. })),
                "{name}: a refused request has nothing to cancel"
            );
        }
    }

    #[test]
    fn module_statuses_map_to_the_c_outcomes() {
        for (status, name) in [
            (ModuleStatus::CapabilityAbsent, "CAPABILITY_ABSENT"),
            (ModuleStatus::Cancelled, "CANCELLED"),
            (ModuleStatus::DeadlineExceeded, "DEADLINE_EXCEEDED"),
            (ModuleStatus::InvalidRequest, "INVALID_REQUEST"),
            (ModuleStatus::Internal, "INTERNAL"),
        ] {
            let bus = FakeBus::new(move |bus, s| {
                if let Sent::Fragment { corr, .. } = s {
                    let p = result_payload(status, STAGE, 0, b"");
                    bus.push(reply_frame(KIND, *corr, false, p.len()), p);
                }
            });
            let caller = ModuleCaller::new(&bus);
            assert_eq!(
                caller
                    .call(&Call::new(KIND, STAGE, b"x"))
                    .unwrap_err()
                    .name(),
                name
            );
            // The module finished the call; there is nothing to cancel.
            assert!(!bus.sent().iter().any(|s| matches!(s, Sent::Cancel { .. })));
        }
    }

    /// A malformed reply for this correlation is a protocol error, never
    /// skipped and never a success.
    #[test]
    fn replies_that_break_the_contract_are_protocol_errors() {
        type Make = fn(u64) -> (Frame, Vec<u8>);
        let cases: [(&str, Make); 10] = [
            ("not a reply", |corr| {
                let p = result_payload(ModuleStatus::Ok, STAGE, 0, b"ok");
                let mut f = reply_frame(KIND, corr, false, p.len());
                f.hdr_flags = F_REQUEST | F_INLINE;
                (f, p)
            }),
            ("wrong kind", |corr| {
                let p = result_payload(ModuleStatus::Ok, STAGE, 0, b"ok");
                (reply_frame(KIND + 1, corr, false, p.len()), p)
            }),
            ("undecodable", |corr| {
                (reply_frame(KIND, corr, false, 3), vec![1, 2, 3])
            }),
            ("an invoke, not a result", |corr| {
                let mut p = result_payload(ModuleStatus::Ok, STAGE, 0, b"ok");
                p[AMOD_OFF_OPERATION_FOR_TEST] = Operation::Invoke as u8;
                (reply_frame(KIND, corr, false, p.len()), p)
            }),
            ("wrong stage", |corr| {
                let p = result_payload(ModuleStatus::Ok, STAGE + 1, 0, b"ok");
                (reply_frame(KIND, corr, false, p.len()), p)
            }),
            ("wrong trace", |corr| {
                let p = result_payload(ModuleStatus::Ok, STAGE, 9, b"ok");
                (reply_frame(KIND, corr, false, p.len()), p)
            }),
            ("declared body longer than the payload", |corr| {
                let mut p = result_payload(ModuleStatus::Ok, STAGE, 0, b"ok");
                p.truncate(p.len() - 1);
                (reply_frame(KIND, corr, false, p.len()), p)
            }),
            ("non-OK with a body", |corr| {
                let mut p = result_payload(ModuleStatus::Ok, STAGE, 0, b"ok");
                p[AMOD_OFF_STATUS_FOR_TEST] = ModuleStatus::Internal as u8;
                (reply_frame(KIND, corr, false, p.len()), p)
            }),
            ("non-OK with F_MORE", |corr| {
                let p = result_payload(ModuleStatus::Internal, STAGE, 0, b"");
                (reply_frame(KIND, corr, true, p.len()), p)
            }),
            ("unknown status", |corr| {
                let mut p = result_payload(ModuleStatus::Ok, STAGE, 0, b"");
                p[AMOD_OFF_STATUS_FOR_TEST] = 77;
                (reply_frame(KIND, corr, false, p.len()), p)
            }),
        ];
        for (name, make) in cases {
            let bus = FakeBus::new(move |bus, s| {
                if let Sent::Fragment { corr, .. } = s {
                    let (f, p) = make(*corr);
                    bus.push(f, p);
                }
            });
            let caller = ModuleCaller::new(&bus);
            let err = caller.call(&Call::new(KIND, STAGE, b"x")).unwrap_err();
            assert_eq!(err.name(), "PROTOCOL", "{name}: {err}");
        }
    }

    #[test]
    fn a_non_ok_status_after_ok_fragments_is_a_protocol_error() {
        let bus = FakeBus::new(|bus, s| {
            if let Sent::Fragment { corr, .. } = s {
                let p = result_payload(ModuleStatus::Ok, STAGE, 0, b"part");
                bus.push(reply_frame(KIND, *corr, true, p.len()), p);
                let p = result_payload(ModuleStatus::Internal, STAGE, 0, b"");
                bus.push(reply_frame(KIND, *corr, false, p.len()), p);
            }
        });
        let caller = ModuleCaller::new(&bus);
        assert_eq!(
            caller
                .call(&Call::new(KIND, STAGE, b"x"))
                .unwrap_err()
                .name(),
            "PROTOCOL"
        );
    }

    #[test]
    fn stale_and_unrelated_frames_are_drained() {
        let bus = FakeBus::new(|bus, s| {
            if let Sent::Fragment { corr, .. } = s {
                // A late reply to an earlier correlation, a host control frame
                // for one, and an unrelated notification, all before ours.
                let p = result_payload(ModuleStatus::Cancelled, STAGE, 0, b"");
                bus.push(reply_frame(KIND, corr + 1000, false, p.len()), p);
                bus.push(
                    Frame {
                        hdr_flags: F_NOTIFICATION | F_CONTROL,
                        event_kind: KIND_CAPABILITY_ABSENT,
                        correlation_id: corr + 5000,
                        ..Frame::default()
                    },
                    Vec::new(),
                );
                bus.push(
                    Frame {
                        hdr_flags: F_NOTIFICATION | F_INLINE,
                        event_kind: 7000,
                        payload_len: 1,
                        ..Frame::default()
                    },
                    vec![1],
                );
                reply(bus, *corr, 0, b"mine", 64);
            }
        });
        let caller = ModuleCaller::new(&bus);
        assert_eq!(caller.call(&Call::new(KIND, STAGE, b"x")).unwrap(), b"mine");
    }

    #[test]
    fn a_response_over_the_limit_is_drained_and_reported_with_its_length() {
        let body: Vec<u8> = (0..500u32).map(|i| i as u8).collect();
        let bus = echo();
        let caller = ModuleCaller::new(&bus);
        let err = caller
            .call(&Call::new(KIND, STAGE, &body).response_limit(100))
            .unwrap_err();
        assert!(
            matches!(err, CallError::ResponseTooLarge { len: 500 }),
            "{err}"
        );
        // Every fragment was consumed: nothing is left for the next call, and
        // the next call succeeds with its own reply.
        assert!(bus.inbound.lock().unwrap().is_empty());
        assert_eq!(
            caller.call(&Call::new(KIND, STAGE, b"next")).unwrap(),
            b"next"
        );
        // A limit exactly the response length is not too large.
        assert_eq!(
            caller
                .call(&Call::new(KIND, STAGE, &body).response_limit(500))
                .unwrap(),
            body
        );
        // No cancel: the call completed at the module.
        assert!(!bus.sent().iter().any(|s| matches!(s, Sent::Cancel { .. })));
    }

    #[test]
    fn a_reassembly_past_the_amod_limit_is_a_protocol_error() {
        let bus = FakeBus::new(|bus, s| {
            if let Sent::Fragment { corr, .. } = s {
                // Two fragments whose declared lengths together pass the
                // body limit; each payload carries what it declares.
                let first = vec![0u8; MODULE_MESSAGE_MAX_BODY as usize];
                let p = result_payload(ModuleStatus::Ok, STAGE, 0, &first);
                bus.push(reply_frame(KIND, *corr, true, p.len()), p);
                let p = result_payload(ModuleStatus::Ok, STAGE, 0, b"!");
                bus.push(reply_frame(KIND, *corr, false, p.len()), p);
            }
        });
        let caller = ModuleCaller::new(&bus);
        assert_eq!(
            caller
                .call(&Call::new(KIND, STAGE, b"x"))
                .unwrap_err()
                .name(),
            "PROTOCOL"
        );
    }

    #[test]
    fn a_deadline_while_waiting_cancels_the_correlation() {
        let bus = FakeBus::new(|_, _| {}); // a module that never answers
        let caller = ModuleCaller::new(&bus);
        let started = std::time::Instant::now();
        let err = caller
            .call(&Call::new(KIND, STAGE, b"slow").timeout(Duration::from_millis(30)))
            .unwrap_err();
        assert_eq!(err.name(), "DEADLINE_EXCEEDED");
        assert!(started.elapsed() >= Duration::from_millis(30));
        let sent = bus.sent();
        assert_eq!(
            sent.last(),
            Some(&Sent::Cancel {
                kind: KIND,
                corr: 1
            }),
            "{sent:?}"
        );
        // The deadline travels in every request envelope.
        let (_, headers) = request_of(&sent, 1);
        assert!(headers.iter().all(|h| h.deadline_ns != 0));
    }

    #[test]
    fn cancellation_while_waiting_cancels_the_correlation() {
        let bus = FakeBus::new(|_, _| {});
        let caller = ModuleCaller::new(&bus);
        let flag = Arc::new(AtomicBool::new(false));
        let setter = flag.clone();
        let t = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(20));
            setter.store(true, Ordering::Release);
        });
        let err = caller
            .call_cancellable(&Call::new(KIND, STAGE, b"x"), &|| {
                flag.load(Ordering::Acquire)
            })
            .unwrap_err();
        t.join().unwrap();
        assert_eq!(err.name(), "CANCELLED");
        assert_eq!(
            bus.sent().last(),
            Some(&Sent::Cancel {
                kind: KIND,
                corr: 1
            })
        );
    }

    #[test]
    fn a_full_outbound_ring_is_retried_then_sent() {
        let bus = echo();
        *bus.would_block.lock().unwrap() = 5;
        let caller = ModuleCaller::new(&bus);
        assert_eq!(
            caller.call(&Call::new(KIND, STAGE, b"retry")).unwrap(),
            b"retry"
        );
    }

    #[test]
    fn a_deadline_while_the_ring_stays_full_sends_no_cancel_for_an_unsent_call() {
        let bus = echo();
        *bus.would_block.lock().unwrap() = u32::MAX;
        let caller = ModuleCaller::new(&bus);
        let err = caller
            .call(&Call::new(KIND, STAGE, b"x").timeout(Duration::from_millis(20)))
            .unwrap_err();
        assert_eq!(err.name(), "DEADLINE_EXCEEDED");
        assert!(
            bus.sent().is_empty(),
            "nothing reached the host: {:?}",
            bus.sent()
        );
    }

    #[test]
    fn a_cancel_mid_request_cancels_what_was_already_sent() {
        // The first fragment goes out; then the ring stays full while the
        // caller is cancelled. The host has a partial request: cancel it.
        let request = vec![1u8; 200];
        let sent_once = Arc::new(AtomicBool::new(false));
        let flag = sent_once.clone();
        let bus = FakeBus::new(move |bus, s| {
            if matches!(s, Sent::Fragment { .. }) {
                flag.store(true, Ordering::Release);
                *bus.would_block.lock().unwrap() = u32::MAX;
            }
        });
        let caller = ModuleCaller::new(&bus);
        let err = caller
            .call_cancellable(&Call::new(KIND, STAGE, &request), &|| {
                sent_once.load(Ordering::Acquire)
            })
            .unwrap_err();
        assert_eq!(err.name(), "CANCELLED");
        let sent = bus.sent();
        assert_eq!(sent.len(), 2, "{sent:?}");
        assert!(matches!(sent[0], Sent::Fragment { more: true, .. }));
        assert_eq!(
            sent[1],
            Sent::Cancel {
                kind: KIND,
                corr: 1
            }
        );
    }

    #[test]
    fn a_failed_send_is_transport_and_cancels_only_if_something_went_out() {
        let bus = echo();
        *bus.fail_send.lock().unwrap() = Some(|| BusError::Epoch);
        let caller = ModuleCaller::new(&bus);
        assert!(matches!(
            caller.call(&Call::new(KIND, STAGE, b"x")),
            Err(CallError::Transport(BusError::Epoch))
        ));
        assert!(bus.sent().is_empty());
    }

    #[test]
    fn a_poll_failure_is_transport_with_nothing_left_to_cancel() {
        let bus = FakeBus::new(|bus, s| {
            if matches!(s, Sent::Fragment { .. }) {
                bus.push_err(BusError::Epoch);
            }
        });
        let caller = ModuleCaller::new(&bus);
        assert!(matches!(
            caller.call(&Call::new(KIND, STAGE, b"x")),
            Err(CallError::Transport(BusError::Epoch))
        ));
        assert!(!bus.sent().iter().any(|s| matches!(s, Sent::Cancel { .. })));
    }

    #[test]
    fn concurrent_callers_take_turns_and_each_gets_its_own_reply() {
        let bus = Arc::new(echo());
        let caller = Arc::new(ModuleCaller::new(bus.clone()));
        let threads: Vec<_> = (0..8u8)
            .map(|i| {
                let caller = caller.clone();
                std::thread::spawn(move || {
                    for j in 0..20u8 {
                        let body = vec![i; 1 + usize::from(j) * 13];
                        assert_eq!(caller.call(&Call::new(KIND, STAGE, &body)).unwrap(), body);
                    }
                })
            })
            .collect();
        for t in threads {
            t.join().unwrap();
        }
        // Serialised: the fragments of one correlation are never interleaved
        // with another's.
        let mut last = 0;
        let mut seen = std::collections::HashSet::new();
        for s in bus.sent() {
            if let Sent::Fragment { corr, .. } = s {
                if corr != last {
                    assert!(seen.insert(corr), "correlation {corr} interleaved");
                    last = corr;
                }
            }
        }
        assert_eq!(seen.len(), 160);
    }

    #[test]
    fn error_names_are_the_c_result_names() {
        let all = [
            CallError::CapabilityAbsent,
            CallError::CapabilityDenied,
            CallError::Cancelled,
            CallError::DeadlineExceeded,
            CallError::InvalidRequest,
            CallError::Internal,
            CallError::ResponseTooLarge { len: 1 },
            CallError::Transport(BusError::WouldBlock),
            CallError::Protocol("x"),
            CallError::InvalidArgument("x"),
        ];
        let names: Vec<_> = all.iter().map(|e| e.name()).collect();
        // aimee_module_call_result_name, less "OK", in enum order.
        assert_eq!(
            names,
            [
                "CAPABILITY_ABSENT",
                "CAPABILITY_DENIED",
                "CANCELLED",
                "DEADLINE_EXCEEDED",
                "INVALID_REQUEST",
                "INTERNAL",
                "RESPONSE_TOO_LARGE",
                "TRANSPORT",
                "PROTOCOL",
                "INVALID_ARGUMENT"
            ]
        );
        for e in &all {
            assert!(e.to_string().contains(e.name()));
        }
    }

    // Byte offsets used to corrupt an encoded envelope (little-endian u16s;
    // the low byte is enough for these values).
    const AMOD_OFF_OPERATION_FOR_TEST: usize = crate::layout::AMOD_OFF_OPERATION;
    const AMOD_OFF_STATUS_FOR_TEST: usize = crate::layout::AMOD_OFF_STATUS;
}
