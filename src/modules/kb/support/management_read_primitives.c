#include "kb_store_management_read.h"

const char *server_mgmt_read_selector_name(int selector)
{
   return selector == KB_STORE_SERVER_MGMT_READ_SELECTOR_AGENTS   ? "agents"
          : selector == KB_STORE_SERVER_MGMT_READ_SELECTOR_CONFIG ? "config"
                                                                  : 0;
}
