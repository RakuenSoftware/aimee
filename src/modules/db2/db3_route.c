#include <aimee/db2/db3_route.h>

#include <math.h>
#include <string.h>

#define SEARCH_REQUEST_MAGIC  AIMEE_DB3_SEARCH_REQUEST_MAGIC
#define SEARCH_REPLY_MAGIC    AIMEE_DB3_SEARCH_REPLY_MAGIC
#define APPLY_MAGIC           AIMEE_DB3_APPLY_MAGIC
#define WIRE_VERSION          AIMEE_DB3_WIRE_VERSION
#define SEARCH_REQUEST_HEADER AIMEE_DB3_SEARCH_REQUEST_HEADER
#define SEARCH_REPLY_HEADER   AIMEE_DB3_SEARCH_REPLY_HEADER
#define APPLY_HEADER          AIMEE_DB3_APPLY_HEADER
#define APPLY_V2_HEADER       AIMEE_DB3_APPLY_V2_HEADER
#define APPLY_V2_VERSION      AIMEE_DB3_APPLY_V2_VERSION
#define LABEL_HEADER          AIMEE_DB3_LABEL_HEADER

_Static_assert(sizeof(float) == 4, "DB3 wire requires 32-bit float");
_Static_assert(sizeof(double) == 8, "DB3 wire requires 64-bit double");
_Static_assert(AIMEE_DB3_MAX_DIM <= UINT16_MAX, "DB3 dimension must fit its wire field");
_Static_assert(AIMEE_DB3_MAX_TOP_K <= UINT16_MAX, "DB3 top-K must fit its wire field");
_Static_assert(AIMEE_DB3_MAX_LABELS <= UINT16_MAX, "DB3 label count must fit its wire field");
_Static_assert(AIMEE_DB3_MAX_LABEL_BYTES <= UINT16_MAX, "DB3 label bytes must fit its wire field");
_Static_assert(sizeof(aimee_db3_apply_t) <= 24u * 1024u,
               "DB3 apply stack value exceeds its explicit budget");

static void put_u16(uint8_t *out, uint16_t value)
{
   out[0] = (uint8_t)value;
   out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      out[i] = (uint8_t)(value >> (i * 8));
}

static void put_u64(uint8_t *out, uint64_t value)
{
   for (unsigned i = 0; i < 8; ++i)
      out[i] = (uint8_t)(value >> (i * 8));
}

static uint16_t get_u16(const uint8_t *in)
{
   return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)in[i] << (i * 8);
   return value;
}

static uint64_t get_u64(const uint8_t *in)
{
   uint64_t value = 0;
   for (unsigned i = 0; i < 8; ++i)
      value |= (uint64_t)in[i] << (i * 8);
   return value;
}

static size_t text_length(const char *text, size_t capacity)
{
   const char *end = memchr(text, '\0', capacity);
   return end ? (size_t)(end - text) : SIZE_MAX;
}

static int text_valid(const char *text, size_t capacity, int allow_empty)
{
   size_t length = text_length(text, capacity);
   if (length == SIZE_MAX || (!allow_empty && length == 0))
      return 0;
   for (size_t i = 0; i < length; ++i)
   {
      unsigned char byte = (unsigned char)text[i];
      if (byte < 0x21u || byte > 0x7eu)
         return 0;
   }
   return 1;
}

static int vectors_valid(const float *vector, uint32_t dimension)
{
   /* A vector the caller never supplied is not a vector of zeroes. Now that the
    * memory is theirs, a missing one has to be refused here rather than read. */
   if (dimension > 0 && !vector)
      return 0;
   for (uint32_t i = 0; i < dimension; ++i)
      if (!isfinite(vector[i]))
         return 0;
   return 1;
}

static int label_key_valid(const char *key)
{
   size_t length = text_length(key, AIMEE_DB3_MAX_LABEL_KEY);
   if (length == 0 || length == SIZE_MAX || key[0] < 'a' || key[0] > 'z')
      return 0;
   for (size_t i = 1; i < length; ++i)
   {
      unsigned char byte = (unsigned char)key[i];
      if ((byte < 'a' || byte > 'z') && (byte < '0' || byte > '9') && byte != '_' && byte != '.' &&
          byte != '-')
         return 0;
   }
   return 1;
}

static int label_value_valid(const char *value)
{
   size_t length = text_length(value, AIMEE_DB3_MAX_LABEL_VALUE);
   if (length == SIZE_MAX)
      return 0;
   for (size_t i = 0; i < length; ++i)
   {
      unsigned char byte = (unsigned char)value[i];
      if (byte < 0x20u || byte > 0x7eu)
         return 0;
   }
   return 1;
}

static int labels_size(const aimee_db3_apply_t *apply, size_t *size)
{
   if (!apply || !size || apply->label_count > AIMEE_DB3_MAX_LABELS)
      return -1;
   *size = 0;
   for (uint32_t i = 0; i < apply->label_count; ++i)
   {
      const aimee_db3_exact_label_t *label = &apply->labels[i];
      if (!label_key_valid(label->key) || !label_value_valid(label->value) ||
          (i > 0 && strcmp(apply->labels[i - 1].key, label->key) >= 0))
         return -1;
      size_t key = text_length(label->key, sizeof(label->key));
      size_t value = text_length(label->value, sizeof(label->value));
      if (key > SIZE_MAX - LABEL_HEADER || value > SIZE_MAX - LABEL_HEADER - key)
         return -1;
      size_t entry = LABEL_HEADER + key + value;
      if (entry > AIMEE_DB3_MAX_LABEL_BYTES - *size)
         return -1;
      *size += entry;
   }
   return 0;
}

static int copy_scope(char *destination, size_t capacity, const char *value)
{
   if (!value || !value[0])
   {
      destination[0] = '\0';
      return 0;
   }
   size_t length = strlen(value);
   /* Refused rather than truncated: a workspace cut short is a different
    * workspace, and it would filter to rows nobody asked for. */
   if (length >= capacity)
      return -1;
   memcpy(destination, value, length + 1);
   return 0;
}

int aimee_db3_search_filters_expressible(const aimee_db3_search_filters_t *filters)
{
   if (!filters)
      return 0;
   /* Every one of these is a filter DB3 v1 has no field for. Emptiness is the
    * only acceptable value; anything else means the caller is asking for
    * something this wire cannot ask. */
   if (filters->exclude_project && filters->exclude_project[0])
      return 0;
   if (filters->kind_count > 0 || filters->kinds)
      return 0;
   if (filters->rank_column && filters->rank_column[0])
      return 0;
   if (filters->label_count > 0 || filters->label_keys || filters->label_values)
      return 0;
   return 1;
}

int aimee_db3_search_request_build(const aimee_db3_search_filters_t *filters, uint64_t request_id,
                                   uint64_t required_generation, const float *vector,
                                   uint32_t dimension, uint32_t top_k,
                                   aimee_db3_search_request_t *request)
{
   if (!filters || !request || !vector)
      return -1;
   /* Checked BEFORE anything is written, so a refusal leaves no half-built
    * request for a caller to use by mistake. */
   if (!aimee_db3_search_filters_expressible(filters))
      return -1;

   aimee_db3_search_request_t built = {0};
   if (copy_scope(built.workspace, sizeof(built.workspace), filters->workspace) != 0 ||
       copy_scope(built.project, sizeof(built.project), filters->project) != 0 ||
       copy_scope(built.record_type, sizeof(built.record_type), filters->record_type) != 0)
      return -1;

   built.request_id = request_id;
   built.required_generation = required_generation;
   built.dimension = dimension;
   built.top_k = top_k;
   built.vector = vector;
   if (aimee_db3_search_request_validate(&built) != 0)
      return -1;
   *request = built;
   return 0;
}

int aimee_db3_search_request_validate(const aimee_db3_search_request_t *request)
{
   if (!request || request->request_id == 0 || request->required_generation == 0 ||
       request->dimension == 0 || request->dimension > AIMEE_DB3_MAX_DIM || request->top_k == 0 ||
       request->top_k > AIMEE_DB3_MAX_TOP_K ||
       !text_valid(request->workspace, sizeof(request->workspace), 1) ||
       !text_valid(request->project, sizeof(request->project), 1) ||
       (!request->workspace[0] && !request->project[0]) ||
       !text_valid(request->record_type, sizeof(request->record_type), 0) ||
       !vectors_valid(request->vector, request->dimension))
      return -1;
   return 0;
}

int aimee_db3_search_reply_validate(const aimee_db3_search_request_t *request,
                                    const aimee_db3_search_reply_t *reply)
{
   if (!request || !reply || reply->request_id != request->request_id ||
       reply->generation != request->required_generation || reply->count > request->top_k ||
       reply->count > AIMEE_DB3_MAX_TOP_K)
      return -1;
   for (uint32_t i = 0; i < reply->count; ++i)
   {
      if (reply->candidates[i].point_id <= 0 || !isfinite(reply->candidates[i].score))
         return -1;
      for (uint32_t j = 0; j < i; ++j)
         if (reply->candidates[j].point_id == reply->candidates[i].point_id)
            return -1;
   }
   return 0;
}

int aimee_db3_apply_validate(const aimee_db3_apply_t *apply)
{
   size_t ignored = 0;
   if (!apply || apply->operation_id == 0 || apply->generation == 0 || apply->point_id <= 0 ||
       !text_valid(apply->collection, sizeof(apply->collection), 0) ||
       labels_size(apply, &ignored) != 0)
      return -1;
   if (apply->kind == AIMEE_DB3_APPLY_UPSERT)
   {
      if (apply->dimension == 0 || apply->dimension > AIMEE_DB3_MAX_DIM ||
          !vectors_valid(apply->vector, apply->dimension))
         return -1;
   }
   else if ((apply->kind != AIMEE_DB3_APPLY_DELETE && apply->kind != AIMEE_DB3_APPLY_TOMBSTONE) ||
            apply->dimension != 0 || apply->label_count != 0)
      return -1;
   return 0;
}

static int checked_total(size_t header, size_t a, size_t b, size_t c, size_t item_size,
                         size_t item_count, size_t *total)
{
   if (!total || a > SIZE_MAX - header)
      return -1;
   size_t fixed = header + a;
   if (b > SIZE_MAX - fixed)
      return -1;
   fixed += b;
   if (c > SIZE_MAX - fixed)
      return -1;
   fixed += c;
   if (item_size == 0 || item_count > (SIZE_MAX - fixed) / item_size)
      return -1;
   *total = fixed + item_size * item_count;
   return 0;
}

int aimee_db3_search_request_encode(const aimee_db3_search_request_t *request, uint8_t *output,
                                    size_t capacity, size_t *length)
{
   if (length)
      *length = 0;
   if (!output || !length || aimee_db3_search_request_validate(request) != 0)
      return -1;
   size_t workspace_len = text_length(request->workspace, sizeof(request->workspace));
   size_t project_len = text_length(request->project, sizeof(request->project));
   size_t record_len = text_length(request->record_type, sizeof(request->record_type));
   size_t total = 0;
   if (checked_total(SEARCH_REQUEST_HEADER, workspace_len, project_len, record_len, sizeof(float),
                     request->dimension, &total) != 0 ||
       total > capacity || workspace_len > UINT16_MAX || project_len > UINT16_MAX ||
       record_len > UINT16_MAX || request->dimension > UINT16_MAX || request->top_k > UINT16_MAX)
      return -1;
   memset(output, 0, SEARCH_REQUEST_HEADER);
   put_u32(output, SEARCH_REQUEST_MAGIC);
   put_u16(output + 4, WIRE_VERSION);
   put_u16(output + 6, SEARCH_REQUEST_HEADER);
   put_u64(output + 8, request->request_id);
   put_u64(output + 16, request->required_generation);
   put_u16(output + 24, (uint16_t)workspace_len);
   put_u16(output + 26, (uint16_t)project_len);
   put_u16(output + 28, (uint16_t)record_len);
   put_u16(output + 30, (uint16_t)request->dimension);
   put_u16(output + 32, (uint16_t)request->top_k);
   size_t offset = SEARCH_REQUEST_HEADER;
   memcpy(output + offset, request->workspace, workspace_len);
   offset += workspace_len;
   memcpy(output + offset, request->project, project_len);
   offset += project_len;
   memcpy(output + offset, request->record_type, record_len);
   offset += record_len;
   for (uint32_t i = 0; i < request->dimension; ++i)
   {
      uint32_t bits = 0;
      memcpy(&bits, &request->vector[i], sizeof(bits));
      put_u32(output + offset, bits);
      offset += sizeof(bits);
   }
   *length = total;
   return 0;
}

int aimee_db3_search_request_decode(const uint8_t *input, size_t length,
                                    aimee_db3_search_request_t *request, float *vector_out,
                                    size_t vector_capacity)
{
   if (!input || !request || !vector_out || length < SEARCH_REQUEST_HEADER ||
       get_u32(input) != SEARCH_REQUEST_MAGIC || get_u16(input + 4) != WIRE_VERSION ||
       get_u16(input + 6) != SEARCH_REQUEST_HEADER || get_u16(input + 34) != 0)
      return -1;
   uint16_t workspace_len = get_u16(input + 24), project_len = get_u16(input + 26);
   uint16_t record_len = get_u16(input + 28), dimension = get_u16(input + 30);
   uint16_t top_k = get_u16(input + 32);
   if (workspace_len >= AIMEE_DB3_MAX_SCOPE || project_len >= AIMEE_DB3_MAX_SCOPE ||
       record_len == 0 || record_len >= AIMEE_DB3_MAX_RECORD_TYPE || dimension == 0 ||
       dimension > AIMEE_DB3_MAX_DIM || dimension > vector_capacity || top_k == 0 ||
       top_k > AIMEE_DB3_MAX_TOP_K)
      return -1;
   size_t total = 0;
   if (checked_total(SEARCH_REQUEST_HEADER, workspace_len, project_len, record_len, sizeof(float),
                     dimension, &total) != 0 ||
       total != length)
      return -1;
   memset(request, 0, sizeof(*request));
   request->request_id = get_u64(input + 8);
   request->required_generation = get_u64(input + 16);
   request->dimension = dimension;
   request->top_k = top_k;
   size_t offset = SEARCH_REQUEST_HEADER;
   memcpy(request->workspace, input + offset, workspace_len);
   offset += workspace_len;
   memcpy(request->project, input + offset, project_len);
   offset += project_len;
   memcpy(request->record_type, input + offset, record_len);
   offset += record_len;
   for (uint32_t i = 0; i < dimension; ++i)
   {
      uint32_t bits = get_u32(input + offset);
      memcpy(&vector_out[i], &bits, sizeof(bits));
      offset += sizeof(bits);
   }
   request->vector = vector_out;
   return aimee_db3_search_request_validate(request);
}

int aimee_db3_search_reply_encode(const aimee_db3_search_reply_t *reply, uint8_t *output,
                                  size_t capacity, size_t *length)
{
   if (length)
      *length = 0;
   if (!reply || !output || !length || reply->request_id == 0 || reply->generation == 0 ||
       reply->count > AIMEE_DB3_MAX_TOP_K)
      return -1;
   size_t total = SEARCH_REPLY_HEADER + (size_t)reply->count * 16u;
   if (total > capacity)
      return -1;
   memset(output, 0, SEARCH_REPLY_HEADER);
   put_u32(output, SEARCH_REPLY_MAGIC);
   put_u16(output + 4, WIRE_VERSION);
   put_u16(output + 6, SEARCH_REPLY_HEADER);
   put_u64(output + 8, reply->request_id);
   put_u64(output + 16, reply->generation);
   put_u32(output + 24, reply->count);
   size_t offset = SEARCH_REPLY_HEADER;
   for (uint32_t i = 0; i < reply->count; ++i)
   {
      uint64_t score_bits = 0;
      if (reply->candidates[i].point_id <= 0 || !isfinite(reply->candidates[i].score))
         return -1;
      for (uint32_t j = 0; j < i; ++j)
         if (reply->candidates[j].point_id == reply->candidates[i].point_id)
            return -1;
      put_u64(output + offset, (uint64_t)reply->candidates[i].point_id);
      memcpy(&score_bits, &reply->candidates[i].score, sizeof(score_bits));
      put_u64(output + offset + 8, score_bits);
      offset += 16;
   }
   *length = total;
   return 0;
}

int aimee_db3_search_reply_decode(const uint8_t *input, size_t length,
                                  aimee_db3_search_reply_t *reply)
{
   if (!input || !reply || length < SEARCH_REPLY_HEADER || get_u32(input) != SEARCH_REPLY_MAGIC ||
       get_u16(input + 4) != WIRE_VERSION || get_u16(input + 6) != SEARCH_REPLY_HEADER)
      return -1;
   uint32_t count = get_u32(input + 24);
   if (count > AIMEE_DB3_MAX_TOP_K || length != SEARCH_REPLY_HEADER + (size_t)count * 16u)
      return -1;
   memset(reply, 0, sizeof(*reply));
   reply->request_id = get_u64(input + 8);
   reply->generation = get_u64(input + 16);
   reply->count = count;
   if (reply->request_id == 0 || reply->generation == 0)
      return -1;
   size_t offset = SEARCH_REPLY_HEADER;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint64_t point_bits = get_u64(input + offset);
      uint64_t score_bits = get_u64(input + offset + 8);
      if (point_bits == 0 || point_bits > INT64_MAX)
         return -1;
      reply->candidates[i].point_id = (int64_t)point_bits;
      memcpy(&reply->candidates[i].score, &score_bits, sizeof(score_bits));
      if (reply->candidates[i].point_id <= 0 || !isfinite(reply->candidates[i].score))
         return -1;
      for (uint32_t j = 0; j < i; ++j)
         if (reply->candidates[j].point_id == reply->candidates[i].point_id)
            return -1;
      offset += 16;
   }
   return 0;
}

int aimee_db3_apply_encode(const aimee_db3_apply_t *apply, uint8_t *output, size_t capacity,
                           size_t *length)
{
   if (length)
      *length = 0;
   if (!output || !length || aimee_db3_apply_validate(apply) != 0)
      return -1;
   size_t collection_len = text_length(apply->collection, sizeof(apply->collection));
   size_t label_bytes = 0;
   if (labels_size(apply, &label_bytes) != 0)
      return -1;
   size_t header = apply->label_count ? APPLY_V2_HEADER : APPLY_HEADER;
   size_t total = 0;
   if (checked_total(header, collection_len, label_bytes, 0, sizeof(float), apply->dimension,
                     &total) != 0 ||
       total > capacity || collection_len > UINT16_MAX || apply->dimension > UINT16_MAX)
      return -1;
   memset(output, 0, header);
   put_u32(output, APPLY_MAGIC);
   put_u16(output + 4, apply->label_count ? APPLY_V2_VERSION : WIRE_VERSION);
   output[6] = (uint8_t)apply->kind;
   put_u64(output + 8, apply->operation_id);
   put_u64(output + 16, apply->generation);
   put_u64(output + 24, (uint64_t)apply->point_id);
   put_u16(output + 32, (uint16_t)collection_len);
   put_u16(output + 34, (uint16_t)apply->dimension);
   if (apply->label_count)
   {
      put_u16(output + 36, (uint16_t)apply->label_count);
      put_u16(output + 38, (uint16_t)label_bytes);
   }
   size_t offset = header;
   memcpy(output + offset, apply->collection, collection_len);
   offset += collection_len;
   for (uint32_t i = 0; i < apply->dimension; ++i)
   {
      uint32_t bits = 0;
      memcpy(&bits, &apply->vector[i], sizeof(bits));
      put_u32(output + offset, bits);
      offset += sizeof(bits);
   }
   for (uint32_t i = 0; i < apply->label_count; ++i)
   {
      size_t key = text_length(apply->labels[i].key, sizeof(apply->labels[i].key));
      size_t value = text_length(apply->labels[i].value, sizeof(apply->labels[i].value));
      put_u16(output + offset, (uint16_t)key);
      put_u16(output + offset + 2, (uint16_t)value);
      offset += LABEL_HEADER;
      memcpy(output + offset, apply->labels[i].key, key);
      offset += key;
      memcpy(output + offset, apply->labels[i].value, value);
      offset += value;
   }
   *length = total;
   return 0;
}

int aimee_db3_apply_decode(const uint8_t *input, size_t length, aimee_db3_apply_t *apply,
                           float *vector_out, size_t vector_capacity)
{
   if (!input || !apply || length < APPLY_HEADER || get_u32(input) != APPLY_MAGIC || input[7] != 0)
      return -1;
   uint16_t version = get_u16(input + 4);
   size_t header = APPLY_HEADER;
   uint16_t label_count = 0, label_bytes = 0;
   if (version == APPLY_V2_VERSION)
   {
      if (length < APPLY_V2_HEADER)
         return -1;
      header = APPLY_V2_HEADER;
      label_count = get_u16(input + 36);
      label_bytes = get_u16(input + 38);
      if (label_count == 0 || label_count > AIMEE_DB3_MAX_LABELS ||
          label_bytes > AIMEE_DB3_MAX_LABEL_BYTES)
         return -1;
   }
   else if (version != WIRE_VERSION)
      return -1;
   uint16_t collection_len = get_u16(input + 32), dimension = get_u16(input + 34);
   if (collection_len == 0 || collection_len >= AIMEE_DB3_MAX_COLLECTION ||
       dimension > AIMEE_DB3_MAX_DIM || (dimension > 0 && (!vector_out || dimension > vector_capacity)) ||
       length != header + (size_t)collection_len + (size_t)dimension * sizeof(float) + label_bytes)
      return -1;
   memset(apply, 0, sizeof(*apply));
   apply->kind = (aimee_db3_apply_kind_t)input[6];
   apply->operation_id = get_u64(input + 8);
   apply->generation = get_u64(input + 16);
   uint64_t point_bits = get_u64(input + 24);
   if (point_bits == 0 || point_bits > INT64_MAX)
      return -1;
   apply->point_id = (int64_t)point_bits;
   apply->dimension = dimension;
   apply->label_count = label_count;
   size_t offset = header;
   memcpy(apply->collection, input + offset, collection_len);
   offset += collection_len;
   for (uint32_t i = 0; i < dimension; ++i)
   {
      uint32_t bits = get_u32(input + offset);
      memcpy(&vector_out[i], &bits, sizeof(bits));
      offset += sizeof(bits);
   }
   apply->vector = dimension ? vector_out : NULL;
   size_t labels_end = offset + label_bytes;
   for (uint32_t i = 0; i < label_count; ++i)
   {
      if (offset > labels_end || LABEL_HEADER > labels_end - offset)
         return -1;
      uint16_t key = get_u16(input + offset), value = get_u16(input + offset + 2);
      offset += LABEL_HEADER;
      if (key == 0 || key >= AIMEE_DB3_MAX_LABEL_KEY || value >= AIMEE_DB3_MAX_LABEL_VALUE ||
          (size_t)key + value > labels_end - offset)
         return -1;
      memcpy(apply->labels[i].key, input + offset, key);
      apply->labels[i].key[key] = '\0';
      offset += key;
      memcpy(apply->labels[i].value, input + offset, value);
      apply->labels[i].value[value] = '\0';
      offset += value;
   }
   if (offset != labels_end)
      return -1;
   return aimee_db3_apply_validate(apply);
}

int aimee_db3_route_init(aimee_db3_route_t *route, aimee_db3_search_fn internal_pgvector_search,
                         void *internal_context,
                         aimee_db3_candidate_authorize_fn authorize_candidate,
                         void *authorize_context)
{
   if (!route || !internal_pgvector_search || !authorize_candidate)
      return -1;
   memset(route, 0, sizeof(*route));
   route->internal_pgvector_search = internal_pgvector_search;
   route->internal_context = internal_context;
   route->authorize_candidate = authorize_candidate;
   route->authorize_context = authorize_context;
   return 0;
}

int aimee_db3_route_select(aimee_db3_route_t *route, uint32_t principal, int ready,
                           int fallback_enabled, aimee_db3_search_fn external_search,
                           void *external_context)
{
   if (!route || principal == 0 || !external_search)
      return -1;
   route->selected_principal = principal;
   route->selected_ready = ready != 0;
   route->fallback_enabled = fallback_enabled != 0;
   route->external_search = external_search;
   route->external_context = external_context;
   return 0;
}

void aimee_db3_route_clear(aimee_db3_route_t *route)
{
   if (!route)
      return;
   route->selected_principal = 0;
   route->selected_ready = 0;
   route->fallback_enabled = 0;
   route->external_search = NULL;
   route->external_context = NULL;
}

static aimee_db3_result_t call_and_validate(aimee_db3_search_fn search, void *context,
                                            const aimee_db3_search_request_t *request,
                                            aimee_db3_search_reply_t *reply, int external)
{
   memset(reply, 0, sizeof(*reply));
   if (search(context, request, reply) != 0)
      return external ? AIMEE_DB3_PROVIDER_FAILURE : AIMEE_DB3_INTERNAL;
   if (aimee_db3_search_reply_validate(request, reply) != 0)
      return AIMEE_DB3_INVALID_RESPONSE;
   return AIMEE_DB3_OK;
}

static aimee_db3_result_t authorize(aimee_db3_route_t *route,
                                    const aimee_db3_search_request_t *request,
                                    aimee_db3_search_reply_t *reply)
{
   uint32_t kept = 0;
   for (uint32_t i = 0; i < reply->count; ++i)
   {
      int allowed = route->authorize_candidate(route->authorize_context, request->workspace,
                                               request->project, reply->candidates[i].point_id);
      if (allowed < 0)
         return AIMEE_DB3_INTERNAL;
      if (allowed)
         reply->candidates[kept++] = reply->candidates[i];
   }
   reply->count = kept;
   return AIMEE_DB3_OK;
}

aimee_db3_result_t aimee_db3_memory_candidates_search(aimee_db3_route_t *route,
                                                      const aimee_db3_search_request_t *request,
                                                      aimee_db3_search_outcome_t *outcome)
{
   if (!outcome)
      return AIMEE_DB3_INVALID_REQUEST;
   memset(outcome, 0, sizeof(*outcome));
   outcome->result = AIMEE_DB3_INVALID_REQUEST;
   if (!route || !route->internal_pgvector_search || !route->authorize_candidate ||
       aimee_db3_search_request_validate(request) != 0)
      return outcome->result;

   outcome->selected_principal = route->selected_principal;
   if (route->selected_principal == 0)
   {
      outcome->route = AIMEE_DB3_ROUTE_DEFAULT_PGVECTOR;
      outcome->result = call_and_validate(route->internal_pgvector_search, route->internal_context,
                                          request, &outcome->reply, 0);
   }
   else if (route->selected_ready)
   {
      outcome->route = AIMEE_DB3_ROUTE_EXTERNAL;
      outcome->result = call_and_validate(route->external_search, route->external_context, request,
                                          &outcome->reply, 1);
      if (outcome->result != AIMEE_DB3_OK)
         outcome->external_error = (int)outcome->result;
   }
   else
   {
      outcome->route = AIMEE_DB3_ROUTE_EXTERNAL;
      outcome->result = AIMEE_DB3_UNAVAILABLE;
      outcome->external_error = AIMEE_DB3_UNAVAILABLE;
   }

   if (route->selected_principal != 0 && outcome->result != AIMEE_DB3_OK && route->fallback_enabled)
   {
      outcome->route = AIMEE_DB3_ROUTE_EXPLICIT_FALLBACK;
      outcome->result = call_and_validate(route->internal_pgvector_search, route->internal_context,
                                          request, &outcome->reply, 0);
   }
   if (outcome->result == AIMEE_DB3_OK)
      outcome->result = authorize(route, request, &outcome->reply);
   return outcome->result;
}
