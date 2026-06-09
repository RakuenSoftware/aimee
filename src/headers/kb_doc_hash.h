/* kb_doc_hash.h: shared content hashes for staged KB documents. */
#pragma once

#define KB_DOC_HASH_HEX_LEN 64

void kb_doc_content_hash(const char *bytes, int nbytes, char out[KB_DOC_HASH_HEX_LEN + 1]);
