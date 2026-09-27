#ifndef AIMEE_DB2_SUPPORT_MANAGEMENT_READ_H
#define AIMEE_DB2_SUPPORT_MANAGEMENT_READ_H

/*
 * Descriptor-owned numeric ABI for the one management-read selector helper
 * currently required by DB2.  The authoritative legacy enum is compile-time
 * checked by the parity test while the monolith remains active.
 */
enum
{
   DB2_SERVER_MGMT_READ_SELECTOR_AGENTS = 1,
   DB2_SERVER_MGMT_READ_SELECTOR_CONFIG = 2
};

const char *server_mgmt_read_selector_name(int selector);

#endif
