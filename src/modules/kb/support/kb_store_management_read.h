#ifndef AIMEE_KB_STORE_SUPPORT_MANAGEMENT_READ_H
#define AIMEE_KB_STORE_SUPPORT_MANAGEMENT_READ_H

/*
 * Descriptor-owned numeric ABI for the one management-read selector helper
 * currently required by KB_STORE.  The authoritative legacy enum is compile-time
 * checked by the parity test while the monolith remains active.
 */
enum
{
   KB_STORE_SERVER_MGMT_READ_SELECTOR_AGENTS = 1,
   KB_STORE_SERVER_MGMT_READ_SELECTOR_CONFIG = 2
};

const char *server_mgmt_read_selector_name(int selector);

#endif
