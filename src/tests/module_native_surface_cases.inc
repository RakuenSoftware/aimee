/* Full server catalog API, independent forked startup states. No native-memory
 * or private authority is fabricated; the provider is a library test module. */
static int module_surface_calls;
static char *module_surface_call(const char *name,const cJSON *args,int timeout,int *error,void *state)
{
 (void)args;assert(!strcmp(name,"module_surface_sample")&&timeout==5000&&state==&module_surface_calls);
 module_surface_calls++;*error=0;return strdup("module test reply");
}
static const cJSON *module_surface_find(const cJSON *tools,const char *name,int chat)
{
 const cJSON *entry;
 cJSON_ArrayForEach(entry,tools){const cJSON *f=chat?cJSON_GetObjectItemCaseSensitive(entry,"function"):entry;const char *n=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(f,"name"));if(n&&!strcmp(n,name))return f;}return NULL;
}
static void module_surface_case(int which)
{
 cJSON *schema=cJSON_Parse("{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\"}},\"required\":[\"ref\"],\"additionalProperties\":false}");assert(schema);
 agent_module_native_definition definition={"module_surface_sample","A module test definition",schema,{AIMEE_TOOL_CLASS_WRITE,AIMEE_TOOL_POLICY_WRITE,AIMEE_TOOL_POLICY_WRITE}};
 if(which==0){cJSON *catalog=build_tools_array();assert(catalog&&!module_surface_find(catalog,definition.name,1));cJSON_Delete(catalog);assert(agent_tools_module_native_install(&definition,1,module_surface_call,&module_surface_calls)<0);}
 else if(which==1){definition.name="bash";assert(agent_tools_module_native_install(&definition,1,module_surface_call,&module_surface_calls)<0);}
 else
 {
  assert(!agent_tools_module_native_install(&definition,1,module_surface_call,&module_surface_calls));
  if(which==2){cJSON *chat=build_tools_array(),*responses=build_tools_array_responses(),*anthropic=build_tools_array_anthropic();const cJSON *c=module_surface_find(chat,definition.name,1),*r=module_surface_find(responses,definition.name,0),*a=module_surface_find(anthropic,definition.name,0);assert(c&&r&&a&&cJSON_Compare(cJSON_GetObjectItemCaseSensitive(c,"parameters"),cJSON_GetObjectItemCaseSensitive(r,"parameters"),1)&&cJSON_Compare(cJSON_GetObjectItemCaseSensitive(c,"parameters"),cJSON_GetObjectItemCaseSensitive(a,"input_schema"),1));aimee_tool_t current={0};assert(!agent_tools_execution_definition(definition.name,&current)&&agent_tools_module_native_admitted(&current));aimee_tool_free(&current);cJSON_Delete(chat);cJSON_Delete(responses);cJSON_Delete(anthropic);}
  if(which==3){agent_tools_register_mcp_tool(definition.name);assert(!build_tools_array());int error=0;char *reply=agent_tools_module_native_call(definition.name,"{\"ref\":\"safe\"}",5000,&error);assert(error&&reply&&!module_surface_calls);free(reply);}
  if(which==4){int error=1;char *reply=agent_tools_module_native_call(definition.name,"{\"ref\":\"safe\"}",5000,&error);assert(!error&&reply&&module_surface_calls==1);free(reply);}
 }
 cJSON_Delete(schema);
}
static void module_surface_cases(void)
{
 for(int i=0;i<5;i++){pid_t child=fork();assert(child>=0);if(!child){alarm(10);module_surface_case(i);_exit(0);}int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
 puts("  PASS: module-native defaultoff, collisions, all three catalogs and owned callback");
}
