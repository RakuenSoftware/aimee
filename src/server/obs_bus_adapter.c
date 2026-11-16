/* obs_bus_adapter.c: server-only persistence edges for the shared daemon bus. */
#include "obs_bus_adapter.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/audit/audit_worm.h>

#include "config_client.h"
#include "db1_client/db1_module_api.h"
#include "db1_client/guardrail_events.h"
#include "server.h"
#include "server_delegate_monitor.h"

/* Test binaries that link the adapter without the park loop or the delegate
 * monitor have nothing to wake. */
extern void server_park_wake(void) __attribute__((weak));
extern void server_delegate_monitor_wake(void) __attribute__((weak));
extern void server_coord_dispatcher_notify(void) __attribute__((weak));
extern void server_learning_recovery_wake(void) __attribute__((weak));
extern void trigger_scheduler_jobs_changed(void) __attribute__((weak));
extern void server_kb_heartbeat_wake(void) __attribute__((weak));

static int persist_guardrail(const guardrail_event_t *event, void *ctx)
{
   (void)ctx;
   /* The DB1 implementation owns its transaction gate, so it is safe to call
    * from the bus consumer thread. */
   return db1_guardrail_event_insert(event);
}

static int persist_durable(const char *actor_role, const char *actor_principal, const char *action,
                           const char *subject, const char *verdict, const char *detail, void *ctx)
{
   (void)ctx;
   return audit_worm_append(actor_role, actor_principal, action, subject, verdict, detail);
}

/* The config module announced a change, or the ring says one was missed. Runs
 * on the bus consumer thread, so it only records it and wakes the park loop,
 * which reloads. */
static void config_changed(uint32_t kind, const uint8_t *payload, uint32_t len, void *ctx)
{
   (void)kind, (void)ctx;
   config_client_note_announcement(payload, len);
   if (server_park_wake)
      server_park_wake();
   /* The delegates module toggle gates coord dispatch; config holds cron jobs. */
   if (server_coord_dispatcher_notify)
      server_coord_dispatcher_notify();
   if (trigger_scheduler_jobs_changed)
      trigger_scheduler_jobs_changed();
   /* KB mode and connection are config: an unenrolled server looks again. */
   if (server_kb_heartbeat_wake)
      server_kb_heartbeat_wake();
}

/* A delegation or agent job changed: the delegate monitor, the coord
 * dispatcher and the cron scheduler (coord jobs, tasks and cron jobs are agent
 * work) look now instead of sweeping on a timer. */
static void delegation_changed(uint32_t kind, const uint8_t *payload, uint32_t len, void *ctx)
{
   (void)kind, (void)payload, (void)len, (void)ctx;
   if (server_delegate_monitor_wake)
      server_delegate_monitor_wake();
   if (server_coord_dispatcher_notify)
      server_coord_dispatcher_notify();
   if (trigger_scheduler_jobs_changed)
      trigger_scheduler_jobs_changed();
}

/* A learning projection was queued: the delivery worker claims it now. */
static void learning_projection_pending(uint32_t kind, const uint8_t *payload, uint32_t len,
                                        void *ctx)
{
   (void)kind, (void)payload, (void)len, (void)ctx;
   if (server_learning_recovery_wake)
      server_learning_recovery_wake();
}

int server_obs_bus_configure(void)
{
   if (obs_bus_set_durable_sink(persist_durable, NULL) != 0)
      return -1;
   if (obs_bus_on_notification(AIMEE_CONFIG_CHANGED_KIND, config_changed, NULL) != 0 ||
       obs_bus_on_notification(AIMEE_DB1_EVENT_DELEGATION_CHANGED, delegation_changed, NULL) != 0 ||
       obs_bus_on_notification(AIMEE_DB1_EVENT_LEARNING_PROJECTION_PENDING,
                               learning_projection_pending, NULL) != 0)
      return -1;
   /* config_client_changed answers from those announcements, not by asking. */
   config_client_use_announcements();
   return obs_bus_set_guardrail_sink(persist_guardrail, NULL);
}
