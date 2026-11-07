#include "db1_client/primary_sessions.h"
#include "db1_client/transport.h"
#include "db1_client/db1_module_api.h"
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "log.h"
void aimee_log(log_level_t l,const char *m,const char *fmt,...){(void)l;(void)m;(void)fmt;}
static unsigned char frame[8192];static uint32_t length,override_length;static int calls;
int obs_bus_module_available(uint32_t event){(void)event;return 1;}
uint64_t aimee_module_call_deadline_ns(uint64_t ms){return ms*1000000;}
aimee_module_call_result_t obs_bus_module_call(uint32_t event,uint32_t stage,uint64_t trace,uint64_t deadline,const void *request,uint32_t request_len,void *response,uint32_t cap,uint32_t *response_len,aimee_module_cancelled_fn cancel,void *context){
 (void)event;(void)stage;(void)trace;(void)deadline;(void)request;(void)request_len;(void)cancel;(void)context;calls++;memcpy(response,frame,length<cap?length:cap);*response_len=override_length?override_length:length;return length>cap?AIMEE_MODULE_CALL_RESPONSE_TOO_LARGE:AIMEE_MODULE_CALL_OK;
}
static void put(uint32_t at,uint32_t n){for(int i=0;i<4;i++)frame[at+i]=(unsigned char)(n>>(8*i));}
static void reply(unsigned n,const char **values){memset(frame,0,sizeof frame);put(0,AIMEE_DB1_STATUS_OK);put(4,n);length=8;override_length=0;for(unsigned i=0;i<n;i++){size_t l=strlen(values[i]);put(length,l);length+=4;memcpy(frame+length,values[i],l);length+=(uint32_t)l;}}
static int begin(db1_original_input_receipt_t *r){return db1_session_original_input_begin("session","12345678-1234-4234-8234-123456789abc","turn-new","actor","{}",r);}
static void unknown(void){int before=calls;db1_original_input_receipt_t r;memset(&r,0xa5,sizeof r);assert(begin(&r)==-1&&r.outcome==DB1_ORIGINAL_INPUT_UNKNOWN&&r.begin_sequence==0&&r.input_sequence==0&&!r.original_turn[0]&&calls==before+1);}
int main(void){
 size_t request_size=0;assert(!db1_request_frame_size(NULL,0,&request_size)&&request_size==8);assert(db1_request_frame_size(NULL,1,&request_size)==-1);assert(db1_request_frame_size(NULL,0,NULL)==-1);
 char *maximum=malloc(AIMEE_MODULE_MESSAGE_MAX_BODY);assert(maximum);memset(maximum,'x',AIMEE_MODULE_MESSAGE_MAX_BODY-12);maximum[AIMEE_MODULE_MESSAGE_MAX_BODY-12]=0;
 const char *single[]={maximum},*extra[]={maximum,""};assert(!db1_request_frame_size(single,1,&request_size)&&request_size==AIMEE_MODULE_MESSAGE_MAX_BODY);fprintf(stderr,"checking exactMAX plus empty-cell refusal\n");assert(db1_request_frame_size(extra,2,&request_size)==-1);
 maximum[AIMEE_MODULE_MESSAGE_MAX_BODY-16]=0;assert(!db1_request_frame_size(extra,2,&request_size)&&request_size==AIMEE_MODULE_MESSAGE_MAX_BODY);const char *three_fields[]={maximum,"",""};assert(db1_request_frame_size(three_fields,3,&request_size)==-1);free(maximum);
 const char *valid[]={"9007199254740993","9007199254740994","turn-new","1","created"};db1_original_input_receipt_t r;reply(5,valid);assert(!begin(&r)&&r.begin_sequence==9007199254740993ULL&&r.input_sequence==9007199254740994ULL&&r.outcome==DB1_ORIGINAL_INPUT_CREATED);
 const char *replay[]={"1","2","turn-old","1","replay"};reply(5,replay);assert(begin(&r)==1&&!strcmp(r.original_turn,"turn-old")&&r.outcome==DB1_ORIGINAL_INPUT_REPLAY);
 const char *nulturn[]={"1","7","turn-newZ","1","created"};reply(5,nulturn);uint32_t third=8;for(int i=0;i<2;i++)third+=4+aimee_db1_get_u32(frame+third);frame[third+4+8]=0;fprintf(stderr,"checking embedded-NUL original-turn refusal\n");unknown();
 const char *gap[]={"9007199254740993","9007199254740997","turn-new","1","created"};reply(5,gap);unknown();
 for(unsigned field=0;field<5;field++){const char *bad[]={"0","01","","2","unknown"};const char *v[5];memcpy(v,valid,sizeof v);v[field]=bad[field];reply(5,v);unknown();}
 const char *badseq[]={"9223372036854775808","18446744073709551615","-1","1.0"," 1"};for(unsigned i=0;i<5;i++){const char *v[5];memcpy(v,valid,sizeof v);v[0]=badseq[i];reply(5,v);unknown();}
 const char *v[6];memcpy(v,valid,sizeof valid);v[5]="extra";reply(4,v);unknown();reply(6,v);unknown();
 memcpy(v,valid,sizeof valid);v[0]="17";v[1]="17";reply(5,v);unknown();v[0]="18";reply(5,v);unknown();memcpy(v,valid,sizeof valid);v[2]="turn-old";reply(5,v);unknown();v[2]="\xc0\xaf";reply(5,v);unknown();
 reply(5,valid);length=7;unknown();reply(5,valid);put(8,UINT32_MAX);unknown();reply(5,valid);frame[length++]=0x7f;unknown();reply(5,valid);override_length=UINT32_MAX;unknown();
 // Embedded NUL in a cell must fail before C string exposure.
 reply(1,(const char*[]){"1ab"});frame[13]=0;uint32_t st=0,n=0;assert(db1_reply_frame_validate(frame,length,&st,&n));
 reply(1,(const char*[]){"1"});assert(!db1_session_input_attachment_history_save("s","c","primary","a","t","[]"));reply(0,NULL);assert(db1_session_input_attachment_history_save("s","c","primary","a","t","[]")==-1);reply(1,(const char*[]){"2"});assert(db1_session_input_attachment_history_save("s","c","primary","a","t","[]")==-1);
 db1_family_t f={"fixture","fixture",1,1,1,0};reply(0,NULL);assert(db1_call(&f,1,NULL,0,NULL,NULL,0,NULL)==AIMEE_DB1_STATUS_OK);frame[length++]=0;assert(db1_call(&f,1,NULL,0,NULL,NULL,0,NULL)==-1);int old=calls;assert(db1_call(&f,1,NULL,0,NULL,NULL,1,NULL)==-1&&calls==old);
 char cell[2]={0};char *cells[]={cell};size_t caps[]={sizeof cell};reply(1,(const char*[]){""});assert(db1_call(&f,1,NULL,0,cells,caps,1,NULL)==AIMEE_DB1_STATUS_OK&&!cell[0]);reply(1,(const char*[]){"x"});assert(db1_call(&f,1,NULL,0,cells,caps,1,NULL)==AIMEE_DB1_STATUS_OK&&!strcmp(cell,"x"));
 // Dynamic owned-cell reader uses the same strict validator.
 char *out=NULL;reply(1,(const char*[]){"[]"});assert(!db1_session_ir_running_history_load("s","a","t",&out)&&!strcmp(out,"[]"));free(out);reply(1,(const char*[]){"axb"});frame[13]=0;out=NULL;assert(db1_session_ir_running_history_load("s","a","t",&out)==-1&&!out);
 puts("PASS typed56/57 complete reply validation, unknown/no retry, fixed+dynamic framing, legacy empty/text parity");return 0;
}
