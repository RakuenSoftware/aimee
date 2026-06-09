#define ROUNDTABLE_BRIEF_MAX_BYTES    4096
#define ROUNDTABLE_MAX_ROUNDS_REQUEST 16

typedef struct
{
   char *rendered;
   int truncated;
   char questions[ROUNDTABLE_MAX_QUESTIONS][512];
   const char *question_ptrs[ROUNDTABLE_MAX_QUESTIONS];
   int question_count;
} normalized_roundtable_brief_t;

static int brief_append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
   if (!buf || !pos || *pos >= cap)
      return -1;
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
   va_end(ap);
   if (n < 0)
      return -1;
   if ((size_t)n >= cap - *pos)
   {
      *pos = cap - 1;
      buf[*pos] = '\0';
      return 1;
   }
   *pos += (size_t)n;
   return 0;
}

static int brief_array_append(const char *label, cJSON *arr, char *buf, size_t cap, size_t *pos,
                              normalized_roundtable_brief_t *out, char *err, size_t err_n)
{
   if (!arr)
      return 0;
   if (!cJSON_IsArray(arr))
   {
      snprintf(err, err_n, "brief.%s must be an array of strings", label);
      return -1;
   }
   if (cJSON_GetArraySize(arr) <= 0)
      return 0;
   int rc = brief_append(buf, cap, pos, "%s:\n", label);
   cJSON *it;
   cJSON_ArrayForEach(it, arr)
   {
      if (!cJSON_IsString(it))
      {
         snprintf(err, err_n, "brief.%s must be an array of strings", label);
         return -1;
      }
      if (strcmp(label, "questions") == 0 && out->question_count < ROUNDTABLE_MAX_QUESTIONS)
      {
         snprintf(out->questions[out->question_count], sizeof(out->questions[0]), "%s",
                  it->valuestring ? it->valuestring : "");
         out->question_ptrs[out->question_count] = out->questions[out->question_count];
         out->question_count++;
      }
      if (rc == 0)
         rc = brief_append(buf, cap, pos, "- %s\n", it->valuestring ? it->valuestring : "");
   }
   if (rc == 0)
      rc = brief_append(buf, cap, pos, "\n");
   if (rc > 0)
      out->truncated = 1;
   return 0;
}

static int normalize_roundtable_brief(cJSON *req, normalized_roundtable_brief_t *out, char *err,
                                      size_t err_n)
{
   memset(out, 0, sizeof(*out));
   cJSON *brief = cJSON_GetObjectItemCaseSensitive(req, "brief");
   if (!brief)
      return 0;
   char tmp[ROUNDTABLE_BRIEF_MAX_BYTES + 1];
   memset(tmp, 0, sizeof(tmp));
   size_t pos = 0;
   if (cJSON_IsString(brief))
   {
      const char *s = brief->valuestring ? brief->valuestring : "";
      while (*s && isspace((unsigned char)*s))
         s++;
      if (!*s)
         return 0;
      int rc = brief_append(tmp, sizeof(tmp), &pos, "focus:\n- %s\n", s);
      if (rc > 0)
         out->truncated = 1;
   }
   else if (cJSON_IsObject(brief))
   {
      if (brief_array_append("focus", cJSON_GetObjectItemCaseSensitive(brief, "focus"), tmp,
                             sizeof(tmp), &pos, out, err, err_n) != 0 ||
          brief_array_append("fixes", cJSON_GetObjectItemCaseSensitive(brief, "fixes"), tmp,
                             sizeof(tmp), &pos, out, err, err_n) != 0 ||
          brief_array_append("invariants", cJSON_GetObjectItemCaseSensitive(brief, "invariants"),
                             tmp, sizeof(tmp), &pos, out, err, err_n) != 0 ||
          brief_array_append("questions", cJSON_GetObjectItemCaseSensitive(brief, "questions"), tmp,
                             sizeof(tmp), &pos, out, err, err_n) != 0)
         return -1;
      if (pos == 0)
         return 0;
   }
   else
   {
      snprintf(err, err_n, "brief must be a string or object");
      return -1;
   }
   out->rendered = strdup(tmp);
   if (!out->rendered)
   {
      snprintf(err, err_n, "out of memory");
      return -1;
   }
   return 0;
}

static void add_roundtable_arrays(cJSON *resp, const roundtable_result_t *result)
{
   cJSON *items = cJSON_CreateArray();
   for (int i = 0; i < result->item_count; i++)
   {
      const roundtable_review_item_t *it = &result->items[i];
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "severity", it->severity);
      cJSON_AddStringToObject(o, "category", it->category);
      cJSON_AddStringToObject(o, "location", it->location);
      cJSON_AddStringToObject(o, "summary", it->summary);
      cJSON_AddStringToObject(o, "recommendation", it->recommendation);
      cJSON_AddStringToObject(o, "identity_key", it->identity_key);
      cJSON_AddStringToObject(o, "sources", it->sources);
      cJSON_AddNumberToObject(o, "count", it->count);
      cJSON_AddItemToArray(items, o);
   }
   cJSON_AddItemToObject(resp, "items", items);

   cJSON *answers = cJSON_CreateArray();
   for (int i = 0; i < result->answered_question_count; i++)
   {
      const roundtable_answered_question_t *a = &result->answered_questions[i];
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "question", a->question);
      cJSON_AddStringToObject(o, "answer", a->answer);
      cJSON_AddStringToObject(o, "evidence", a->evidence);
      cJSON_AddBoolToObject(o, "answered", a->answered ? 1 : 0);
      cJSON_AddItemToArray(answers, o);
   }
   cJSON_AddItemToObject(resp, "answered_questions", answers);

   cJSON *gaps = cJSON_CreateArray();
   for (int i = 0; i < result->coverage_gap_count; i++)
      cJSON_AddItemToArray(gaps, cJSON_CreateString(result->coverage_gaps[i]));
   cJSON_AddItemToObject(resp, "coverage_gaps", gaps);
}
