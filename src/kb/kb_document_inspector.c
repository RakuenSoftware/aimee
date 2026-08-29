#include "kb_document_inspector.h"

#include "integrity.h"
#include "kb_doc_hash.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define INSPECT_MAX_RAW      (32 * 1024 * 1024)
#define INSPECT_MAX_MEMBERS  256
#define INSPECT_MAX_MEMBER   (4 * 1024 * 1024)
#define INSPECT_MAX_EXPANDED (16 * 1024 * 1024)
#define INSPECT_MAX_RATIO    100
#define INSPECT_MAX_TEXT     (64 * 1024)
#define INSPECT_MAX_NODES    100000

typedef struct
{
   char data[INSPECT_MAX_TEXT + 1];
   size_t len;
   int overflow;
} text_accumulator_t;

static uint16_t le16(const unsigned char *p)
{
   return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t le32(const unsigned char *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ends_with_ci(const char *value, const char *suffix)
{
   size_t n = strlen(value), m = strlen(suffix);
   return m <= n && strcasecmp(value + n - m, suffix) == 0;
}

static int bytes_contains_ci(const char *bytes, size_t n, const char *needle)
{
   size_t m = strlen(needle);
   if (!m || m > n)
      return 0;
   for (size_t i = 0; i + m <= n; i++)
      if (strncasecmp(bytes + i, needle, m) == 0)
         return 1;
   return 0;
}

static const char *find_ci(const char *bytes, size_t n, const char *needle)
{
   size_t m = strlen(needle);
   if (!m || m > n)
      return NULL;
   for (size_t i = 0; i + m <= n; i++)
      if (strncasecmp(bytes + i, needle, m) == 0)
         return bytes + i;
   return NULL;
}

static void text_append(text_accumulator_t *out, const char *bytes, size_t n)
{
   if (!out || !bytes || !n)
      return;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)bytes[i];
      if (c == '&')
      {
         if (i + 4 <= n && strncmp(bytes + i, "&lt;", 4) == 0)
         {
            c = '<';
            i += 3;
         }
         else if (i + 4 <= n && strncmp(bytes + i, "&gt;", 4) == 0)
         {
            c = '>';
            i += 3;
         }
         else if (i + 5 <= n && strncmp(bytes + i, "&amp;", 5) == 0)
         {
            c = '&';
            i += 4;
         }
      }
      if (isspace(c))
      {
         if (out->len == 0 || out->data[out->len - 1] == ' ')
            continue;
         c = ' ';
      }
      if (out->len >= INSPECT_MAX_TEXT)
      {
         out->overflow = 1;
         continue;
      }
      out->data[out->len++] = (char)c;
   }
   out->data[out->len] = '\0';
}

static void extract_attr(const char *tag, size_t n, const char *name, text_accumulator_t *hidden)
{
   const char *at = find_ci(tag, n, name);
   if (!at)
      return;
   at += strlen(name);
   const char *end = tag + n;
   while (at < end && isspace((unsigned char)*at))
      at++;
   if (at >= end || *at != '=')
      return;
   at++;
   while (at < end && isspace((unsigned char)*at))
      at++;
   if (at >= end || (*at != '\'' && *at != '"'))
      return;
   char quote = *at++;
   const char *value = at;
   while (at < end && *at != quote)
      at++;
   text_append(hidden, value, (size_t)(at - value));
}

static int tag_is_hidden(const char *tag, size_t n)
{
   int display_none =
       bytes_contains_ci(tag, n, "display:none") || bytes_contains_ci(tag, n, "display: none");
   int visibility = bytes_contains_ci(tag, n, "visibility:hidden") ||
                    bytes_contains_ci(tag, n, "visibility: hidden");
   int off_canvas = bytes_contains_ci(tag, n, "position:absolute") &&
                    (bytes_contains_ci(tag, n, "left:-") || bytes_contains_ci(tag, n, "top:-"));
   int white =
       (bytes_contains_ci(tag, n, "color:white") || bytes_contains_ci(tag, n, "color: white") ||
        bytes_contains_ci(tag, n, "color:#fff")) &&
       (bytes_contains_ci(tag, n, "background:white") ||
        bytes_contains_ci(tag, n, "background: white") ||
        bytes_contains_ci(tag, n, "background-color:#fff"));
   int collapsed_details =
       bytes_contains_ci(tag, n, "<details") && !bytes_contains_ci(tag, n, " open");
   return display_none || visibility || off_canvas || white || collapsed_details ||
          bytes_contains_ci(tag, n, " hidden") ||
          bytes_contains_ci(tag, n, "aria-hidden=\"true\"") ||
          bytes_contains_ci(tag, n, "aria-hidden='true'") ||
          bytes_contains_ci(tag, n, "w:vanish") || bytes_contains_ci(tag, n, "w:webhidden") ||
          bytes_contains_ci(tag, n, " hidden=\"1\"") ||
          bytes_contains_ci(tag, n, " state=\"hidden\"") ||
          bytes_contains_ci(tag, n, " show=\"0\"");
}

static void extract_markup_text(const char *markup, size_t n, int force_hidden,
                                text_accumulator_t *visible, text_accumulator_t *hidden, int *nodes,
                                int *active, int *external)
{
   int hidden_stack[64] = {0};
   int depth = 0, hidden_depth = force_hidden ? 1 : 0;
   size_t i = 0;
   while (i < n)
   {
      if (markup[i] != '<')
      {
         size_t start = i;
         while (i < n && markup[i] != '<')
            i++;
         text_append(hidden_depth ? hidden : visible, markup + start, i - start);
         continue;
      }
      if (i + 4 <= n && strncmp(markup + i, "<!--", 4) == 0)
      {
         const char *comment_end = find_ci(markup + i + 4, n - i - 4, "-->");
         if (!comment_end)
         {
            visible->overflow = hidden->overflow = 1;
            return;
         }
         text_append(hidden, markup + i + 4, (size_t)(comment_end - (markup + i + 4)));
         (*nodes)++;
         i = (size_t)(comment_end - markup) + 3;
         continue;
      }
      size_t end = i + 1;
      while (end < n && markup[end] != '>')
         end++;
      if (end == n || end - i > 4096)
      {
         visible->overflow = hidden->overflow = 1;
         return;
      }
      (*nodes)++;
      const char *tag = markup + i;
      size_t tag_n = end - i + 1;
      int closing = tag_n > 2 && tag[1] == '/';
      int self_closing = tag_n > 2 && tag[tag_n - 2] == '/';
      int tag_hidden = tag_is_hidden(tag, tag_n) || bytes_contains_ci(tag, tag_n, "<script") ||
                       bytes_contains_ci(tag, tag_n, "<style") ||
                       bytes_contains_ci(tag, tag_n, "<template");
      if (bytes_contains_ci(tag, tag_n, "<script") || bytes_contains_ci(tag, tag_n, " onload=") ||
          bytes_contains_ci(tag, tag_n, " onclick=") ||
          bytes_contains_ci(tag, tag_n, "javascript:"))
         (*active)++;
      if ((bytes_contains_ci(tag, tag_n, "href=") || bytes_contains_ci(tag, tag_n, "src=") ||
           bytes_contains_ci(tag, tag_n, "targetmode=\"external\"")) &&
          (bytes_contains_ci(tag, tag_n, "http://") || bytes_contains_ci(tag, tag_n, "https://") ||
           bytes_contains_ci(tag, tag_n, "=\"//")))
         (*external)++;
      extract_attr(tag, tag_n, "alt", hidden);
      extract_attr(tag, tag_n, "title", hidden);
      extract_attr(tag, tag_n, "descr", hidden);
      if (bytes_contains_ci(tag, tag_n, "<meta"))
         extract_attr(tag, tag_n, "content", hidden);
      if (closing)
      {
         if (depth > 0)
         {
            depth--;
            if (hidden_stack[depth])
               hidden_depth--;
         }
      }
      else if (!self_closing && depth < 64)
      {
         hidden_stack[depth++] = tag_hidden;
         if (tag_hidden)
            hidden_depth++;
      }
      else if (!self_closing && depth >= 64)
      {
         visible->overflow = hidden->overflow = 1;
         return;
      }
      i = end + 1;
   }
}

static int is_hidden_member(const char *name)
{
   return bytes_contains_ci(name, strlen(name), "comments") ||
          bytes_contains_ci(name, strlen(name), "notesslides/") ||
          bytes_contains_ci(name, strlen(name), "docprops/") ||
          bytes_contains_ci(name, strlen(name), "footnotes.xml") ||
          bytes_contains_ci(name, strlen(name), "endnotes.xml") ||
          bytes_contains_ci(name, strlen(name), "externallinks/");
}

static int inspect_member_name(const char *name, kb_document_channel_report_t *report)
{
   if (name[0] == '/' || name[0] == '\\' || strstr(name, "../") || strstr(name, "..\\") ||
       strchr(name, '\\'))
      return -1;
   if (ends_with_ci(name, ".zip") || ends_with_ci(name, ".docx") || ends_with_ci(name, ".pptx") ||
       ends_with_ci(name, ".xlsx") || ends_with_ci(name, ".odt") || ends_with_ci(name, ".epub"))
      return -1;
   if (bytes_contains_ci(name, strlen(name), "vbaproject.bin") ||
       bytes_contains_ci(name, strlen(name), "activex/") || ends_with_ci(name, ".exe") ||
       ends_with_ci(name, ".js"))
      report->active_content_flags++;
   return 0;
}

static int inflate_member(const unsigned char *compressed, uint32_t compressed_n, uint16_t method,
                          unsigned char *out, uint32_t out_n)
{
   if (method == 0)
   {
      if (compressed_n != out_n)
         return -1;
      memcpy(out, compressed, out_n);
      return 0;
   }
   if (method != 8)
      return -1;
   z_stream stream;
   memset(&stream, 0, sizeof(stream));
   stream.next_in = (Bytef *)compressed;
   stream.avail_in = compressed_n;
   stream.next_out = out;
   stream.avail_out = out_n;
   if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
      return -1;
   int rc = inflate(&stream, Z_FINISH);
   int ok = rc == Z_STREAM_END && stream.total_out == out_n;
   inflateEnd(&stream);
   return ok ? 0 : -1;
}

static int inspect_ooxml(const unsigned char *zip, size_t n, kb_document_channel_report_t *report,
                         text_accumulator_t *visible, text_accumulator_t *hidden)
{
   if (n < 22)
      return -1;
   size_t search_start = n > 65557 ? n - 65557 : 0;
   size_t eocd = n - 22;
   while (eocd >= search_start && le32(zip + eocd) != 0x06054b50u)
   {
      if (eocd == 0)
         return -1;
      eocd--;
   }
   if (le32(zip + eocd) != 0x06054b50u)
      return -1;
   uint16_t disk = le16(zip + eocd + 4), central_disk = le16(zip + eocd + 6);
   uint16_t disk_entries = le16(zip + eocd + 8), entries = le16(zip + eocd + 10);
   uint32_t cd_size = le32(zip + eocd + 12), cd_offset = le32(zip + eocd + 16);
   uint16_t comment_n = le16(zip + eocd + 20);
   if (disk != 0 || central_disk != 0 || disk_entries != entries ||
       (uint64_t)eocd + 22u + comment_n != n || (uint64_t)cd_offset + cd_size != eocd)
      return -1;
   if (entries == 0xffffu || entries > INSPECT_MAX_MEMBERS)
      return -2;
   report->archive_members = entries;
   size_t cursor = cd_offset;
   uint64_t expanded = 0;
   int nodes = 0;
   int content_types = 0, main_part = 0;
   char seen_names[INSPECT_MAX_MEMBERS][256] = {{0}};
   for (uint16_t entry = 0; entry < entries; entry++)
   {
      if (cursor + 46 > n || le32(zip + cursor) != 0x02014b50u)
         return -1;
      uint16_t flags = le16(zip + cursor + 8), method = le16(zip + cursor + 10);
      uint32_t compressed_n = le32(zip + cursor + 20), out_n = le32(zip + cursor + 24);
      uint16_t name_n = le16(zip + cursor + 28), extra_n = le16(zip + cursor + 30);
      uint16_t comment_n = le16(zip + cursor + 32);
      uint32_t local = le32(zip + cursor + 42);
      if (name_n == 0 || name_n > 255 || cursor + 46u + name_n + extra_n + comment_n > eocd ||
          flags & 1u || (method != 0 && method != 8))
         return -1;
      if (out_n > INSPECT_MAX_MEMBER)
         return -2;
      char name[256];
      memcpy(name, zip + cursor + 46, name_n);
      name[name_n] = '\0';
      if (inspect_member_name(name, report) != 0)
         return -1;
      for (uint16_t prior = 0; prior < entry; prior++)
         if (strcmp(name, seen_names[prior]) == 0)
            return -1;
      snprintf(seen_names[entry], sizeof(seen_names[entry]), "%s", name);
      if (strcasecmp(name, "[Content_Types].xml") == 0)
         content_types = 1;
      if ((strcmp(report->format, "docx") == 0 && strcasecmp(name, "word/document.xml") == 0) ||
          (strcmp(report->format, "pptx") == 0 && strcasecmp(name, "ppt/presentation.xml") == 0) ||
          (strcmp(report->format, "xlsx") == 0 && strcasecmp(name, "xl/workbook.xml") == 0))
         main_part = 1;
      expanded += out_n;
      if (expanded > INSPECT_MAX_EXPANDED ||
          (compressed_n == 0 ? out_n != 0
                             : (uint64_t)out_n > (uint64_t)compressed_n * INSPECT_MAX_RATIO))
         return -2;
      cursor += 46u + name_n + extra_n + comment_n;
      if (!(ends_with_ci(name, ".xml") || ends_with_ci(name, ".rels")))
         continue;
      if ((uint64_t)local + 30 > n || le32(zip + local) != 0x04034b50u)
         return -1;
      uint16_t local_flags = le16(zip + local + 6), local_method = le16(zip + local + 8);
      uint16_t local_name_n = le16(zip + local + 26), local_extra_n = le16(zip + local + 28);
      uint64_t data_at = (uint64_t)local + 30u + local_name_n + local_extra_n;
      if (local_flags != flags || local_method != method || local_name_n != name_n ||
          (uint64_t)local + 30u + local_name_n > n || memcmp(zip + local + 30, name, name_n) != 0 ||
          data_at + compressed_n > cd_offset)
         return -1;
      unsigned char *markup = malloc((size_t)out_n + 1);
      if (!markup)
         return -1;
      if (inflate_member(zip + data_at, compressed_n, method, markup, out_n) != 0)
      {
         free(markup);
         return -1;
      }
      markup[out_n] = '\0';
      if (ends_with_ci(name, ".rels") && ((bytes_contains_ci((char *)markup, out_n, "targetmode") &&
                                           bytes_contains_ci((char *)markup, out_n, "external")) ||
                                          bytes_contains_ci((char *)markup, out_n, "http://") ||
                                          bytes_contains_ci((char *)markup, out_n, "https://") ||
                                          bytes_contains_ci((char *)markup, out_n, "file:")))
         report->external_relationships++;
      int concealed_part = is_hidden_member(name) ||
                           bytes_contains_ci((char *)markup, out_n, "w:vanish") ||
                           bytes_contains_ci((char *)markup, out_n, "w:webhidden") ||
                           bytes_contains_ci((char *)markup, out_n, " hidden=\"1\"") ||
                           bytes_contains_ci((char *)markup, out_n, " state=\"hidden\"") ||
                           bytes_contains_ci((char *)markup, out_n, " show=\"0\"") ||
                           bytes_contains_ci((char *)markup, out_n, "a:off x=\"-") ||
                           (bytes_contains_ci((char *)markup, out_n, "w:color w:val=\"ffffff\"") &&
                            bytes_contains_ci((char *)markup, out_n, "w:shd w:fill=\"ffffff\""));
      extract_markup_text((char *)markup, out_n, concealed_part, visible, hidden, &nodes,
                          &report->active_content_flags, &report->external_relationships);
      free(markup);
      if (nodes > INSPECT_MAX_NODES || visible->overflow || hidden->overflow)
         return -2;
   }
   if (cursor != eocd)
      return -1;
   report->expanded_bytes = (int)expanded;
   if (!content_types || !main_part)
      return -1;
   return 0;
}

static const char *format_for(const char *filename)
{
   if (!filename)
      return "plain";
   if (ends_with_ci(filename, ".html") || ends_with_ci(filename, ".htm"))
      return "html";
   if (ends_with_ci(filename, ".docx"))
      return "docx";
   if (ends_with_ci(filename, ".pptx"))
      return "pptx";
   if (ends_with_ci(filename, ".xlsx"))
      return "xlsx";
   if (ends_with_ci(filename, ".odt") || ends_with_ci(filename, ".epub") ||
       ends_with_ci(filename, ".rtf") || ends_with_ci(filename, ".pdf"))
      return "unsupported";
   return "plain";
}

const char *kb_document_disposition_name(kb_document_disposition_t disposition)
{
   switch (disposition)
   {
   case KB_DOCUMENT_CLEAN:
      return "clean";
   case KB_DOCUMENT_REVIEW:
      return "review";
   case KB_DOCUMENT_REJECT:
      return "reject";
   case KB_DOCUMENT_UNSUPPORTED:
      return "unsupported";
   case KB_DOCUMENT_RESOURCE_LIMIT:
      return "resource_limit";
   default:
      return "invalid";
   }
}

int kb_document_inspect(const char *filename, const char *bytes, int nbytes,
                        kb_document_channel_report_t *report)
{
   if (!report || !bytes || nbytes < 0)
      return -1;
   memset(report, 0, sizeof(*report));
   const char *format = format_for(filename);
   snprintf(report->format, sizeof(report->format), "%s", format);
   kb_doc_content_hash(bytes, nbytes, report->raw_digest);
   if (strcmp(format, "unsupported") == 0)
   {
      report->disposition = KB_DOCUMENT_UNSUPPORTED;
      return 0;
   }
   if (strcmp(format, "plain") == 0)
   {
      report->disposition = KB_DOCUMENT_CLEAN;
      return 0;
   }
   if (nbytes > INSPECT_MAX_RAW)
   {
      report->resource_limit = 1;
      report->disposition = KB_DOCUMENT_RESOURCE_LIMIT;
      return 0;
   }
   text_accumulator_t visible = {0}, hidden = {0};
   int rc = 0, nodes = 0;
   if (strcmp(format, "html") == 0)
      extract_markup_text(bytes, (size_t)nbytes, 0, &visible, &hidden, &nodes,
                          &report->active_content_flags, &report->external_relationships);
   else
      rc = inspect_ooxml((const unsigned char *)bytes, (size_t)nbytes, report, &visible, &hidden);
   if (rc == -2 || visible.overflow || hidden.overflow || nodes > INSPECT_MAX_NODES)
   {
      report->resource_limit = 1;
      report->disposition = KB_DOCUMENT_RESOURCE_LIMIT;
      return 0;
   }
   if (rc != 0)
   {
      report->disposition = KB_DOCUMENT_INVALID;
      return 0;
   }
   kb_doc_content_hash(visible.data, (int)visible.len, report->visible_text_digest);
   text_accumulator_t extracted = visible;
   text_append(&extracted, "\n", 1);
   text_append(&extracted, hidden.data, hidden.len);
   kb_doc_content_hash(extracted.data, (int)extracted.len, report->extracted_text_digest);
   if (report->active_content_flags || report->external_relationships)
   {
      report->disposition = KB_DOCUMENT_REJECT;
      return 0;
   }
   if (hidden.len)
   {
      report->hidden_spans = 1;
      snprintf(report->first_hidden_channel, sizeof(report->first_hidden_channel), "%s",
               strcmp(format, "html") == 0 ? "hidden_dom" : "package_channel");
      kb_doc_content_hash(hidden.data, (int)hidden.len, report->first_hidden_digest);
      integrity_result_t lexical = integrity_gate_check(hidden.data, INTEGRITY_SOURCE_DOCUMENT);
      snprintf(report->lexical_verdict, sizeof(report->lexical_verdict), "%s",
               integrity_verdict_name(lexical.verdict));
      report->disposition =
          lexical.verdict == INTEGRITY_VERDICT_ACCEPT ? KB_DOCUMENT_REVIEW : KB_DOCUMENT_REJECT;
      return 0;
   }
   snprintf(report->lexical_verdict, sizeof(report->lexical_verdict), "accept");
   report->disposition = KB_DOCUMENT_CLEAN;
   return 0;
}
