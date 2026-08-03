/* obs_bus_adapter.h: aimee-server sinks for the shared daemon event bus. */
#ifndef AIMEE_SERVER_OBS_BUS_ADAPTER_H
#define AIMEE_SERVER_OBS_BUS_ADAPTER_H 1

/* Install server-owned event sinks before the observability bus starts.
 * Idempotent while the bus is stopped; returns 0 on success, -1 if the bus was
 * already running with a different profile. */
int server_obs_bus_configure(void);

#endif /* AIMEE_SERVER_OBS_BUS_ADAPTER_H */
