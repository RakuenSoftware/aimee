/* tts.h: optional TTS (text-to-speech) dispatcher for gateway voice replies. */
#pragma once

/* Returns 1 if TTS is enabled (AIMEE_GATEWAY_TTS_PROVIDER is set), 0 otherwise. */
int tts_enabled(void);

/* Synthesize `text` to an audio file. On success, *out_path_out is set to a
 * malloc'd unique path string pointing to an owned audio file (wav or mp3
 * depending on provider). Caller must unlink the file and free the path.
 * Returns 0 on success, -1 on error with *out_path_out set to NULL. */
int tts_synthesize(const char *text, char **out_path_out);
