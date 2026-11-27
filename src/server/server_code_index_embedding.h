#ifndef AIMEE_SERVER_CODE_INDEX_EMBEDDING_H
#define AIMEE_SERVER_CODE_INDEX_EMBEDDING_H
#include "cJSON.h"
int server_code_index_embedding_available(void);
void server_code_index_embed_file(const char *path, const char *content, cJSON *file);
void server_code_index_embed_query(const char *route, cJSON *request);
/* The embedder's current serving identity (caller frees), or NULL. */
char *server_code_index_serving_id(void);
/* Set a stored vector on an extracted file under serving identity `serving`. */
void server_code_index_reuse_embedding(cJSON *file, const cJSON *vector, const char *serving);
#endif
