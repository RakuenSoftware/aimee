#include "vault_custody_pkcs11.h"
#include "vault_crypto.h"
#include "runtime_secret.h"
#include <openssl/crypto.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#ifdef WITH_PKCS11
#include <dlfcn.h>
#include <p11-kit/pkcs11.h>
typedef struct
{
   pthread_mutex_t mu;
   void *dl;
   CK_FUNCTION_LIST_PTR f;
   CK_SESSION_HANDLE s;
   int sealed;
} pk11_ctx;
static pk11_ctx g = {PTHREAD_MUTEX_INITIALIZER, NULL, NULL, 0, 1};
static int open_token(pk11_ctx *c)
{
   const char *mod = getenv("AIMEE_VAULT_PKCS11_MODULE");
   if (!mod || !*mod)
      mod = "/usr/lib/softhsm/libsofthsm2.so";
   char pin[256] = "";
   if (!runtime_secret_get("AIMEE_VAULT_PKCS11_PIN", pin, sizeof(pin)))
      return -1;
   const char *slot_s = getenv("AIMEE_VAULT_PKCS11_SLOT");
   unsigned long slot = slot_s ? strtoul(slot_s, NULL, 10) : 0;
   c->dl = dlopen(mod, RTLD_NOW | RTLD_LOCAL);
   if (!c->dl)
   {
      OPENSSL_cleanse(pin, sizeof(pin));
      return -1;
   }
   CK_C_GetFunctionList get = (CK_C_GetFunctionList)dlsym(c->dl, "C_GetFunctionList");
   if (!get || get(&c->f) != CKR_OK)
   {
      OPENSSL_cleanse(pin, sizeof(pin));
      return -1;
   }
   if (c->f->C_Initialize(NULL_PTR) != CKR_OK)
   {
      OPENSSL_cleanse(pin, sizeof(pin));
      return -1;
   }
   CK_ULONG n = 8;
   CK_SLOT_ID slots[8];
   if (c->f->C_GetSlotList(CK_TRUE, slots, &n) != CKR_OK || !n)
   {
      OPENSSL_cleanse(pin, sizeof(pin));
      return -1;
   }
   if (slot_s && slot >= n)
   {
      OPENSSL_cleanse(pin, sizeof(pin));
      return -1;
   }
   slot = slot_s ? slots[slot] : slots[0];
   if (c->f->C_OpenSession(slot, CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &c->s) != CKR_OK)
   {
      OPENSSL_cleanse(pin, sizeof(pin));
      return -1;
   }
   CK_RV login_rc = c->f->C_Login(c->s, CKU_USER, (CK_UTF8CHAR_PTR)pin, (CK_ULONG)strlen(pin));
   OPENSSL_cleanse(pin, sizeof(pin));
   if (login_rc != CKR_OK)
      return -1;
   c->sealed = 0;
   return 0;
}
static int get_kek(void *v, uint8_t out[VAULT_KEK_LEN])
{
   pk11_ctx *c = v;
   pthread_mutex_lock(&c->mu);
   if (c->sealed && open_token(c) != 0)
   {
      pthread_mutex_unlock(&c->mu);
      return -1;
   }
   const char *label = getenv("AIMEE_VAULT_PKCS11_LABEL");
   if (!label || !*label)
      label = "aimee-kek";
   CK_UTF8CHAR lab[128];
   memcpy(lab, label, strlen(label));
   CK_ULONG ll = strlen(label);
   CK_ATTRIBUTE q = {CKA_LABEL, lab, ll};
   if (c->f->C_FindObjectsInit(c->s, &q, 1) != CKR_OK)
   {
      pthread_mutex_unlock(&c->mu);
      return -1;
   }
   CK_OBJECT_HANDLE o = 0;
   CK_ULONG n = 0;
   CK_RV r = c->f->C_FindObjects(c->s, &o, 1, &n);
   c->f->C_FindObjectsFinal(c->s);
   if (r != CKR_OK || n != 1)
   {
      pthread_mutex_unlock(&c->mu);
      return -1;
   }
   CK_ULONG len = VAULT_KEK_LEN;
   CK_ATTRIBUTE a = {CKA_VALUE, out, len};
   r = c->f->C_GetAttributeValue(c->s, o, &a, 1);
   pthread_mutex_unlock(&c->mu);
   return r == CKR_OK && a.ulValueLen == VAULT_KEK_LEN ? 0 : -1;
}
static int sealed(void *v)
{
   return ((pk11_ctx *)v)->sealed;
}
static int unseal(void *v, const void *p, size_t n)
{
   (void)p;
   (void)n;
   pk11_ctx *c = v;
   pthread_mutex_lock(&c->mu);
   int r = open_token(c);
   pthread_mutex_unlock(&c->mu);
   return r;
}
static int seal(void *v)
{
   ((pk11_ctx *)v)->sealed = 1;
   return 0;
}
static int rotate(void *v, const char *a, int *b, int *c, char *d, size_t e, char *f, size_t g)
{
   (void)v;
   (void)a;
   (void)b;
   (void)c;
   (void)d;
   (void)e;
   (void)f;
   (void)g;
   return -1;
}
static const vault_custody_provider_t p = {
    .name = "pkcs11",
    .ctx = &g,
    .get_kek = get_kek,
    .rotate = rotate,
    .is_sealed = sealed,
    .unseal = unseal,
    .seal = seal,
};
#else
static int fail(void *v, uint8_t k[VAULT_KEK_LEN])
{
   (void)v;
   (void)k;
   return -1;
}
static int yes(void *v)
{
   (void)v;
   return 1;
}
static int no(void *v, const void *p, size_t n)
{
   (void)v;
   (void)p;
   (void)n;
   return -1;
}
static int no_seal(void *v)
{
   (void)v;
   return -1;
}
static int rot(void *v, const char *a, int *b, int *c, char *d, size_t e, char *f, size_t g)
{
   (void)v;
   (void)a;
   (void)b;
   (void)c;
   (void)d;
   (void)e;
   (void)f;
   (void)g;
   return -1;
}
static const vault_custody_provider_t p = {
    .name = "pkcs11",
    .get_kek = fail,
    .rotate = rot,
    .is_sealed = yes,
    .unseal = no,
    .seal = no_seal,
};
#endif
const vault_custody_provider_t *vault_custody_pkcs11_provider(void)
{
   return &p;
}
