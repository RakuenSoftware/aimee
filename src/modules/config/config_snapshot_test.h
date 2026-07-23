#ifndef AIMEE_CONFIG_SNAPSHOT_TEST_H
#define AIMEE_CONFIG_SNAPSHOT_TEST_H

/* Internal deterministic coordination seam for test_config_snapshot.  Production
 * leaves the hook unset; state injection is valid only under quiescent test
 * control. */
typedef enum
{
   CONFIG_SNAPSHOT_TEST_AFTER_OBSERVE = 1,
   CONFIG_SNAPSHOT_TEST_PIN_ACQUIRED = 2,
   CONFIG_SNAPSHOT_TEST_PIN_VALIDATED = 3,
   CONFIG_SNAPSHOT_TEST_BEFORE_WRITE = 4,
   CONFIG_SNAPSHOT_TEST_BEFORE_RESERVE = 5,
   CONFIG_SNAPSHOT_TEST_RESERVE_CONTENDED = 6,
} config_snapshot_test_event_t;

typedef void (*config_snapshot_test_hook_fn)(config_snapshot_test_event_t event, unsigned slot,
                                             void *ctx);

void config_snapshot_test_set_hook(config_snapshot_test_hook_fn hook, void *ctx);
unsigned config_snapshot_test_writer_reserved(void);
unsigned config_snapshot_test_reader_max(void);
int config_snapshot_test_set_slot_state(unsigned slot, unsigned state);
unsigned config_snapshot_test_get_slot_state(unsigned slot);
unsigned config_snapshot_test_active_slot(void);

#endif
