#ifndef SERVER_KB_HEARTBEAT_H
#define SERVER_KB_HEARTBEAT_H

/* Start/stop the optional server→kb heartbeat worker. Start is inert when no
 * AIMEE_KB_CONN is configured and fails closed for an invalid server id. */
int server_kb_heartbeat_start(const char *server_id);
void server_kb_heartbeat_stop(void);

#endif
