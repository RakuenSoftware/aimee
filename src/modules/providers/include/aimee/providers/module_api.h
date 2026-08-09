/* Wire contract for provider/model configuration.
 *
 * A provider owns an endpoint and credentials; models hang off it, and each
 * model carries the numbers that describe it -- context window, output ceiling,
 * prices, capabilities. This module owns the RULES for that surface: which
 * value wins, where it came from, and whether a proposed declaration is usable.
 * The CLI, the server and the GUI are three clients of one set of rules rather
 * than three implementations of them, which is what stopped being true when the
 * same precedence was rewritten at four call sites and disagreed at each.
 *
 * WHY A MODULE AND NOT A SERVER HANDLER. The bus already carries request/reply
 * module calls (module_client.h: correlation ids, AMOD envelopes, deadlines,
 * cancellation, response validation), and obs_bus.h states the migration rule:
 * "Each migration is all-or-nothing: the bus is the SOLE route for that event,
 * not a flagged parallel path." A second path through server_agent.c would be
 * exactly the flagged parallel path that rule forbids.
 *
 * WHY THESE NUMBERS ARE DECLARED, NOT LOOKED UP. They used to come from a
 * bundled third-party catalog snapshot, which went stale the moment a model
 * shipped and silently outranked values the operator had set. Probing the
 * configured fleet showed most endpoints publish nothing usable -- a /models
 * that 404s, one that omits its own configured model, one that 401s -- so a
 * provider fetch cannot be the general answer either. The operator's
 * declaration is authoritative; a provider fetch fills in what that provider
 * genuinely publishes; nothing else invents a value.
 *
 * ZERO MEANS DIFFERENT THINGS FOR PRICES AND CAPACITIES, deliberately. A
 * declared price of 0 is a real statement -- a free or subscription-priced seat
 * costs nothing per token -- and is distinguishable from silence by its
 * DECLARED bit. A capacity of 0 is not a capacity; there is no model with a
 * zero-token window, so 0 there reads as unknown. Both are carried the same way
 * on the wire; only the interpretation differs, and it is the declared bitmask
 * that makes either legible.
 *
 * Prices are integer MICRO-DOLLARS per million tokens, not doubles: the
 * envelope is deliberately free of host-endian and native-float fields so C, Go
 * and future module SDKs implement one contract independently (see
 * module_protocol.h). 3000000 is $3.00/Mtok.
 */
#ifndef AIMEE_PROVIDERS_MODULE_API_H
#define AIMEE_PROVIDERS_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* THE MODULE DECIDES; THE CALLER ACTS. Every module_adapter.c in this tree is a
 * pure function over a wire record -- their entire include set is the runtime
 * header, their own module_api.h, two local policy headers, and stdlib. None
 * opens a file, a socket or a database. The daemon owns agents.json, the
 * provider HTTP fetch and the db1 catalog cache; this module owns the RULES for
 * combining what they produce.
 *
 * So the operations are decisions, not CRUD. An earlier draft of this contract
 * had MODEL_GET / MODEL_SET / CATALOG_FETCH -- a persisted read, a persisted
 * write and an HTTP call -- none of which a pure handler can perform. Resolving
 * precedence and validating a declaration is exactly the rule-heavy logic that
 * was previously smeared across model_registry.c's five-deep fallback chain,
 * and it is testable in isolation precisely because it touches nothing.
 *
 * Event-kind block 43; blocks 23-31 and 35-42 are taken (see the other
 * module_api.h headers). One stage per event kind, numbered from 1. */
#define AIMEE_PROVIDERS_EVENT_RESOLVE  11009u
#define AIMEE_PROVIDERS_EVENT_VALIDATE 11010u

#define AIMEE_PROVIDERS_STAGE_RESOLVE  1u
#define AIMEE_PROVIDERS_STAGE_VALIDATE 2u

#define AIMEE_PROVIDERS_WIRE_VERSION   1u
#define AIMEE_PROVIDERS_REQUEST_MAGIC  0x51455250u /* "PREQ" */
#define AIMEE_PROVIDERS_RESPONSE_MAGIC 0x53455250u /* "PRES" */

#define AIMEE_PROVIDERS_NAME_MAX    32u  /* provider name */
#define AIMEE_PROVIDERS_MODEL_MAX   128u /* model id */
#define AIMEE_PROVIDERS_DISPLAY_MAX 64u  /* human label */

/* Which fields of a record the OPERATOR declared. Mirrors AGENT_DECL_* in
 * agent_types.h; kept as its own numbering because this is a wire format and
 * must not move when an internal enum is reordered. */
#define AIMEE_PROVIDERS_DECL_PRICE_IN       (1u << 0)
#define AIMEE_PROVIDERS_DECL_PRICE_OUT      (1u << 1)
#define AIMEE_PROVIDERS_DECL_PRICE_CACHED   (1u << 2)
#define AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW (1u << 3)
#define AIMEE_PROVIDERS_DECL_MAX_OUTPUT     (1u << 4)
#define AIMEE_PROVIDERS_DECL_CAPS           (1u << 5)

/* Where a returned value came from. A GUI must be able to show "you set this"
 * apart from "the provider reported this" apart from "nobody knows" -- showing a
 * fetched number as though the operator had chosen it is how the stale-catalog
 * problem stayed invisible for two months. */
#define AIMEE_PROVIDERS_SRC_UNKNOWN  0u
#define AIMEE_PROVIDERS_SRC_DECLARED 1u
#define AIMEE_PROVIDERS_SRC_FETCHED  2u

/* One model as configured and/or reported. Fixed layout, little-endian.
 *    0  provider   (NAME_MAX, NUL-padded)
 *   32  model      (MODEL_MAX, NUL-padded)
 *  160  display    (DISPLAY_MAX, NUL-padded)
 *  224  u32 context_window
 *  228  u32 max_output
 *  232  u32 caps                MODEL_CAP_* bitmask
 *  236  u32 declared            AIMEE_PROVIDERS_DECL_*
 *  240  u64 price_in_micro      micro-dollars per Mtok
 *  248  u64 price_out_micro
 *  256  u64 price_cached_micro
 *  264  u8  deprecated
 *  265  u8  context_source      AIMEE_PROVIDERS_SRC_*
 *  266  u8  max_output_source
 *  267  u8  price_source
 *  268  (end)
 */
#define AIMEE_PROVIDERS_OFF_PROVIDER       0u
#define AIMEE_PROVIDERS_OFF_MODEL          32u
#define AIMEE_PROVIDERS_OFF_DISPLAY        160u
#define AIMEE_PROVIDERS_OFF_CONTEXT        224u
#define AIMEE_PROVIDERS_OFF_MAX_OUTPUT     228u
#define AIMEE_PROVIDERS_OFF_CAPS           232u
#define AIMEE_PROVIDERS_OFF_DECLARED       236u
#define AIMEE_PROVIDERS_OFF_PRICE_IN       240u
#define AIMEE_PROVIDERS_OFF_PRICE_OUT      248u
#define AIMEE_PROVIDERS_OFF_PRICE_CACHED   256u
#define AIMEE_PROVIDERS_OFF_DEPRECATED     264u
#define AIMEE_PROVIDERS_OFF_CONTEXT_SRC    265u
#define AIMEE_PROVIDERS_OFF_MAX_OUTPUT_SRC 266u
#define AIMEE_PROVIDERS_OFF_PRICE_SRC      267u
#define AIMEE_PROVIDERS_RECORD_LEN         268u

/* The layout above is a WIRE format, so it is pinned here rather than described
 * in the comment alone. Each field must start where the previous one ends: an
 * offset edited without moving the ones after it silently reinterprets every
 * later field, and both sides would agree on the same wrong bytes. These fire at
 * compile time, in every translation unit that includes the contract. */
_Static_assert(AIMEE_PROVIDERS_OFF_MODEL == AIMEE_PROVIDERS_OFF_PROVIDER + AIMEE_PROVIDERS_NAME_MAX,
               "model follows provider");
_Static_assert(AIMEE_PROVIDERS_OFF_DISPLAY == AIMEE_PROVIDERS_OFF_MODEL + AIMEE_PROVIDERS_MODEL_MAX,
               "display follows model");
_Static_assert(AIMEE_PROVIDERS_OFF_CONTEXT ==
                   AIMEE_PROVIDERS_OFF_DISPLAY + AIMEE_PROVIDERS_DISPLAY_MAX,
               "context follows display");
_Static_assert(AIMEE_PROVIDERS_OFF_MAX_OUTPUT == AIMEE_PROVIDERS_OFF_CONTEXT + 4u, "u32 context");
_Static_assert(AIMEE_PROVIDERS_OFF_CAPS == AIMEE_PROVIDERS_OFF_MAX_OUTPUT + 4u, "u32 max_output");
_Static_assert(AIMEE_PROVIDERS_OFF_DECLARED == AIMEE_PROVIDERS_OFF_CAPS + 4u, "u32 caps");
_Static_assert(AIMEE_PROVIDERS_OFF_PRICE_IN == AIMEE_PROVIDERS_OFF_DECLARED + 4u, "u32 declared");
_Static_assert(AIMEE_PROVIDERS_OFF_PRICE_OUT == AIMEE_PROVIDERS_OFF_PRICE_IN + 8u, "u64 price_in");
_Static_assert(AIMEE_PROVIDERS_OFF_PRICE_CACHED == AIMEE_PROVIDERS_OFF_PRICE_OUT + 8u,
               "u64 price_out");
_Static_assert(AIMEE_PROVIDERS_OFF_DEPRECATED == AIMEE_PROVIDERS_OFF_PRICE_CACHED + 8u,
               "u64 price_cached");
_Static_assert(AIMEE_PROVIDERS_OFF_CONTEXT_SRC == AIMEE_PROVIDERS_OFF_DEPRECATED + 1u, "u8 depr");
_Static_assert(AIMEE_PROVIDERS_OFF_MAX_OUTPUT_SRC == AIMEE_PROVIDERS_OFF_CONTEXT_SRC + 1u,
               "u8 context_src");
_Static_assert(AIMEE_PROVIDERS_OFF_PRICE_SRC == AIMEE_PROVIDERS_OFF_MAX_OUTPUT_SRC + 1u,
               "u8 max_output_src");
_Static_assert(AIMEE_PROVIDERS_RECORD_LEN == AIMEE_PROVIDERS_OFF_PRICE_SRC + 1u,
               "record ends after price_src");

/* RESOLVE request: magic, version, then TWO records -- what the operator
 * declared, then what the provider reported. Either may be all-zero, which is
 * the ordinary case: most configured endpoints publish nothing, and a model
 * nobody has configured yet has no declaration. The caller supplies both
 * because it, not the module, is what can read config and talk to a provider.
 *
 * VALIDATE request: magic, version, then ONE record -- a proposed declaration,
 * before it is written anywhere. */
#define AIMEE_PROVIDERS_RESOLVE_REQUEST_LEN  (8u + 2u * AIMEE_PROVIDERS_RECORD_LEN)
#define AIMEE_PROVIDERS_VALIDATE_REQUEST_LEN (8u + AIMEE_PROVIDERS_RECORD_LEN)
#define AIMEE_PROVIDERS_OFF_DECLARED_RECORD  8u
#define AIMEE_PROVIDERS_OFF_FETCHED_RECORD   (8u + AIMEE_PROVIDERS_RECORD_LEN)

/* Response: magic, version, u32 status, u32 record_count, then records. Both
 * operations return exactly one -- the effective record (RESOLVE) or the
 * normalized proposal (VALIDATE). The count is explicit anyway so the envelope
 * does not have to change if a later stage returns a set. */
#define AIMEE_PROVIDERS_RESPONSE_HEADER_LEN 16u
#define AIMEE_PROVIDERS_RESPONSE_LEN (AIMEE_PROVIDERS_RESPONSE_HEADER_LEN + AIMEE_PROVIDERS_RECORD_LEN)

typedef enum
{
   AIMEE_PROVIDERS_OK = 0,
   AIMEE_PROVIDERS_ERR_MALFORMED = 1,
   /* The two records disagree about which model they describe. Resolving them
    * together would silently attribute one model's limits to another. */
   AIMEE_PROVIDERS_ERR_IDENTITY_MISMATCH = 2,
   /* A proposed declaration is not usable: an empty model id, or a capacity
    * that cannot be true. Reachability and "the provider published nothing"
    * are the CALLER's states, not this module's -- it never talks to anyone. */
   AIMEE_PROVIDERS_ERR_INVALID_DECLARATION = 3
} aimee_providers_status_t;

static inline void aimee_providers_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_providers_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline void aimee_providers_put_u64(uint8_t *p, uint64_t v)
{
   for (unsigned i = 0; i < 8; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint64_t aimee_providers_get_u64(const uint8_t *p)
{
   uint64_t v = 0;
   for (unsigned i = 0; i < 8; ++i)
      v |= (uint64_t)p[i] << (8u * i);
   return v;
}

/* NUL-pad a bounded string field. Truncation is REFUSED rather than silently
 * shortened: a truncated model id addresses a DIFFERENT model, and writing
 * declared limits against the wrong one is worse than failing the call. */
static inline int aimee_providers_put_str(uint8_t *p, size_t cap, const char *s)
{
   size_t n = s ? strlen(s) : 0;
   if (n >= cap)
      return -1;
   memset(p, 0, cap);
   if (n)
      memcpy(p, s, n);
   return 0;
}

static inline void aimee_providers_get_str(const uint8_t *p, size_t cap, char *out, size_t out_cap)
{
   size_t n = 0;
   while (n < cap && p[n])
      n++;
   if (n >= out_cap)
      n = out_cap ? out_cap - 1 : 0;
   if (n)
      memcpy(out, p, n);
   if (out_cap)
      out[n] = '\0';
}

#endif /* AIMEE_PROVIDERS_MODULE_API_H */
