/* web_search_breaker.h -- per-engine health, so a dead engine is not retried on
 * every call.
 *
 * `http_retry.c` retries within one call but keeps no memory across calls, so an
 * engine that is down costs its full retry ladder every single search. This adds
 * the missing memory: consecutive failures trip a breaker, the engine is skipped
 * for a cooldown, then ONE probe decides whether it is back.
 *
 * AN EMPTY RESULT IS A FAILURE. A scraped engine that has decided you are a bot
 * answers 200 with an empty result list, so success-by-status-code would keep
 * an engine that never returns anything permanently "healthy". Callers report
 * outcomes through web_search_breaker_report(), not through HTTP status.
 *
 * A caveat worth stating: this makes a genuinely empty result set (a query with
 * no hits anywhere) count against the engine. With a 3-strike threshold that
 * needs three consecutive no-hit searches to trip, and the cost of being wrong
 * is one cooldown during which the OTHER engines still answer. That asymmetry
 * is why empty-as-failure is the right default despite the false positive.
 *
 * STATE IS PROCESS-LOCAL AND DELIBERATELY FORGOTTEN ON RESTART. The breaker
 * exists to stop hammering a dead engine within a working session. Persisting it
 * would let a stale "engine is dead" verdict outlive the outage that caused it
 * and suppress a working engine after a restart -- the worse failure of the two,
 * because it is silent and self-perpetuating.
 *
 * Thresholds below are GUESSES, not measurements. There is no engine-uptime data
 * to fit them to. Revise them against observed behaviour; do not document them
 * as justified. */
#ifndef DEC_WEB_SEARCH_BREAKER_H
#define DEC_WEB_SEARCH_BREAKER_H 1

/* Consecutive failures that trip the breaker. */
#define WEB_BREAKER_THRESHOLD 3

/* How long a tripped engine is skipped before one probe is allowed. */
#define WEB_BREAKER_COOLDOWN_SECONDS 60

/* May `engine` be called right now?
 *
 * Returns 1 when closed (healthy) or when the cooldown has elapsed and this
 * caller is being handed the single half-open probe. Returns 0 while the
 * breaker is open. Handing out the probe is a state change, so two concurrent
 * callers cannot both be told to probe. */
int web_search_breaker_allow(const char *engine);

/* Record the outcome of a call. `ok` must be 0 for an empty result set, not
 * just for a transport error -- see the header comment. Success closes the
 * breaker and clears the count; failure increments it and may trip. */
void web_search_breaker_report(const char *engine, int ok);

/* Open (tripped) or not. For tests and for reporting; does not hand out a probe
 * and does not change state. */
int web_search_breaker_is_open(const char *engine);

/* Forget all engine state. For tests, so one case cannot leak into the next. */
void web_search_breaker_reset_all(void);

/* Override the clock, for tests. `now` of 0 restores the real clock. */
void web_search_breaker_set_clock(long (*now)(void));

#endif /* DEC_WEB_SEARCH_BREAKER_H */
