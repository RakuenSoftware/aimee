#ifndef AIMEE_TRANSLATION_CODEC_STDIO_H
#define AIMEE_TRANSLATION_CODEC_STDIO_H
#include <stdio.h>
/* Pure one-shot adapter bridge for existing non-C hosts. No configuration,
 * credentials, transport, tools, daemon lifecycle or persistence is entered. */
int aimee_model_codec_stdio(FILE *input, FILE *output);
#endif
