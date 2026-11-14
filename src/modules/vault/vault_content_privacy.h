#ifndef AIMEE_VAULT_CONTENT_PRIVACY_H
#define AIMEE_VAULT_CONTENT_PRIVACY_H
#include <stddef.h>
#include <stdint.h>
/* Trusted host intake only. Locators convey no read/use authority and are not
 * Native protected-source proofs. No plaintext is sent to a module bus. */
int vault_content_privacy_ready(void);
int vault_content_protect_text(const char *text, char **safe);
int vault_content_protect_json(const char *json, char **safe);
/* Store-owned, fixed-purpose key; no caller-controlled credential locator. */
int vault_store_content_index_key(const uint8_t *server_kek, char *out, size_t cap);
#endif
