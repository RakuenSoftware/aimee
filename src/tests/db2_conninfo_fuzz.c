/* Randomised differential fuzzer over the same invariants as diff_oracle.
 * Seeded, so a failure is reproducible. */
#include <libpq-fe.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int db2_pg_conninfo_with_bounds(const char *c, char *o, size_t n);
static const char *KEYS[]={"connect_timeout","keepalives","keepalives_idle",
                           "keepalives_interval","keepalives_count"};
static char *val_of(PQconninfoOption*o,const char*k){for(;o->keyword;o++)if(!strcmp(o->keyword,k))return o->val;return NULL;}
static int seq(const char*a,const char*b){if(!a&&!b)return 1;if(!a||!b)return 0;return !strcmp(a,b);}
static int fails;

static void check(const char *in){
   char out[4096];
   if (db2_pg_conninfo_with_bounds(in,out,sizeof out)!=0) return;
   char *e1=NULL,*e2=NULL,*e3=NULL;
   PQconninfoOption *base=PQconninfoParse(in,&e1), *dflt=PQconninfoParse("",&e2), *res=NULL;
   if(!base) goto done;
   res=PQconninfoParse(out,&e3);
   if(!res){ printf("BROKE PARSE\n  in : [%s]\n  out: [%s]\n  %s",in,out,e3?e3:"\n"); fails++; goto done; }
   /* no keepalives exception any more: the rule is uniform */
   for(size_t i=0;i<5;i++){
      const char*k=KEYS[i]; char*vb=val_of(base,k),*vd=val_of(dflt,k),*vr=val_of(res,k);
      int set=!seq(vb,vd);
      if(set&&!seq(vr,vb)){printf("CALLER OVERRIDDEN %s ('%s'->'%s')\n  in : [%s]\n  out: [%s]\n",
          k,vb?vb:"",vr?vr:"",in,out);fails++;}
      if(!set&&(!vr||!*vr)){printf("BOUND MISSING %s\n  in : [%s]\n  out: [%s]\n",k,in,out);fails++;}
   }
   /* dbname/host/user must be byte-identical to what the input alone yields */
   const char *F[]={"dbname","host","port","user"};
   for(size_t i=0;i<4;i++){ char*a=val_of(base,F[i]),*b=val_of(res,F[i]);
      if(!seq(a,b)){printf("FIELD MUTATED %s ('%s'->'%s')\n  in : [%s]\n  out: [%s]\n",
          F[i],a?a:"",b?b:"",in,out);fails++;} }
done:
   if(base)PQconninfoFree(base); if(dflt)PQconninfoFree(dflt); if(res)PQconninfoFree(res);
   if(e1)PQfreemem(e1); if(e2)PQfreemem(e2); if(e3)PQfreemem(e3);
}

static const char *WS[]={" ","\t","\n","\r","\v","\f","  "};
static const char *OPTK[]={"connect_timeout","keepalives","keepalives_idle",
   "keepalives_interval","keepalives_count","sslmode","application_name","dbname","options"};
static const char *OPTV[]={"0","1","3","60","require","x","'a b'","'k keepalives=0'","''"};
/* Percent-encode a random subset of the key's characters, in both hex cases.
 * libpq decodes these, so connect%5Ftimeout IS connect_timeout — a generator that
 * only ever emits plain keys never exercises that path at all. */
static void encode_key(const char *k, char *out, size_t cap){
   size_t n=0;
   for(const char*c=k; *c && n+4<cap; c++){
      if(rand()%4==0){
         const char *hex = (rand()%2) ? "0123456789abcdef" : "0123456789ABCDEF";
         out[n++]='%'; out[n++]=hex[((unsigned char)*c)>>4]; out[n++]=hex[((unsigned char)*c)&15];
      } else out[n++]=*c;
   }
   out[n]='\0';
}

int main(int argc,char**argv){
   unsigned seed = argc>1?(unsigned)atoi(argv[1]):1;
   srand(seed);
   char buf[2048];
   for(int iter=0;iter<40000;iter++){
      int uri = rand()%2;
      size_t n=0; buf[0]='\0';
      if(uri){
         n+=snprintf(buf+n,sizeof buf-n,"%s://%sh%s/db",
            rand()%2?"postgresql":"postgres",
            rand()%3==0?"u:p@":"", rand()%3==0?":5432":"");
         int np=rand()%3;
         for(int i=0;i<np;i++){
            const char*k=OPTK[rand()%9]; const char*v=OPTV[rand()%9];
            if(strchr(v,'\''))continue;
            char ek[64];
            encode_key(k, ek, sizeof ek);
            n+=snprintf(buf+n,sizeof buf-n,"%c%s=%s",i==0?'?':'&',ek,v);
         }
         if(rand()%8==0) n+=snprintf(buf+n,sizeof buf-n,"%c",n&&strchr(buf,'?')?'&':'?');
      } else {
         n+=snprintf(buf+n,sizeof buf-n,"host=db");
         int np=rand()%4;
         for(int i=0;i<np;i++)
            n+=snprintf(buf+n,sizeof buf-n,"%s%s=%s",WS[rand()%7],OPTK[rand()%9],OPTV[rand()%9]);
         if(rand()%8==0) n+=snprintf(buf+n,sizeof buf-n,"%s",WS[rand()%7]);
      }
      check(buf);
      if(fails>8){printf("(stopping after %d)\n",fails);break;}
   }
   printf(fails?"\n=== %d DISAGREEMENT(S) (seed %u) ===\n":"\n=== 40000 random conninfos: no disagreements (seed %u) ===\n",
          fails?fails:(int)seed, seed);
   return fails?1:0;
}
