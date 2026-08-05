/* Wire contracts for bounded skills-process decisions. */
#ifndef AIMEE_SKILLS_MODULE_API_H
#define AIMEE_SKILLS_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_SKILLS_EVENT_CONTEXT  7681u
#define AIMEE_SKILLS_STAGE_CONTEXT  1u
#define AIMEE_SKILLS_REQUEST_MAGIC  0x58435453u /* "STCX" */
#define AIMEE_SKILLS_RESPONSE_MAGIC 0x57454956u /* "VIEW" */
#define AIMEE_SKILLS_WIRE_VERSION   1u
#define AIMEE_SKILLS_REQUEST_LEN    16u
#define AIMEE_SKILLS_RESPONSE_LEN   8u

#define AIMEE_SKILLS_EVENT_TRIGGER          7682u
#define AIMEE_SKILLS_STAGE_TRIGGER          2u
#define AIMEE_SKILLS_TRIGGER_REQUEST_MAGIC  0x51544b53u /* "SKTQ" */
#define AIMEE_SKILLS_TRIGGER_RESPONSE_MAGIC 0x52544b53u /* "SKTR" */
#define AIMEE_SKILLS_TRIGGER_HEADER_LEN     20u
#define AIMEE_SKILLS_TRIGGER_CONTENT_MAX    (100u * 1024u)
#define AIMEE_SKILLS_TRIGGER_TOOL_MAX       255u
#define AIMEE_SKILLS_TRIGGER_SUBJECT_MAX    (1024u * 1024u)
#define AIMEE_SKILLS_TRIGGER_RESPONSE_LEN   12u

static inline void aimee_skills_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_skills_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline int aimee_skills_nonzero_text(const uint8_t *text, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (text[i] == 0)
         return 0;
   return 1;
}

static inline int aimee_skills_request_encode(int hook_count, int interval, uint8_t *out,
                                              size_t cap)
{
   if (!out || cap < AIMEE_SKILLS_REQUEST_LEN)
      return -1;
   aimee_skills_put_u32(out, AIMEE_SKILLS_REQUEST_MAGIC);
   aimee_skills_put_u32(out + 4, AIMEE_SKILLS_WIRE_VERSION);
   aimee_skills_put_u32(out + 8, (uint32_t)hook_count);
   aimee_skills_put_u32(out + 12, (uint32_t)interval);
   return 0;
}

static inline int aimee_skills_response_decode(const uint8_t *in, size_t len, int *fire)
{
   if (!in || len != AIMEE_SKILLS_RESPONSE_LEN || !fire ||
       aimee_skills_get_u32(in) != AIMEE_SKILLS_RESPONSE_MAGIC || aimee_skills_get_u32(in + 4) > 1u)
      return -1;
   *fire = (int)aimee_skills_get_u32(in + 4);
   return 0;
}

static inline size_t aimee_skills_trigger_request_size(const char *content, const char *tool_name,
                                                       const char *subject)
{
   if (!content || !tool_name || !tool_name[0])
      return 0;
   size_t content_len = strlen(content);
   size_t tool_len = strlen(tool_name);
   size_t subject_len = subject ? strlen(subject) : 0;
   if (content_len > AIMEE_SKILLS_TRIGGER_CONTENT_MAX || tool_len > AIMEE_SKILLS_TRIGGER_TOOL_MAX ||
       subject_len > AIMEE_SKILLS_TRIGGER_SUBJECT_MAX)
      return 0;
   return AIMEE_SKILLS_TRIGGER_HEADER_LEN + content_len + tool_len + subject_len;
}

static inline int aimee_skills_trigger_request_encode(const char *content, const char *tool_name,
                                                      const char *subject, uint8_t *out,
                                                      size_t capacity)
{
   size_t total = aimee_skills_trigger_request_size(content, tool_name, subject);
   if (!total || !out || capacity < total)
      return -1;
   size_t content_len = strlen(content);
   size_t tool_len = strlen(tool_name);
   size_t subject_len = subject ? strlen(subject) : 0;
   aimee_skills_put_u32(out, AIMEE_SKILLS_TRIGGER_REQUEST_MAGIC);
   aimee_skills_put_u32(out + 4, AIMEE_SKILLS_WIRE_VERSION);
   aimee_skills_put_u32(out + 8, (uint32_t)content_len);
   aimee_skills_put_u32(out + 12, (uint32_t)tool_len);
   aimee_skills_put_u32(out + 16, (uint32_t)subject_len);
   memcpy(out + AIMEE_SKILLS_TRIGGER_HEADER_LEN, content, content_len);
   memcpy(out + AIMEE_SKILLS_TRIGGER_HEADER_LEN + content_len, tool_name, tool_len);
   if (subject_len)
      memcpy(out + AIMEE_SKILLS_TRIGGER_HEADER_LEN + content_len + tool_len, subject, subject_len);
   return 0;
}

static inline int aimee_skills_trigger_request_decode(
    const uint8_t *in, size_t len, const uint8_t **content, uint32_t *content_len,
    const uint8_t **tool_name, uint32_t *tool_len, const uint8_t **subject, uint32_t *subject_len)
{
   if (!in || len < AIMEE_SKILLS_TRIGGER_HEADER_LEN || !content || !content_len || !tool_name ||
       !tool_len || !subject || !subject_len ||
       aimee_skills_get_u32(in) != AIMEE_SKILLS_TRIGGER_REQUEST_MAGIC ||
       aimee_skills_get_u32(in + 4) != AIMEE_SKILLS_WIRE_VERSION)
      return -1;
   uint32_t c_len = aimee_skills_get_u32(in + 8);
   uint32_t t_len = aimee_skills_get_u32(in + 12);
   uint32_t s_len = aimee_skills_get_u32(in + 16);
   if (c_len > AIMEE_SKILLS_TRIGGER_CONTENT_MAX || t_len == 0 ||
       t_len > AIMEE_SKILLS_TRIGGER_TOOL_MAX || s_len > AIMEE_SKILLS_TRIGGER_SUBJECT_MAX)
      return -1;
   size_t total = AIMEE_SKILLS_TRIGGER_HEADER_LEN + (size_t)c_len + (size_t)t_len + (size_t)s_len;
   if (total != len)
      return -1;
   const uint8_t *c_text = in + AIMEE_SKILLS_TRIGGER_HEADER_LEN;
   const uint8_t *t_text = c_text + c_len;
   const uint8_t *s_text = t_text + t_len;
   if (!aimee_skills_nonzero_text(c_text, c_len) || !aimee_skills_nonzero_text(t_text, t_len) ||
       !aimee_skills_nonzero_text(s_text, s_len))
      return -1;
   *content = c_text;
   *content_len = c_len;
   *tool_name = t_text;
   *tool_len = t_len;
   *subject = s_text;
   *subject_len = s_len;
   return 0;
}

static inline int aimee_skills_trigger_response_encode(int match, uint8_t *out, size_t capacity)
{
   if ((match != 0 && match != 1) || !out || capacity < AIMEE_SKILLS_TRIGGER_RESPONSE_LEN)
      return -1;
   aimee_skills_put_u32(out, AIMEE_SKILLS_TRIGGER_RESPONSE_MAGIC);
   aimee_skills_put_u32(out + 4, AIMEE_SKILLS_WIRE_VERSION);
   aimee_skills_put_u32(out + 8, (uint32_t)match);
   return 0;
}

static inline int aimee_skills_trigger_response_decode(const uint8_t *in, size_t len, int *match)
{
   if (!in || len != AIMEE_SKILLS_TRIGGER_RESPONSE_LEN || !match ||
       aimee_skills_get_u32(in) != AIMEE_SKILLS_TRIGGER_RESPONSE_MAGIC ||
       aimee_skills_get_u32(in + 4) != AIMEE_SKILLS_WIRE_VERSION ||
       aimee_skills_get_u32(in + 8) > 1u)
      return -1;
   *match = (int)aimee_skills_get_u32(in + 8);
   return 0;
}

#endif
