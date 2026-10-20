//! Conformance module process: the Rust counterpart of
//! `server-go/bus/testdata/module_helper`. `unit-test-module-runtime` spawns
//! it, admits it by executable path and peer credentials, and drives
//! dispatch, fragmentation, deadlines, cancellation and shutdown against it.
//!
//! Serves kind 5889 stage 1 as principal class 1 ref 7. It echoes the
//! request, except "cancel", which waits until the call is cancelled.

use aimee_bus::module_adapter::{Adapter, Backend, Response, StageLimits};
use aimee_bus::module_protocol::{MODULE_MESSAGE_MAX_BODY, ModuleStatus};
use aimee_bus::module_runtime::{
    Invocation, ModuleProcessConfig, ModuleStage, Shutdown, run_module_process,
};
use std::io::Write;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

const TEST_KIND: u32 = 5889;
const TEST_STAGE: u32 = 1;

static SIGNALLED: AtomicBool = AtomicBool::new(false);

extern "C" fn on_signal(_: libc::c_int) {
    SIGNALLED.store(true, Ordering::Release);
}

fn install_signal_handlers() {
    for sig in [libc::SIGTERM, libc::SIGINT] {
        // SAFETY: the handler only performs an atomic store, which is
        // async-signal-safe; sigaction is given a fully initialised struct.
        unsafe {
            let mut action: libc::sigaction = std::mem::zeroed();
            action.sa_sigaction = on_signal as *const () as libc::sighandler_t;
            libc::sigemptyset(&mut action.sa_mask);
            libc::sigaction(sig, &action, std::ptr::null_mut());
        }
    }
}

#[derive(Debug)]
struct Echo;
impl Backend for Echo {
    fn invoke(
        &self,
        inv: &Invocation,
        req: &[u8],
        response: &mut Response,
    ) -> Result<(), ModuleStatus> {
        if req == b"cancel" {
            while !inv.cancelled() {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        response.write_all(req).map_err(|_| ModuleStatus::Internal)
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: {} DAEMON_MODULE_BUS_SOCKET", args[0]);
        std::process::exit(2);
    }
    install_signal_handlers();
    let shutdown = Shutdown::new();
    let watcher = shutdown.clone();
    std::thread::spawn(move || {
        while !SIGNALLED.load(Ordering::Acquire) {
            std::thread::sleep(Duration::from_millis(5));
        }
        watcher.trigger();
    });

    let config = ModuleProcessConfig {
        socket_path: args[1].clone().into(),
        module_name: "rust-conformance-module".into(),
        principal_class: 1,
        principal_ref: 7,
        stages: vec![ModuleStage {
            event_kind: TEST_KIND,
            stage_id: TEST_STAGE,
        }],
        handler: Some(
            Adapter::new(
                Arc::new(Echo),
                [StageLimits {
                    stage_id: TEST_STAGE,
                    request_bytes: MODULE_MESSAGE_MAX_BODY as usize,
                    response_bytes: MODULE_MESSAGE_MAX_BODY as usize,
                }],
            )
            .expect("fixture limits")
            .handler(),
        ),
        max_idle_poll: Duration::ZERO,
        after_attach: None,
    };
    if let Err(e) = run_module_process(config, &shutdown) {
        eprintln!("{e}");
        std::process::exit(1);
    }
}
