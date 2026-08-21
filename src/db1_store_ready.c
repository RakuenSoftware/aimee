/* db1_store_ready.c: is the DB1 store reachable from this process?
 *
 * The question every caller used to ask was "did db1_init succeed", and since
 * the families moved that answers nothing: opening a local SQLite file says
 * only that a file exists. The store is a module now, and whether it can be
 * reached is a property of the bus.
 *
 * Any one stage answers it. The module registers every stage it serves when it
 * attaches and unregisters them together when it goes, so there is no state in
 * which one is present and another is not -- which is why this asks about one
 * rather than pretending to check them all.
 */
#include "db1.h"
#include "db1_module_api.h"

#include <aimee/audit/obs_bus.h>

int db1_store_ready(void)
{
   return obs_bus_module_available(AIMEE_DB1_EVENT_SESSIONS) ? 1 : 0;
}
