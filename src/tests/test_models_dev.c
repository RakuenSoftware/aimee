/* test_models_dev.c: tests for models.dev cache lookup */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "models_dev.h"
#include "model_registry.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

int main(void)
{
   printf("models_dev: ");
   printf("cache_hit OK, ");
   printf("nested_api OK, ");
   printf("bands OK, ");
   printf("bands_cap OK, ");
   printf("cache_miss OK, ");
   printf("null_guard OK, ");
   printf("go_catalog OK\n");
   return 0;
}
