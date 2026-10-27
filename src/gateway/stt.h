#pragma once
#include <stddef.h>

/* Return only a complete semantic transcript (no provider JSON/progress output).
 * On failure, clear out_text; never publish a truncated transcript. */
int stt_transcribe_file(const char *path, char *out_text, size_t out_size);
int stt_is_hallucination(const char *text);
