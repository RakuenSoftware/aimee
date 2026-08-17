#include "db2_pii_inject_gate.h"

int memory_pii_should_inject(int sensitivity, double confidence, int turn_requests_sensitive)
{
   /* The comparison also rejects NaN, preserving the legacy fail-closed gate. */
   if (!(confidence >= DB2_PII_GATE_CONFIDENCE_FLOOR))
      return 0;
   switch (sensitivity)
   {
   case DB2_PII_SENS_NORMAL:
      return 1;
   case DB2_PII_SENS_PII:
      return turn_requests_sensitive ? 1 : 0;
   case DB2_PII_SENS_SECRET:
   default:
      return 0;
   }
}
