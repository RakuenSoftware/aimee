/* Provider/model configuration rules.
 *
 * Pure, like every other module_adapter.c here: no file, socket or database. The
 * daemon reads agents.json, talks to providers and owns the db1 catalog cache;
 * this decides which of the values they produce actually applies, and where it
 * came from.
 *
 * The precedence encoded here previously existed as four hand-written copies at
 * four call sites, each keying "did the operator set this" on `value > 0`, and
 * each subtly different. One copy, testable without a filesystem, is the point.
 */
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/providers/module_api.h>

#include <string.h>

static int record_str_eq(const uint8_t *a, const uint8_t *b, size_t cap)
{
   return memcmp(a, b, cap) == 0;
}

static int record_is_blank(const uint8_t *r)
{
   /* A record nobody filled in. Checked on the identity fields only: an
    * all-zero id is what the caller sends for "the operator declared nothing"
    * or "the provider reported nothing", which are both ordinary. */
   for (unsigned i = 0; i < AIMEE_PROVIDERS_MODEL_MAX; ++i)
      if (r[AIMEE_PROVIDERS_OFF_MODEL + i])
         return 0;
   return 1;
}

/* A capacity: declared wins when the operator gave a usable one, else whatever
 * the provider reported, else unknown.
 *
 * A declared 0 is NOT a declaration here, deliberately -- there is no model with
 * a zero-token window, so 0 reads as "unknown" and resolution continues. This is
 * the one place the declared-bit rule differs from prices, and conflating them
 * would make a stray 0 out-rank a real number the provider published. */
static uint32_t resolve_capacity(uint32_t declared_value, int declared_bit, uint32_t fetched_value,
                                 uint8_t *source_out)
{
   if (declared_bit && declared_value > 0)
   {
      *source_out = (uint8_t)AIMEE_PROVIDERS_SRC_DECLARED;
      return declared_value;
   }
   if (fetched_value > 0)
   {
      *source_out = (uint8_t)AIMEE_PROVIDERS_SRC_FETCHED;
      return fetched_value;
   }
   *source_out = (uint8_t)AIMEE_PROVIDERS_SRC_UNKNOWN;
   return 0;
}

static aimee_module_status_t do_resolve(const uint8_t *declared, const uint8_t *fetched,
                                        uint8_t *out)
{
   const int have_declared = !record_is_blank(declared);
   const int have_fetched = !record_is_blank(fetched);

   /* Resolving two records that name different models would attribute one
    * model's limits to another -- the exact failure this whole surface exists to
    * stop. Only compared when both sides actually carry an identity. */
   if (have_declared && have_fetched &&
       (!record_str_eq(declared + AIMEE_PROVIDERS_OFF_MODEL, fetched + AIMEE_PROVIDERS_OFF_MODEL,
                       AIMEE_PROVIDERS_MODEL_MAX) ||
        !record_str_eq(declared + AIMEE_PROVIDERS_OFF_PROVIDER,
                       fetched + AIMEE_PROVIDERS_OFF_PROVIDER, AIMEE_PROVIDERS_NAME_MAX)))
      return (aimee_module_status_t)AIMEE_PROVIDERS_ERR_IDENTITY_MISMATCH;

   memset(out, 0, AIMEE_PROVIDERS_RECORD_LEN);
   const uint8_t *identity = have_declared ? declared : fetched;
   memcpy(out + AIMEE_PROVIDERS_OFF_PROVIDER, identity + AIMEE_PROVIDERS_OFF_PROVIDER,
          AIMEE_PROVIDERS_NAME_MAX);
   memcpy(out + AIMEE_PROVIDERS_OFF_MODEL, identity + AIMEE_PROVIDERS_OFF_MODEL,
          AIMEE_PROVIDERS_MODEL_MAX);

   /* The provider's own label beats a local one: it is the name the vendor
    * publishes for the model, and an operator rarely wants to restate it. */
   const uint8_t *label = fetched[AIMEE_PROVIDERS_OFF_DISPLAY] ? fetched : declared;
   memcpy(out + AIMEE_PROVIDERS_OFF_DISPLAY, label + AIMEE_PROVIDERS_OFF_DISPLAY,
          AIMEE_PROVIDERS_DISPLAY_MAX);

   const uint32_t decl = aimee_providers_get_u32(declared + AIMEE_PROVIDERS_OFF_DECLARED);

   uint8_t src = 0;
   aimee_providers_put_u32(
       out + AIMEE_PROVIDERS_OFF_CONTEXT,
       resolve_capacity(aimee_providers_get_u32(declared + AIMEE_PROVIDERS_OFF_CONTEXT),
                        (decl & AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW) != 0,
                        aimee_providers_get_u32(fetched + AIMEE_PROVIDERS_OFF_CONTEXT), &src));
   out[AIMEE_PROVIDERS_OFF_CONTEXT_SRC] = src;

   aimee_providers_put_u32(
       out + AIMEE_PROVIDERS_OFF_MAX_OUTPUT,
       resolve_capacity(aimee_providers_get_u32(declared + AIMEE_PROVIDERS_OFF_MAX_OUTPUT),
                        (decl & AIMEE_PROVIDERS_DECL_MAX_OUTPUT) != 0,
                        aimee_providers_get_u32(fetched + AIMEE_PROVIDERS_OFF_MAX_OUTPUT), &src));
   out[AIMEE_PROVIDERS_OFF_MAX_OUTPUT_SRC] = src;

   /* Capabilities: a declared set replaces the reported one rather than merging.
    * Merging would make a capability impossible to REVOKE -- an operator who
    * knows a seat cannot really do tool calls could never say so. */
   if (decl & AIMEE_PROVIDERS_DECL_CAPS)
      aimee_providers_put_u32(out + AIMEE_PROVIDERS_OFF_CAPS,
                              aimee_providers_get_u32(declared + AIMEE_PROVIDERS_OFF_CAPS));
   else
      aimee_providers_put_u32(out + AIMEE_PROVIDERS_OFF_CAPS,
                              aimee_providers_get_u32(fetched + AIMEE_PROVIDERS_OFF_CAPS));

   /* Prices: the declared value wins WHATEVER it is, including 0. That is the
    * asymmetry with capacities -- 0 here means "this seat costs nothing per
    * token", a real and common statement for a subscription or flat-rate seat.
    * No provider publishes prices, so an undeclared price is simply unknown. */
   static const struct
   {
      unsigned off;
      uint32_t bit;
   } price_fields[] = {
       {AIMEE_PROVIDERS_OFF_PRICE_IN, AIMEE_PROVIDERS_DECL_PRICE_IN},
       {AIMEE_PROVIDERS_OFF_PRICE_OUT, AIMEE_PROVIDERS_DECL_PRICE_OUT},
       {AIMEE_PROVIDERS_OFF_PRICE_CACHED, AIMEE_PROVIDERS_DECL_PRICE_CACHED},
   };
   uint8_t price_src = (uint8_t)AIMEE_PROVIDERS_SRC_UNKNOWN;
   for (unsigned i = 0; i < sizeof price_fields / sizeof price_fields[0]; ++i)
   {
      if (decl & price_fields[i].bit)
      {
         aimee_providers_put_u64(out + price_fields[i].off,
                                 aimee_providers_get_u64(declared + price_fields[i].off));
         price_src = (uint8_t)AIMEE_PROVIDERS_SRC_DECLARED;
      }
   }
   out[AIMEE_PROVIDERS_OFF_PRICE_SRC] = price_src;

   /* Deprecation is the union, not a precedence. Either side saying a model is
    * retired is enough, because routing AWAY from it is the recoverable
    * direction: the cost of being wrong is spending more, not calling a model
    * that no longer exists. */
   out[AIMEE_PROVIDERS_OFF_DEPRECATED] =
       (uint8_t)(declared[AIMEE_PROVIDERS_OFF_DEPRECATED] || fetched[AIMEE_PROVIDERS_OFF_DEPRECATED]);

   /* The declared mask rides along so a caller can still tell which fields the
    * operator stated, after resolution has folded in the provider's answers. */
   aimee_providers_put_u32(out + AIMEE_PROVIDERS_OFF_DECLARED, decl);
   return AIMEE_MODULE_STATUS_OK;
}

static aimee_module_status_t do_validate(const uint8_t *proposal, uint8_t *out)
{
   if (record_is_blank(proposal) || !proposal[AIMEE_PROVIDERS_OFF_PROVIDER])
      return (aimee_module_status_t)AIMEE_PROVIDERS_ERR_INVALID_DECLARATION;

   memcpy(out, proposal, AIMEE_PROVIDERS_RECORD_LEN);
   uint32_t decl = aimee_providers_get_u32(out + AIMEE_PROVIDERS_OFF_DECLARED);

   /* NORMALIZE rather than reject a zero capacity. An operator clearing a field
    * in a form sends 0; treating that as an error would make "I no longer want
    * to state this" impossible to express. Clearing the bit is exactly that
    * statement, and it keeps the invariant the resolver relies on: a set
    * capacity bit always carries a usable number. */
   if ((decl & AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW) &&
       aimee_providers_get_u32(out + AIMEE_PROVIDERS_OFF_CONTEXT) == 0)
      decl &= ~(uint32_t)AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW;
   if ((decl & AIMEE_PROVIDERS_DECL_MAX_OUTPUT) &&
       aimee_providers_get_u32(out + AIMEE_PROVIDERS_OFF_MAX_OUTPUT) == 0)
      decl &= ~(uint32_t)AIMEE_PROVIDERS_DECL_MAX_OUTPUT;

   /* A declared max_output above the context window cannot be true: the window
    * bounds prompt AND completion together. Refused rather than normalized --
    * silently shrinking a number the operator typed would hide the mistake. */
   uint32_t ctx = aimee_providers_get_u32(out + AIMEE_PROVIDERS_OFF_CONTEXT);
   uint32_t out_cap = aimee_providers_get_u32(out + AIMEE_PROVIDERS_OFF_MAX_OUTPUT);
   if ((decl & AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW) && (decl & AIMEE_PROVIDERS_DECL_MAX_OUTPUT) &&
       out_cap > ctx)
      return (aimee_module_status_t)AIMEE_PROVIDERS_ERR_INVALID_DECLARATION;

   aimee_providers_put_u32(out + AIMEE_PROVIDERS_OFF_DECLARED, decl);
   /* A validated proposal describes what the operator stated; nothing here was
    * fetched, so the source fields say so rather than inheriting stale bytes. */
   out[AIMEE_PROVIDERS_OFF_CONTEXT_SRC] = (uint8_t)((decl & AIMEE_PROVIDERS_DECL_CONTEXT_WINDOW)
                                                        ? AIMEE_PROVIDERS_SRC_DECLARED
                                                        : AIMEE_PROVIDERS_SRC_UNKNOWN);
   out[AIMEE_PROVIDERS_OFF_MAX_OUTPUT_SRC] = (uint8_t)((decl & AIMEE_PROVIDERS_DECL_MAX_OUTPUT)
                                                           ? AIMEE_PROVIDERS_SRC_DECLARED
                                                           : AIMEE_PROVIDERS_SRC_UNKNOWN);
   const uint32_t any_price = AIMEE_PROVIDERS_DECL_PRICE_IN | AIMEE_PROVIDERS_DECL_PRICE_OUT |
                              AIMEE_PROVIDERS_DECL_PRICE_CACHED;
   out[AIMEE_PROVIDERS_OFF_PRICE_SRC] =
       (uint8_t)((decl & any_price) ? AIMEE_PROVIDERS_SRC_DECLARED : AIMEE_PROVIDERS_SRC_UNKNOWN);
   return AIMEE_MODULE_STATUS_OK;
}

static void write_response(uint8_t *response_body, uint32_t status, const uint8_t *record,
                           uint32_t *response_len)
{
   aimee_providers_put_u32(response_body, AIMEE_PROVIDERS_RESPONSE_MAGIC);
   aimee_providers_put_u32(response_body + 4, AIMEE_PROVIDERS_WIRE_VERSION);
   aimee_providers_put_u32(response_body + 8, status);
   aimee_providers_put_u32(response_body + 12, record ? 1u : 0u);
   if (record)
      memcpy(response_body + AIMEE_PROVIDERS_RESPONSE_HEADER_LEN, record,
             AIMEE_PROVIDERS_RECORD_LEN);
   else
      memset(response_body + AIMEE_PROVIDERS_RESPONSE_HEADER_LEN, 0, AIMEE_PROVIDERS_RECORD_LEN);
   *response_len = AIMEE_PROVIDERS_RESPONSE_LEN;
}

static int header_ok(const uint8_t *request_body, uint32_t request_len, uint32_t expect_len)
{
   return request_body && request_len == expect_len &&
          aimee_providers_get_u32(request_body) == AIMEE_PROVIDERS_REQUEST_MAGIC &&
          aimee_providers_get_u32(request_body + 4) == AIMEE_PROVIDERS_WIRE_VERSION;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len || !response_body ||
       response_capacity < AIMEE_PROVIDERS_RESPONSE_LEN)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;

   uint8_t record[AIMEE_PROVIDERS_RECORD_LEN];

   if (invocation->stage_id == AIMEE_PROVIDERS_STAGE_RESOLVE)
   {
      if (!header_ok(request_body, request_len, AIMEE_PROVIDERS_RESOLVE_REQUEST_LEN))
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      aimee_module_status_t rc = do_resolve(request_body + AIMEE_PROVIDERS_OFF_DECLARED_RECORD,
                                            request_body + AIMEE_PROVIDERS_OFF_FETCHED_RECORD,
                                            record);
      /* A rule failure is a RESULT, not a transport error: the caller needs the
       * status to show the operator, so it rides the response envelope. */
      if (rc != AIMEE_MODULE_STATUS_OK)
      {
         write_response(response_body, (uint32_t)rc, NULL, response_len);
         return AIMEE_MODULE_STATUS_OK;
      }
      write_response(response_body, AIMEE_PROVIDERS_OK, record, response_len);
      return AIMEE_MODULE_STATUS_OK;
   }

   if (invocation->stage_id == AIMEE_PROVIDERS_STAGE_VALIDATE)
   {
      if (!header_ok(request_body, request_len, AIMEE_PROVIDERS_VALIDATE_REQUEST_LEN))
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      if (aimee_module_invocation_cancelled(invocation))
         return AIMEE_MODULE_STATUS_CANCELLED;
      aimee_module_status_t rc = do_validate(request_body + 8, record);
      if (rc != AIMEE_MODULE_STATUS_OK)
      {
         write_response(response_body, (uint32_t)rc, NULL, response_len);
         return AIMEE_MODULE_STATUS_OK;
      }
      write_response(response_body, AIMEE_PROVIDERS_OK, record, response_len);
      return AIMEE_MODULE_STATUS_OK;
   }

   return AIMEE_MODULE_STATUS_INVALID_REQUEST;
}
