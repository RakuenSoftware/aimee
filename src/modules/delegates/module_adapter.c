#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/delegates/module_api.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *canonical(const char *role)
{
   static const struct
   {
      const char *alias;
      const char *role;
   } aliases[] = {{"implement", "code"},        {"build", "code"},
                  {"reviewer", "review"},       {"verifier", "validate"},
                  {"test", "validate"},         {"check", "validate"},
                  {"evaluate", "validate"},     {"evaluate-optimize", "validate"},
                  {"inspect", "diagnose"},      {"research", "execute"},
                  {"enforce", "execute"},       {"recall", "search"},
                  {"synthesize", "summarize"},  {"rank-fuse", "reason"},
                  {"classify-score", "reason"}, {"planner", "plan"},
                  {"planning", "plan"},         {NULL, NULL}};
   for (size_t i = 0; aliases[i].alias; ++i)
      if (strcmp(role, aliases[i].alias) == 0)
         return aliases[i].role;
   return role;
}

/* Capability inference: what a prompt implies a model must be able to do. The
 * Go module states the same rule; test_process_module_handlers pins the two
 * together, because nothing in the build otherwise would. */
static int cap_contains_ci(const char *haystack, const char *needle)
{
   size_t nlen = strlen(needle);
   if (nlen == 0)
      return 1;
   for (const char *p = haystack; *p; ++p)
   {
      size_t i = 0;
      while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
         ++i;
      if (i == nlen)
         return 1;
   }
   return 0;
}

static int cap_contains_any(const char *prompt, const char *const *markers, size_t count)
{
   for (size_t i = 0; i < count; ++i)
      if (cap_contains_ci(prompt, markers[i]))
         return 1;
   return 0;
}

static void infer_capabilities(const char *prompt, int tools_enabled, uint32_t *required_out,
                               uint32_t *min_context_out)
{
   /* Markdown image syntax ("![") is deliberately not a trigger: it appears in
    * any doc or diff under review and never means an image must be decoded. */
   static const char *const vision[] = {".png", ".jpg",       ".jpeg",    ".webp",
                                        ".gif", "screenshot", "image_url"};
   static const char *const pdf[] = {".pdf", " pdf "};
   /* Only actual audio file formats. The bare word "audio" appears in prompts
    * that implement speech features in code and asks nothing of the model. */
   static const char *const audio[] = {".mp3", ".wav", ".m4a", ".ogg", ".flac", ".aac"};

   uint32_t required = tools_enabled ? AIMEE_DELEGATES_CAP_TOOLS : 0u;
   uint32_t min_context = 0;
   if (prompt && prompt[0])
   {
      if (cap_contains_any(prompt, vision, sizeof(vision) / sizeof(vision[0])))
         required |= AIMEE_DELEGATES_CAP_VISION;
      if (cap_contains_any(prompt, pdf, sizeof(pdf) / sizeof(pdf[0])))
         required |= AIMEE_DELEGATES_CAP_PDF;
      if (cap_contains_any(prompt, audio, sizeof(audio) / sizeof(audio[0])))
         required |= AIMEE_DELEGATES_CAP_AUDIO;
      /* Roughly one token per four characters; only a materially large prompt
       * constrains the window. */
      size_t estimated = strlen(prompt) / 4 + 1;
      if (estimated > 4096)
         min_context = (uint32_t)(estimated + 1024);
   }
   *required_out = required;
   *min_context_out = min_context;
}

static aimee_module_status_t handle_capabilities(const aimee_module_invocation_t *invocation,
                                                 const uint8_t *request_body, uint32_t request_len,
                                                 uint8_t *response_body, uint32_t response_capacity,
                                                 uint32_t *response_len)
{
   if (request_len < AIMEE_DELEGATES_CAP_HEADER_LEN ||
       response_capacity < AIMEE_DELEGATES_CAP_RESPONSE_LEN ||
       aimee_delegates_get_u32(request_body) != AIMEE_DELEGATES_CAP_REQUEST_MAGIC ||
       request_body[4] != AIMEE_DELEGATES_WIRE_VERSION || request_body[5] > 1u)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   uint32_t length = aimee_delegates_get_u32(request_body + 8);
   if (length > AIMEE_DELEGATES_CAP_PROMPT_MAX ||
       request_len != AIMEE_DELEGATES_CAP_HEADER_LEN + length)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   char *prompt = malloc((size_t)length + 1);
   if (!prompt)
      return AIMEE_MODULE_STATUS_INTERNAL;
   memcpy(prompt, request_body + AIMEE_DELEGATES_CAP_HEADER_LEN, length);
   prompt[length] = '\0';
   uint32_t required = 0, min_context = 0;
   infer_capabilities(prompt, request_body[5] == 1u, &required, &min_context);
   free(prompt);

   memset(response_body, 0, AIMEE_DELEGATES_CAP_RESPONSE_LEN);
   aimee_delegates_put_u32(response_body, AIMEE_DELEGATES_CAP_RESPONSE_MAGIC);
   aimee_delegates_put_u32(response_body + 4, required);
   aimee_delegates_put_u32(response_body + 8, min_context);
   *response_len = AIMEE_DELEGATES_CAP_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}

/* Chain depth. Stated in Go as well (server-go/modules/delegates/chain.go) and
 * pinned by test_process_module_handlers, because nothing in the build keeps
 * the two in step. */
static aimee_module_status_t handle_chain(const aimee_module_invocation_t *invocation,
                                          const uint8_t *request_body, uint32_t request_len,
                                          uint8_t *response_body, uint32_t response_capacity,
                                          uint32_t *response_len)
{
   if (request_len != AIMEE_DELEGATES_CHAIN_REQUEST_LEN ||
       response_capacity < AIMEE_DELEGATES_CHAIN_RESPONSE_LEN ||
       aimee_delegates_get_u32(request_body) != AIMEE_DELEGATES_CHAIN_REQUEST_MAGIC ||
       request_body[4] != AIMEE_DELEGATES_WIRE_VERSION)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   unsigned op = request_body[5];
   if (op != AIMEE_DELEGATES_CHAIN_OP_SHOULD_CLEAR && op != AIMEE_DELEGATES_CHAIN_OP_CHECK_DEPTH)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   /* A flag that is neither 0 nor 1 is not a boolean; refuse rather than coerce
    * it, since coercing would silently pick an answer. */
   for (unsigned i = 6; i < 10; ++i)
      if (request_body[i] > 1u)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   memset(response_body, 0, AIMEE_DELEGATES_CHAIN_RESPONSE_LEN);
   aimee_delegates_put_u32(response_body, AIMEE_DELEGATES_CHAIN_RESPONSE_MAGIC);
   if (op == AIMEE_DELEGATES_CHAIN_OP_SHOULD_CLEAR)
   {
      int has_depth = request_body[6] == 1u, has_parent = request_body[7] == 1u;
      int parent_known = request_body[8] == 1u, parent_active = request_body[9] == 1u;
      /* A depth with no parent marker is a leftover from a process that is
       * gone. A parent known to have exited is worse: it names an ancestor that
       * no longer exists. An UNKNOWN liveness must not clear -- discarding the
       * depth there would quietly raise the ceiling underneath it. */
      int clear = (has_depth && !has_parent) || (has_parent && parent_known && !parent_active);
      response_body[4] = clear ? 1u : 0u;
   }
   else
   {
      int32_t parent = (int32_t)aimee_delegates_get_u32(request_body + 12);
      int32_t max = (int32_t)aimee_delegates_get_u32(request_body + 16);
      int32_t current = parent + 1; /* the child's depth: refuse before running */
      response_body[4] = current <= max ? 1u : 0u;
      aimee_delegates_put_u32(response_body + 8, (uint32_t)current);
   }
   *response_len = AIMEE_DELEGATES_CHAIN_RESPONSE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}

aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                           const uint8_t *request_body, uint32_t request_len,
                                           uint8_t *response_body, uint32_t response_capacity,
                                           uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (invocation && response_len && invocation->stage_id == AIMEE_DELEGATES_STAGE_CHAIN)
      return handle_chain(invocation, request_body, request_len, response_body, response_capacity,
                          response_len);
   if (invocation && response_len && invocation->stage_id == AIMEE_DELEGATES_STAGE_CAPABILITIES)
      return handle_capabilities(invocation, request_body, request_len, response_body,
                                 response_capacity, response_len);
   char role[AIMEE_DELEGATES_ROLE_MAX + 1];
   if (!invocation || !response_len || invocation->stage_id != AIMEE_DELEGATES_STAGE_INVOKE ||
       response_capacity < AIMEE_DELEGATES_MESSAGE_LEN ||
       aimee_delegates_message_decode(request_body, request_len, AIMEE_DELEGATES_REQUEST_MAGIC,
                                      role, sizeof(role)) != 0)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   if (aimee_delegates_message_encode(AIMEE_DELEGATES_RESPONSE_MAGIC, canonical(role),
                                      response_body, response_capacity) != 0)
      return AIMEE_MODULE_STATUS_INTERNAL;
   *response_len = AIMEE_DELEGATES_MESSAGE_LEN;
   return AIMEE_MODULE_STATUS_OK;
}
