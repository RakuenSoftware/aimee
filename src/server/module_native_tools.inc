/* Optional module adapter: no business schema, policy or authority lives here. */
#include "tool_args_coerce.h"
#include <math.h>
#include <aimee/ir/session_event.h>
extern int aimee_module_tool_source_mode(const char *) __attribute__((weak));
const unsigned aimee_module_native_tools_contract_v1
    __attribute__((used, externally_visible, retain)) = 1;
static struct
{
   pthread_mutex_t lock;
   int installed, sealed, invalid;
   size_t count;
   aimee_tool_t definitions[16];
   aimee_tool_metadata_t metadata[16];
   agent_module_native_call call;
   agent_module_native_input_admission input_admission;
   void *state;
} module_native = {.lock = PTHREAD_MUTEX_INITIALIZER};
static int module_native_index(const char *name)
{
   if (!name)
      return -1;
   for (size_t i = 0; i < module_native.count; i++)
      if (!strcmp(name, module_native.definitions[i].name))
         return (int)i;
   return -1;
}
static int module_native_copy(const aimee_tool_t *source, aimee_tool_t *out)
{
   memset(out, 0, sizeof *out);
   out->name = strdup(source->name);
   out->description = strdup(source->description);
   out->schema = cJSON_Duplicate(source->schema, 1);
   if (out->name && out->description && out->schema && aimee_ir_tool_valid(out))
      return 0;
   aimee_tool_free(out);
   return -1;
}
static int module_native_keys(const cJSON *object, const char *const *keys, size_t count)
{
   if (!cJSON_IsObject(object)) return 0;
   for (const cJSON *p = object->child; p; p = p->next)
   {
      size_t i = 0;
      while (i < count && (!p->string || strcmp(keys[i], p->string))) i++;
      if (i == count) return 0;
      for (const cJSON *q = p->next; q; q = q->next)
         if (q->string && !strcmp(p->string, q->string)) return 0;
   }
   return 1;
}
/* The adapter accepts flat scalar forms and a bounded, one-level union of
 * complete flat forms. Only these four lexical patterns are supported; this
 * is not an arbitrary regular-expression or recursive JSON-Schema engine. */
static const char *const module_native_patterns[] = {
   "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$",
   "^[0-9a-f]{64}$", "^[1-9][0-9]{0,19}$", "^(0|-?[1-9][0-9]{0,18})$"
};
static int module_native_pattern_kind(const cJSON *schema) {
   const cJSON *pattern = cJSON_GetObjectItemCaseSensitive(schema, "pattern");
   if (!pattern) return 0;
   if (!cJSON_IsString(pattern)) return -1;
   for (size_t i=0;i<sizeof module_native_patterns/sizeof *module_native_patterns;i++)
      if (!strcmp(pattern->valuestring,module_native_patterns[i])) return (int)i+1;
   return -1;
}
static int module_native_pattern_matches(int kind, const char *text) {
   if (!kind) return 1;
   if (kind<0 || !text) return 0;
   size_t n=strlen(text);
   if (kind==1 || kind==2) {
      if (n!=(kind==1?36u:64u)) return 0;
      for (size_t i=0;i<n;i++) {
         if (kind==1 && (i==8||i==13||i==18||i==23)) {
            if (text[i]!='-') return 0;
         } else if (!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f'))) return 0;
      }
      return 1;
   }
   if (kind==4 && n==1 && text[0]=='0') return 1;
   size_t start=kind==4&&text[0]=='-'?1u:0u;
   size_t digits=n-start;
   if (!digits || digits>(kind==3?20u:19u) || text[start]<'1'||text[start]>'9') return 0;
   for (size_t i=start+1;i<n;i++) if (text[i]<'0'||text[i]>'9') return 0;
   return 1;
}
static int module_native_description(const cJSON *schema) {
   const cJSON *description = cJSON_GetObjectItemCaseSensitive(schema, "description");
   return !description || (cJSON_IsString(description) && strlen(description->valuestring) <= 4096);
}
static int module_native_schema_flat(const cJSON *schema, int union_root)
{
   static const char *root_keys[] = {"type","properties","required","additionalProperties","description","oneOf"};
   static const char *string_keys[] = {"type","minLength","maxLength","enum","description","pattern"};
   static const char *boolean_keys[] = {"type","description"};
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(schema,"type"));
   const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema,"properties"),
               *required = cJSON_GetObjectItemCaseSensitive(schema,"required");
   if (!module_native_description(schema) || !module_native_keys(schema,root_keys,union_root?6:5) || !type || strcmp(type,"object") ||
       !cJSON_IsObject(properties) || cJSON_GetArraySize(properties)>64 ||
       !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(schema,"additionalProperties")) ||
       (required && !cJSON_IsArray(required))) return 0;
   for (const cJSON *p = properties->child; p; p = p->next)
   {
      if (!p->string || !p->string[0] || strlen(p->string)>95) return 0;
      for (const cJSON *q=p->next;q;q=q->next)
         if (q->string&&!strcmp(p->string,q->string)) return 0;
      const char *kind=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p,"type"));
      if (!kind || !module_native_description(p)) return 0;
      if (!strcmp(kind,"string"))
      {
         if (!module_native_keys(p,string_keys,6) || module_native_pattern_kind(p)<0) return 0;
         const cJSON *minimum=cJSON_GetObjectItemCaseSensitive(p,"minLength"),
                     *maximum=cJSON_GetObjectItemCaseSensitive(p,"maxLength");
         const cJSON *bounds[] = {minimum,maximum};
         for (size_t k=0;k<2;k++) if (bounds[k] &&
            (!cJSON_IsNumber(bounds[k])||!isfinite(bounds[k]->valuedouble)||
             bounds[k]->valuedouble<0||bounds[k]->valuedouble>1048576||
             floor(bounds[k]->valuedouble)!=bounds[k]->valuedouble)) return 0;
         if (minimum&&maximum&&minimum->valuedouble>maximum->valuedouble) return 0;
         const cJSON *values=cJSON_GetObjectItemCaseSensitive(p,"enum");
         if (values)
         {
            if (!cJSON_IsArray(values)||!cJSON_GetArraySize(values)||cJSON_GetArraySize(values)>128) return 0;
            for (const cJSON *value=values->child;value;value=value->next)
               if (!cJSON_IsString(value)||strlen(value->valuestring)>65536) return 0;
         }
      }
      else if (!strcmp(kind,"boolean"))
      { if (!module_native_keys(p,boolean_keys,2)) return 0; }
      else return 0;
   }
   for (const cJSON *r=required?required->child:NULL;r;r=r->next)
   {
      if (!cJSON_IsString(r)||!cJSON_GetObjectItemCaseSensitive(properties,r->valuestring)) return 0;
      for (const cJSON *q=r->next;q;q=q->next)
         if (cJSON_IsString(q)&&!strcmp(r->valuestring,q->valuestring)) return 0;
   }
   return 1;
}
/* Each alternative is a complete form: no nested unions, recursive objects,
 * unknown schema keywords or inferred/coerced values. The outer properties
 * remain available to the existing canonicalizer and provider renderers. */
static int module_native_schema(const cJSON *schema) {
   if (!module_native_schema_flat(schema,1)) return 0;
   const cJSON *branches=cJSON_GetObjectItemCaseSensitive(schema,"oneOf");
   if (!branches) return 1;
   if (!cJSON_IsArray(branches)||cJSON_GetArraySize(branches)<2||cJSON_GetArraySize(branches)>16) return 0;
   const cJSON *properties=cJSON_GetObjectItemCaseSensitive(schema,"properties");
   for (const cJSON *branch=branches->child;branch;branch=branch->next) {
      if (!module_native_schema_flat(branch,0)) return 0;
      const cJSON *fields=cJSON_GetObjectItemCaseSensitive(branch,"properties");
      for (const cJSON *field=fields->child;field;field=field->next)
         if (!cJSON_GetObjectItemCaseSensitive(properties,field->string)) return 0;
   }
   return 1;
}
static int module_native_form_valid(const cJSON *schema,const cJSON *args) {
   if (!aimee_ir_form_validate(schema,args)) return 0;
   const cJSON *properties=cJSON_GetObjectItemCaseSensitive(schema,"properties");
   for (const cJSON *value=args->child;value;value=value->next) {
      const cJSON *field=cJSON_GetObjectItemCaseSensitive(properties,value->string);
      int kind=module_native_pattern_kind(field);
      if (kind && (!cJSON_IsString(value)||!module_native_pattern_matches(kind,value->valuestring))) return 0;
   }
   return 1;
}
static int module_native_arguments_valid(const cJSON *schema,const cJSON *args) {
   if (!module_native_form_valid(schema,args)) return 0;
   const cJSON *branches=cJSON_GetObjectItemCaseSensitive(schema,"oneOf");
   if (!branches) return 1;
   unsigned matches=0;
   for (const cJSON *branch=branches->child;branch;branch=branch->next)
      matches+=(unsigned)module_native_form_valid(branch,args);
   return matches==1;
}
static int module_native_install(const agent_module_native_definition *defs, size_t count,
                                  agent_module_native_call call,
                                  agent_module_native_input_admission input_admission, void *state)
{
   if (!defs || !count || count > 16 || !call)
      return -1;
   aimee_tool_t owned[16] = {0};
   int result = -1;
   pthread_mutex_lock(&module_native.lock);
   if (module_native.installed || module_native.sealed)
      goto done;
   for (size_t i = 0; i < count; i++)
   {
      const char *name = defs[i].name;
      if (!name || !name[0] || strlen(name) > 95 || !defs[i].description ||
          strlen(defs[i].description) > 4096 || !cJSON_IsObject(defs[i].schema) ||
          !aimee_tool_metadata_valid(&defs[i].metadata) ||
          !module_native_schema(defs[i].schema))
         goto done;
      for (const unsigned char *p = (const unsigned char *)name; *p; p++)
         if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '_'))
            goto done;
      for (size_t j = 0; j < i; j++)
         if (!strcmp(name, owned[j].name))
            goto done;
      for (size_t j = 0; j < sizeof g_builtin_tools / sizeof *g_builtin_tools; j++)
         if (!strcmp(name, g_builtin_tools[j].name))
            goto done;
      if (agent_tools_is_mcp_derived(name))
         goto done;
      char *schema = cJSON_PrintUnformatted(defs[i].schema);
      int bounded = schema && strlen(schema) <= 65536;
      free(schema);
      if (!bounded)
         goto done;
      aimee_tool_t borrowed = {.name = (char *)name,
          .description = (char *)defs[i].description, .schema = (cJSON *)defs[i].schema};
      if (!aimee_ir_tool_valid(&borrowed) || module_native_copy(&borrowed, owned + i))
         goto done;
   }
   for (size_t i = 0; i < count; i++)
   {
      module_native.definitions[i] = owned[i];
      owned[i] = (aimee_tool_t){0};
      module_native.metadata[i] = defs[i].metadata;
   }
   module_native.count = count;
   module_native.call = call;
   module_native.input_admission = input_admission;
   module_native.state = state;
   module_native.installed = 1;
   result = 0;
done:
   pthread_mutex_unlock(&module_native.lock);
   for (size_t i = 0; i < 16; i++)
      aimee_tool_free(owned + i);
   return result;
}
extern const unsigned aimee_ir_module_input_admission_contract_v1 __attribute__((weak));
const unsigned aimee_module_native_admission_contract_v1
    __attribute__((used, externally_visible, retain)) = 1;
__attribute__((used, externally_visible, retain)) int
agent_tools_module_native_install(const agent_module_native_definition *defs, size_t count,
                                  agent_module_native_call call, void *state) {
   return module_native_install(defs, count, call, NULL, state);
}
__attribute__((used, externally_visible, retain)) int agent_tools_module_native_install_checked_v1(
    const agent_module_native_definition *defs, size_t count, agent_module_native_call call,
    agent_module_native_input_admission admit, void *state) {
   if (!admit || !&aimee_ir_module_input_admission_contract_v1 ||
       aimee_ir_module_input_admission_contract_v1 != 1)
      return -1;
   return module_native_install(defs, count, call, admit, state);
}
__attribute__((used, externally_visible, retain)) int
agent_tools_module_native_input_selected_v1(const char *name) {
   pthread_mutex_lock(&module_native.lock);
   int index = module_native_index(name),
       checked = module_native.installed && module_native.input_admission;
   int result = !checked                ? 0
                : (!name || !name[0])   ? -1
                : index < 0             ? 0
                : module_native.invalid ? -1
                                        : 1;
   pthread_mutex_unlock(&module_native.lock);
   return result;
}
extern int aimee_module_native_tool_input_buffered_v1(void) __attribute__((weak));
__attribute__((used, externally_visible, retain)) int
agent_tools_module_native_requires_buffered_v1(const aimee_tool_t *tools, size_t count) {
   if (count > AIMEE_IR_MAX_BLOCKS || (count && !tools))
      return 1;
   if (count && aimee_module_native_tool_input_buffered_v1 &&
       aimee_module_native_tool_input_buffered_v1()!=0) return 1;
   for (size_t i = 0; i < count; i++) {
      /* This registry owns root functions only. A namespace child with the same
       * basename is a different identity, governed by its existing adapter. */
      if (tools[i].kind == AIMEE_TOOL_FUNCTION && !tools[i].n_children &&
          agent_tools_module_native_input_selected_v1(tools[i].name) != 0)
         return 1;
   }
   return 0;
}
__attribute__((used, externally_visible, retain)) int
agent_tools_module_native_has(const char *name) {
   pthread_mutex_lock(&module_native.lock);
   int found = module_native_index(name) >= 0;
   pthread_mutex_unlock(&module_native.lock);
   return found;
}
static void module_native_collision(const char *name) {
   pthread_mutex_lock(&module_native.lock);
   if (module_native_index(name) >= 0)
      module_native.invalid = 1;
   pthread_mutex_unlock(&module_native.lock);
}
__attribute__((used, externally_visible, retain)) int
agent_tools_module_native_definition(const char *name, aimee_tool_t *out) {
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   pthread_mutex_lock(&module_native.lock);
   module_native.sealed = 1;
   int index = module_native.invalid ? -1 : module_native_index(name);
   int result = index >= 0 ? module_native_copy(module_native.definitions + index, out) : -1;
   pthread_mutex_unlock(&module_native.lock);
   return result;
}
__attribute__((used, externally_visible, retain)) int agent_tools_module_native_metadata(const char *name, aimee_tool_metadata_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   pthread_mutex_lock(&module_native.lock);
   module_native.sealed = 1;
   int index = module_native.invalid ? -1 : module_native_index(name);
   if (index >= 0)
      *out = module_native.metadata[index];
   pthread_mutex_unlock(&module_native.lock);
   return index >= 0 ? 0 : -1;
}
static int module_native_emit(cJSON *tools, unsigned surface)
{
   pthread_mutex_lock(&module_native.lock);
   module_native.sealed = 1;
   int invalid = module_native.invalid;
   size_t count = module_native.count;
   pthread_mutex_unlock(&module_native.lock);
   if (invalid) return -1;
   /* Definitions are immutable after installation. No generic lock is held
    * while an optional host adapter decides whether its tool is available. */
   for (size_t i = 0; i < count; i++)
   {
      const aimee_tool_t *definition = module_native.definitions + i;
      if (aimee_module_tool_source_mode && aimee_module_tool_source_mode(definition->name) < 0)
         continue;
      cJSON *entry = openai_ir_render_tool(definition, surface == TSURF_RESP);
      if (!entry || !cJSON_AddItemToArray(tools, entry))
      {
         cJSON_Delete(entry);
         return -1;
      }
   }
   return 0;
}
__attribute__((used, externally_visible, retain)) int agent_tools_module_native_admitted(const aimee_tool_t *admitted)
{
   if (!admitted || !admitted->name)
      return 0;
   if (!agent_tools_module_native_has(admitted->name))
      return 1; /* not an adapter identity */
   aimee_tool_t fresh = {0};
   int okay = !agent_tools_module_native_definition(admitted->name, &fresh) &&
      admitted->kind == AIMEE_TOOL_FUNCTION && !admitted->native_type && !admitted->native_options &&
      !admitted->n_children && admitted->description &&
      !strcmp(admitted->description, fresh.description) &&
      cJSON_Compare(admitted->schema, fresh.schema, 1);
   aimee_tool_free(&fresh);
   return okay;
}
__attribute__((used,externally_visible,retain)) int
agent_tools_module_native_input_admit_v1(const char *name,const cJSON *args)
{
   int selected = agent_tools_module_native_input_selected_v1(name);
   if (!selected)
      return 0;
   if (selected < 0 || !cJSON_IsObject(args) || !aimee_ir_json_unique(args))
      return -1;
   aimee_tool_t definition = {0};
   if (agent_tools_module_native_definition(name, &definition))
      return -1;
   cJSON *canonical = tool_args_canonicalize(definition.schema, args);
   int valid = canonical && cJSON_Compare(canonical, args, 1) &&
               module_native_arguments_valid(definition.schema, args);
   cJSON_Delete(canonical);
   aimee_tool_free(&definition);
   if (!valid)
      return -1;
   /* Immutable startup callback; no generic lock or owned temporary crosses it.
    * It is structural input admission, never execution or source permission. */
   return module_native.input_admission(name, args, module_native.state) == 0 ? 0 : -1;
}
static void module_native_delete_args(void *opaque) { cJSON_Delete(opaque); }
static void module_native_delete_reply(void *opaque) {
   char *volatile *reply=opaque;
   free(*reply);*reply=NULL;
}
__attribute__((used, externally_visible, retain)) char *
agent_tools_module_native_call(const char *name, const char *raw, int timeout, int *error) {
   int local_error = 1;
   int *reported_error = error ? error : &local_error;
   *reported_error = 1;
   aimee_tool_t definition = {0};
   if (!raw || !raw[0] || strlen(raw) > 1048576 || timeout <= 0 ||
       agent_tools_module_native_definition(name, &definition))
      return strdup("error: module tool unavailable");
   cJSON *args = aimee_ir_json_parse_frame(raw, strlen(raw));
   cJSON *canonical = cJSON_IsObject(args) ? tool_args_canonicalize(definition.schema, args) : NULL;
   int okay = canonical && cJSON_Compare(args, canonical, 1) &&
      module_native_arguments_valid(definition.schema,args);
   cJSON_Delete(canonical);
   aimee_tool_free(&definition);
   if (okay && agent_tools_module_native_input_admit_v1(name,args))
      okay = 0;
   if (!okay)
   {
      cJSON_Delete(args);
      return strdup("error: module tool arguments refused");
   }
   /* Immutable provider; no generic lock is held across a module callback. */
   char *volatile reply = NULL;
   agent_module_read_scope_v1 read_scope;
   aimee_tool_metadata_t metadata = {0};
   int trusted_read = !agent_tools_module_native_metadata(name, &metadata) &&
                      metadata.classification == AIMEE_TOOL_CLASS_READ;
   int prior_cancel;
   pthread_cleanup_push(module_native_delete_args, args);
   pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &prior_cancel);
   agent_tools_module_read_enter_v1(&read_scope, trusted_read);
   pthread_cleanup_push(agent_tools_module_read_leave_v1, &read_scope);
   pthread_cleanup_push(module_native_delete_reply, (void *)&reply);
   pthread_setcancelstate(prior_cancel, NULL);
   reply = module_native.call(name, args, timeout, reported_error, module_native.state);
   /* Restore caller cancellation state. Delivered cancellation discards the
    * transaction and frees its reply. An undelivered pending cancellation while
    * DISABLE is not observable: normal callback return commits procedural state,
    * and later delivered loop cancellation/exit still clears the entire frame. */
   pthread_setcancelstate(prior_cancel, NULL);
   if(prior_cancel==PTHREAD_CANCEL_ENABLE)pthread_testcancel();
   agent_tools_module_read_commit_v1(&read_scope, reply && !*reported_error);
   pthread_cleanup_pop(0); /* normal return transfers reply ownership */
   pthread_cleanup_pop(1);
   pthread_cleanup_pop(1);
   if (!reply) *reported_error = 1;
   else *reported_error = !!*reported_error;
   return reply ? reply : strdup("error: module tool outcome unavailable; reconcile before retry");
}
