/* wiki_render.h: deterministic markdown projection of the memory store. */
#ifndef DEC_WIKI_RENDER_H
#define DEC_WIKI_RENDER_H 1

#ifdef __cplusplus
extern "C"
{
#endif

   /* Fetch the Go owner's rendered bundle and write it locally. Existing log
    * content is retained. Returns -1 on retrieval, validation or I/O failure. */
   int wiki_render(const char *out_dir);

#ifdef __cplusplus
}
#endif

#endif /* DEC_WIKI_RENDER_H */
