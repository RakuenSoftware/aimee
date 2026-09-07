/* cli_profile.c: see cli_profile.h. */

#include "cli_profile.h"

#include <stdio.h>
#include <string.h>

static void shift_argv_left(int *argc, char **argv, int from, int by)
{
   for (int j = from; j + by < *argc; j++)
      argv[j] = argv[j + by];
   *argc -= by;
}

int cli_extract_profile(int *argc, char **argv, char *out, size_t outsz)
{
   if (!argc || !argv || !out || outsz == 0 || *argc <= 1)
      return 0;

   for (int i = 1; i < *argc; i++)
   {
      const char *a = argv[i];
      if (!a)
         continue;
      /* Everything after the option terminator belongs to the launched client. */
      if (strcmp(a, "--") == 0)
         break;

      const char *value = NULL;
      int consume = 0;

      if (strcmp(a, "-p") == 0 || strcmp(a, "--profile") == 0)
      {
         /* '-p' or '--profile' followed by separate value */
         if (i + 1 >= *argc || !argv[i + 1] || !argv[i + 1][0])
            return 0;
         value = argv[i + 1];
         consume = 2;
      }
      else if (strncmp(a, "--profile=", 10) == 0)
      {
         /* '--profile=value' */
         value = a + 10;
         if (!value[0])
            return 0;
         consume = 1;
      }
      else
      {
         continue;
      }

      snprintf(out, outsz, "%s", value);
      shift_argv_left(argc, argv, i, consume);
      return 1;
   }
   return 0;
}
