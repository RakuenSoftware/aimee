#include "server.h"
#include "server_http.h"
#include "server_http_internal.h"
#include "db1_client/server_sessions.h"
#include "cJSON.h"
#include "log.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static int delete_result, cancellations, pool_closes, presence_closes;
static char *last_reply;
void aimee_log(log_level_t level,const char *module,const char *fmt,...){(void)level;(void)module;(void)fmt;}
void kb_client_memory_scope_context_apply(cJSON *request){(void)request;}
char *kb_v1_action_request(const char *action,cJSON *request){(void)action;(void)request;return strdup("{\"status\":\"ok\",\"memory_unit_id\":1}");}
int db1_server_session_delete(const char *sid){assert(!strcmp(sid,"s"));return delete_result;}
int verify_cancel_session(const char *sid){(void)sid;cancellations++;return 0;}
int turn_registry_cancel(const char *sid,const char *principal){(void)sid;(void)principal;return 0;}
void server_session_pool_close(server_ctx_t *ctx,const char *sid){(void)ctx;(void)sid;pool_closes++;}
int presence_session_close(const char *sid){(void)sid;presence_closes++;return 0;}
int server_send_response(server_conn_t *conn,cJSON *response){(void)conn;free(last_reply);last_reply=cJSON_PrintUnformatted(response);return 0;}
int server_send_error(server_conn_t *conn,const char *message,const char *request_id){(void)conn;(void)message;(void)request_id;return -1;}
void server_error_kind_apply(cJSON *response,const char *kind){(void)response;(void)kind;}
int main(void)
{
 cJSON *request=cJSON_Parse("{\"session_id\":\"s\"}");
 delete_result=DB1_SESSION_DELETE_CONFLICT;assert(!handle_session_close(NULL,NULL,request));assert(last_reply&&strstr(last_reply,"input_delivery_conflict"));assert(server_http_declared_status(last_reply)==409);assert(!cancellations&&!pool_closes&&!presence_closes);
 delete_result=0;assert(!handle_session_close(NULL,NULL,request));assert(server_http_declared_status(last_reply)==200);assert(cancellations==1&&pool_closes==1&&presence_closes==1);
 delete_result=-1;assert(handle_session_close(NULL,NULL,request)==-1);assert(cancellations==1&&pool_closes==1&&presence_closes==1);
 cJSON_Delete(request);free(last_reply);puts("PASS actual session.close handler named delivery conflict→actual generic HTTP409 classifier; legacy success/failure cleanup parity");return 0;
}
