/* Pattern extraction connection seam.
 *
 * The classifiers and extraction policy live in the Go memory process. C owns
 * only callback registration and forwarding at the event-bus boundary.
 */
#include "memory_extract_patterns.h"

#include <string.h>

static memory_pattern_extractor_fn g_extractor;
static memory_pattern_turn_scanner_fn g_scanner;

void memory_extract_register_extractor(memory_pattern_extractor_fn extractor)
{
   g_extractor = extractor;
}

void memory_extract_register_turn_scanner(memory_pattern_turn_scanner_fn scanner)
{
   g_scanner = scanner;
}

int memory_extract_patterns(const char *text, pattern_triple_t *out, int max)
{
   int count = 0;
   if (!text || !out || max <= 0 || !g_extractor ||
       g_extractor(text, out, max, &count) != 0 || count < 0 || count > max)
      return -1;
   return count;
}

int memory_pattern_scan_turn(const char *text, memory_pattern_turn_t *out)
{
   if (!text || !out || !g_scanner)
      return -1;
   memset(out, 0, sizeof(*out));
   return g_scanner(text, out);
}
