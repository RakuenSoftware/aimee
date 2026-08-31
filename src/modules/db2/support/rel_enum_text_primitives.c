#include "db2_rel_enum_text.h"

const char *correction_behavior_to_text(int behavior)
{
   switch (behavior)
   {
   case DB2_CORR_SUPERSEDE:
      return "supersede";
   case DB2_CORR_HARD_DELETE:
      return "hard_delete";
   case DB2_CORR_IMMUTABLE:
      return "immutable";
   }
   return "supersede";
}

const char *rel_sensitivity_to_text(int sensitivity)
{
   switch (sensitivity)
   {
   case DB2_SENS_NORMAL:
      return "normal";
   case DB2_SENS_PII:
      return "pii";
   case DB2_SENS_SECRET:
      return "secret";
   }
   return "pii";
}
