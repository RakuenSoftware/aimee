/* Parity tests for descriptor-owned DB2 certificate-serial normalization. */
#include <assert.h>
#include <stddef.h>
#include <string.h>

int kb_cert_serial_normalize(const char *serial, char *out, size_t cap);
int db2_support_cert_serial_normalize(const char *serial, char *out, size_t cap);

static void assert_parity(const char *serial, size_t cap)
{
   unsigned char legacy[640];
   unsigned char support[640];
   memset(legacy, 0xa5, sizeof(legacy));
   memset(support, 0xa5, sizeof(support));
   char *legacy_out = cap <= sizeof(legacy) ? (char *)legacy : NULL;
   char *support_out = cap <= sizeof(support) ? (char *)support : NULL;

   int legacy_rc = kb_cert_serial_normalize(serial, legacy_out, cap);
   int support_rc = db2_support_cert_serial_normalize(serial, support_out, cap);
   assert(legacy_rc == support_rc);
   assert(memcmp(legacy, support, sizeof(legacy)) == 0);
}

static void test_known_inputs(void)
{
   const char *const cases[] = {NULL,     "",         "0",        "0000",           "0x",
                                "0X00FF", "0A:1B:2C", "DEADbeef", " 00 : 0A\t0B\n", "not-hex",
                                "\x7f",   "\x80"};
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
      for (size_t cap = 0; cap <= 16; cap++)
         assert_parity(cases[i], cap);

   assert(kb_cert_serial_normalize("0A:1B:2C", NULL, 16) ==
          db2_support_cert_serial_normalize("0A:1B:2C", NULL, 16));
}

static void test_every_byte(void)
{
   char serial[] = {'0', 'x', 'a', 'x', 'b', '\0'};
   for (int byte = 1; byte <= 255; byte++)
   {
      serial[3] = (char)byte;
      assert_parity(serial, sizeof(serial));
      assert_parity(serial, 2);
   }
}

static void test_length_and_capacity_boundaries(void)
{
   char serial[530];
   for (size_t length = 0; length <= 520; length++)
   {
      memset(serial, 'A', length);
      serial[length] = '\0';
      assert_parity(serial, 1);
      assert_parity(serial, length + 1);
      assert_parity(serial, length + 2);
      assert_parity(serial, sizeof(serial));
   }

   memset(serial, ':', 520);
   serial[520] = '\0';
   assert_parity(serial, 2);
   assert_parity(serial, sizeof(serial));
}

int main(void)
{
   test_known_inputs();
   test_every_byte();
   test_length_and_capacity_boundaries();
   return 0;
}
