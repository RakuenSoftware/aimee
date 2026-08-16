#include "db2_management_read.h"

const char *server_mgmt_read_selector_name(int selector)
{
   return selector == DB2_SERVER_MGMT_READ_SELECTOR_AGENTS   ? "agents"
          : selector == DB2_SERVER_MGMT_READ_SELECTOR_CONFIG ? "config"
                                                             : 0;
}
