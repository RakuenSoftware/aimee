/* tts.h: optional TTS (text-to-speech) dispatcher for gateway voice replies. */
#pragma once
#include <aimee/ir/aimee_ir.h>

/* Returns 1 if TTS is enabled (AIMEE_GATEWAY_TTS_PROVIDER is set), 0 otherwise. */
int tts_enabled(void);

/* Synthesize `text` to an audio file. On success, *out_path_out is set to a
 * malloc'd unique path string pointing to an owned audio file (wav or mp3
 * depending on provider). Caller must unlink the file and free the path.
 * Returns 0 on success, -1 on error with *out_path_out set to NULL. */
/* Actual speech-engine edge: project ordered canonical messages here only. */
int tts_synthesize_ir(const aimee_message_t *messages, int count, char **out_path_out);
