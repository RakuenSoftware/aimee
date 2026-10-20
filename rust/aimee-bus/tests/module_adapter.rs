//! Adapter contract exercised through the unchanged AMOD runtime.
use aimee_bus::module_adapter::{Adapter, Backend, ConfigError, Response, StageLimits};
use aimee_bus::module_protocol::*;
use aimee_bus::module_runtime::{
    Invocation, ModuleBus, ModuleProcessConfig, ModuleStage, Shutdown, run_module_client,
};
use aimee_bus::wire::{F_CANCEL, F_INLINE, F_MORE, F_REQUEST, Frame, HDR_LEN, WIRE_VERSION};
use aimee_bus::{BusError, Event};
use std::collections::{HashMap, VecDeque};
use std::io::Write;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, AtomicUsize, Ordering},
};
use std::time::{Duration, Instant};
const KIND: u32 = 5889;
#[derive(Default)]
struct Bus {
    input: Mutex<VecDeque<Event>>,
    output: Mutex<Vec<(u64, Vec<u8>, bool)>>,
}
impl ModuleBus for Bus {
    fn poll(&self) -> Result<Option<Event>, BusError> {
        Ok(self.input.lock().unwrap().pop_front())
    }
    fn reply_fragment(
        &self,
        _: u32,
        correlation: u64,
        payload: &[u8],
        more: bool,
    ) -> Result<(), BusError> {
        self.output
            .lock()
            .unwrap()
            .push((correlation, payload.to_vec(), more));
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
impl Bus {
    fn request(&self, correlation: u64, stage: u32, body: &[u8], deadline: u64) {
        let chunks: Vec<_> = if body.is_empty() {
            vec![&[][..]]
        } else {
            body.chunks(60).collect()
        };
        for (i, chunk) in chunks.iter().enumerate() {
            let mut payload = vec![0; MODULE_MESSAGE_HEADER_LEN + chunk.len()];
            ModuleMessage {
                operation: Operation::Invoke,
                status: ModuleStatus::Ok,
                stage_id: stage,
                flags: 0,
                body_len: chunk.len() as u32,
                deadline_ns: deadline,
                trace_id: 100 + correlation,
            }
            .encode(&mut payload)
            .unwrap();
            payload[MODULE_MESSAGE_HEADER_LEN..].copy_from_slice(chunk);
            self.input.lock().unwrap().push_back(Event {
                frame: Frame {
                    hdr_flags: F_REQUEST | F_INLINE | if i + 1 < chunks.len() { F_MORE } else { 0 },
                    wire_version: WIRE_VERSION,
                    event_kind: KIND,
                    principal_ref: 90,
                    correlation_id: correlation,
                    payload_ref: HDR_LEN as u64,
                    payload_len: payload.len() as u32,
                    ..Frame::default()
                },
                payload,
            });
        }
    }
    fn cancel(&self, correlation: u64) {
        self.input.lock().unwrap().push_back(Event {
            frame: Frame {
                hdr_flags: F_CANCEL,
                wire_version: WIRE_VERSION,
                event_kind: KIND,
                principal_ref: 90,
                correlation_id: correlation,
                ..Frame::default()
            },
            payload: Vec::new(),
        });
    }
    fn result(&self, correlation: u64) -> Option<(ModuleStatus, Vec<u8>)> {
        let mut body = Vec::new();
        for (_, payload, more) in self
            .output
            .lock()
            .unwrap()
            .iter()
            .filter(|(c, _, _)| *c == correlation)
        {
            let message = ModuleMessage::decode(payload).unwrap();
            assert_eq!(message.trace_id, 100 + correlation);
            body.extend_from_slice(message.body(payload));
            if !more {
                return Some((message.status, body));
            }
        }
        None
    }
}
#[derive(Default)]
struct Fixture {
    calls: AtomicUsize,
    started: AtomicBool,
}
impl Backend for Fixture {
    fn invoke(
        &self,
        inv: &Invocation,
        request: &[u8],
        response: &mut Response,
    ) -> Result<(), ModuleStatus> {
        assert_eq!(inv.principal_class, 1);
        assert_eq!(inv.principal_ref, 90);
        assert_eq!(inv.stage_id, 1);
        self.calls.fetch_add(1, Ordering::SeqCst);
        match request {
            b"refuse" => {
                response.write_all(b"private backend detail").unwrap();
                return Err(ModuleStatus::InvalidRequest);
            }
            b"bad-status" => return Err(ModuleStatus::Ok),
            b"overflow" => {
                let _ = response.write_all(&vec![0; response.limit() + 1]);
                return Ok(());
            }
            b"panic" => panic!("fixture panic"),
            b"cancel" => {
                self.started.store(true, Ordering::Release);
                while !inv.cancelled() {
                    std::thread::sleep(Duration::from_millis(1));
                }
                return Err(ModuleStatus::Cancelled);
            }
            _ => {}
        }
        response
            .write_all(request)
            .map_err(|_| ModuleStatus::Internal)
    }
}
fn limits() -> StageLimits {
    StageLimits {
        stage_id: 1,
        request_bytes: 2048,
        response_bytes: 2048,
    }
}
fn wait(predicate: impl Fn() -> bool) {
    let until = Instant::now() + Duration::from_secs(5);
    while !predicate() {
        assert!(Instant::now() < until, "adapter fixture timed out");
        std::thread::sleep(Duration::from_millis(1));
    }
}
fn serve(bus: Arc<Bus>, backend: Arc<Fixture>, actions: impl FnOnce(&Bus, &Fixture)) {
    let handler = Adapter::new(backend.clone(), [limits()]).unwrap().handler();
    let shutdown = Shutdown::new();
    let stop = shutdown.clone();
    let transport = bus.clone();
    let thread = std::thread::spawn(move || {
        let config = ModuleProcessConfig {
            socket_path: "/unused".into(),
            module_name: "adapter-fixture".into(),
            principal_class: 1,
            principal_ref: 7,
            stages: vec![ModuleStage {
                event_kind: KIND,
                stage_id: 1,
            }],
            handler: Some(handler),
            max_idle_poll: Duration::ZERO,
            after_attach: None,
        };
        // Stage 2 is intentionally served by the transport but not by the adapter.
        run_module_client(
            &config,
            &HashMap::from([(KIND, 1), (KIND + 1, 2)]),
            &*transport,
            &stop,
        )
        .unwrap();
    });
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| actions(&bus, &backend)));
    shutdown.trigger();
    thread.join().unwrap();
    if let Err(panic) = result {
        std::panic::resume_unwind(panic);
    }
}
#[test]
fn byte_payloads_fragments_metadata_and_error_bodies() {
    let bus = Arc::new(Bus::default());
    let backend = Arc::new(Fixture::default());
    let large: Vec<_> = (0..1024).map(|i| (i % 256) as u8).collect();
    for (i, body) in [
        b"".as_slice(),
        large.as_slice(),
        b"refuse",
        b"bad-status",
        b"overflow",
        b"panic",
    ]
    .iter()
    .enumerate()
    {
        bus.request(i as u64 + 1, 1, body, 0);
    }
    serve(bus, backend, |bus, _| {
        wait(|| (1..=6).all(|c| bus.result(c).is_some()));
        assert_eq!(bus.result(1).unwrap(), (ModuleStatus::Ok, Vec::new()));
        assert_eq!(bus.result(2).unwrap(), (ModuleStatus::Ok, large));
        assert_eq!(
            bus.result(3).unwrap(),
            (ModuleStatus::InvalidRequest, Vec::new())
        );
        for c in 4..=6 {
            assert_eq!(bus.result(c).unwrap(), (ModuleStatus::Internal, Vec::new()));
        }
    });
}
#[test]
fn limits_and_unconfigured_stages_do_not_reach_backend() {
    let bus = Arc::new(Bus::default());
    let backend = Arc::new(Fixture::default());
    bus.request(1, 1, &[0; 2049], 0);
    bus.request(2, 1, b"expired", 1);
    bus.request(3, 2, b"unknown", 0);
    bus.input
        .lock()
        .unwrap()
        .back_mut()
        .unwrap()
        .frame
        .event_kind = KIND + 1;
    serve(bus, backend, |bus, backend| {
        wait(|| (1..=3).all(|c| bus.result(c).is_some()));
        assert_eq!(bus.result(1).unwrap().0, ModuleStatus::InvalidRequest);
        assert_eq!(bus.result(2).unwrap().0, ModuleStatus::DeadlineExceeded);
        assert_eq!(bus.result(3).unwrap().0, ModuleStatus::InvalidRequest);
        assert_eq!(backend.calls.load(Ordering::SeqCst), 0);
    });
}
#[test]
fn cancellation_reaches_backend_and_does_not_retry() {
    let bus = Arc::new(Bus::default());
    let backend = Arc::new(Fixture::default());
    bus.request(1, 1, b"cancel", 0);
    serve(bus, backend, |bus, backend| {
        wait(|| backend.started.load(Ordering::Acquire));
        bus.cancel(1);
        wait(|| bus.result(1).is_some());
        assert_eq!(
            bus.result(1).unwrap(),
            (ModuleStatus::Cancelled, Vec::new())
        );
        assert_eq!(backend.calls.load(Ordering::SeqCst), 1);
    });
}
#[test]
fn invalid_stage_tables_and_budgets_are_rejected() {
    let backend = Arc::new(Fixture::default());
    assert_eq!(
        Adapter::new(backend.clone(), []).unwrap_err(),
        ConfigError::NoStages
    );
    assert_eq!(
        Adapter::new(backend.clone(), [limits(), limits()]).unwrap_err(),
        ConfigError::DuplicateStage
    );
    for bad in [
        StageLimits {
            stage_id: 0,
            ..limits()
        },
        StageLimits {
            request_bytes: 0,
            ..limits()
        },
        StageLimits {
            response_bytes: MODULE_MESSAGE_MAX_BODY as usize + 1,
            ..limits()
        },
    ] {
        assert!(Adapter::new(backend.clone(), [bad]).is_err());
    }
}
