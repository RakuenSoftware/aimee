/* modules/aws/aws_eventstream.h: AWS eventstream framing decoder (P6b).
 *
 * A PURE, OFFLINE, memory-safety-hardened decoder for the AWS
 * `application/vnd.amazon.eventstream` wire framing that Bedrock streaming
 * emits. NO network, NO clock, NO heap allocation of attacker-sized buffers,
 * NO global mutable state — the caller owns the rolling byte buffer; the
 * decoder is stateless per call and reports how many bytes one complete
 * message consumed. All string/blob/payload/header-name values are ZERO-COPY
 * views {ptr,len} into the CALLER'S buffer: that buffer MUST outlive any use
 * of the decoded aws_es_message_t. The decoder never retains, copies
 * unbounded, or takes ownership.
 *
 * MEMORY-SAFETY CONTRACT (this parses attacker-influenced binary length
 * fields): every length is validated against the remaining bounds BEFORE any
 * read; total_length + headers_length are read as uint32 then WIDENED for all
 * arithmetic; the payload length is only computed AFTER checking
 * headers_length + 16 <= total_length (no unsigned underflow); no signed
 * length arithmetic; no unbounded read.
 *
 * STREAM CONTRACT: aws_es_decode returns AWS_ES_OK (one full valid message,
 * consumed == total_length), AWS_ES_NEED_MORE (fewer than 12 prelude bytes, or
 * fewer than total_length bytes buffered — consumed == 0, the caller buffers
 * more and retries), or AWS_ES_ERROR (malformed — consumed == 0). AWS_ES_ERROR
 * is FATAL and UNRECOVERABLE: an eventstream has no frame delimiter to resync
 * to, so on AWS_ES_ERROR the caller MUST tear down the stream/connection. Do
 * NOT attempt to "skip and continue".
 *
 * Wire format (all multi-byte integers big-endian):
 *   Prelude (12 B): total_length(u32) headers_length(u32) prelude_crc(u32).
 *   Headers (headers_length B): repeat { name_len(u8) name value_type(u8)
 *     value } where value size depends on value_type (see aws_es_hdr_type_t).
 *   Payload: total_length - headers_length - 16 bytes.
 *   Message CRC (4 B, u32): CRC32 of the first (total_length - 4) bytes.
 *
 * Depends only on libc (self-contained CRC32 — NO zlib/OpenSSL). */
#ifndef DEC_AWS_EVENTSTREAM_H
#define DEC_AWS_EVENTSTREAM_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Hard cap on a single message (reject oversized attacker frames): 16 MiB. */
#define AWS_ES_MAX_MESSAGE (16 * 1024 * 1024)

/* Fixed prelude (12 B) + trailing message CRC (4 B) = 16 B of framing overhead;
 * total_length < 16 is malformed. */
#define AWS_ES_FRAMING_OVERHEAD 16

/* Max headers captured into the fixed message array. A well-formed message with
 * more headers still DECODES (payload + classification + the first N headers are
 * reported) and sets headers_truncated; the excess are bounds-validated but not
 * captured. Exceeding the cap is NOT an error. */
#define AWS_ES_MAX_HEADERS 32

   typedef enum
   {
      AWS_ES_OK = 0,    /* one full valid message; consumed == total_length */
      AWS_ES_NEED_MORE, /* incomplete; buffer more bytes and retry; consumed == 0 */
      AWS_ES_ERROR      /* malformed; FATAL — tear down the stream; consumed == 0 */
   } aws_es_status_t;

   typedef enum
   {
      AWS_ES_MSG_EVENT = 0, /* :message-type == "event" */
      AWS_ES_MSG_EXCEPTION, /* :message-type == "exception" */
      AWS_ES_MSG_ERROR,     /* :message-type == "error" */
      AWS_ES_MSG_UNKNOWN    /* :message-type absent / unrecognized */
   } aws_es_msg_type_t;

   /* Header value wire types. Values 2/3/4/5/8 decode to a fixed-width integer;
    * 6/7 are u16-length-prefixed; 9 is a fixed 16-byte UUID; 0/1 carry no bytes. */
   typedef enum
   {
      AWS_ES_HDR_BOOL_TRUE = 0,  /* 0 bytes */
      AWS_ES_HDR_BOOL_FALSE = 1, /* 0 bytes */
      AWS_ES_HDR_INT8 = 2,       /* 1 byte  */
      AWS_ES_HDR_INT16 = 3,      /* 2 bytes BE */
      AWS_ES_HDR_INT32 = 4,      /* 4 bytes BE */
      AWS_ES_HDR_INT64 = 5,      /* 8 bytes BE (fixed, NOT a varint) */
      AWS_ES_HDR_BYTES = 6,      /* u16 BE len + bytes (blob) */
      AWS_ES_HDR_STRING = 7,     /* u16 BE len + bytes (UTF-8) */
      AWS_ES_HDR_TIMESTAMP = 8,  /* 8 bytes BE int64 (epoch millis) */
      AWS_ES_HDR_UUID = 9        /* 16 bytes */
   } aws_es_hdr_type_t;

   /* A borrowed view into the caller's buffer. NOT NUL-terminated. */
   typedef struct
   {
      const uint8_t *ptr;
      size_t len;
   } aws_es_view_t;

   typedef struct
   {
      aws_es_view_t name; /* header name (view into caller buffer) */
      uint8_t value_type; /* aws_es_hdr_type_t */
      /* For the integer types (INT8/16/32/64, TIMESTAMP) and bool (0/1): the
       * decoded value in HOST byte order (converted from wire big-endian; bool
       * is 1/0). For STRING/BYTES/UUID: `value` is the {ptr,len} view and
       * int_value is 0. */
      int64_t int_value;
      aws_es_view_t value; /* STRING/BYTES/UUID payload view; {NULL,0} otherwise */
   } aws_es_header_t;

   typedef struct
   {
      size_t n_headers; /* headers captured (<= AWS_ES_MAX_HEADERS) */
      aws_es_header_t headers[AWS_ES_MAX_HEADERS];
      int headers_truncated;      /* set if the message had > cap headers */
      aws_es_view_t payload;      /* message payload view (may be len 0) */
      aws_es_msg_type_t msg_type; /* classified from :message-type */
      /* Convenience pointers to captured headers (or NULL if absent/uncaptured).
       * These point INTO `headers` above; their string values are views into the
       * caller buffer. Reported for the relevant message classes. */
      const aws_es_header_t *message_type;   /* :message-type */
      const aws_es_header_t *event_type;     /* :event-type */
      const aws_es_header_t *content_type;   /* :content-type */
      const aws_es_header_t *exception_type; /* :exception-type */
      const aws_es_header_t *error_code;     /* :error-code */
      const aws_es_header_t *error_message;  /* :error-message */
   } aws_es_message_t;

   /* IEEE 802.3 CRC32 (reflected, poly 0xEDB88320, init/xorout 0xFFFFFFFF).
    * Table-based, self-contained. aws_es_crc32(NULL/"" ,0) == 0 and
    * aws_es_crc32("123456789",9) == 0xCBF43926. Named aws_es_crc32 (not a
    * generic crc32) to avoid any link collision. */
   uint32_t aws_es_crc32(const uint8_t *buf, size_t len);

   /* Decode ONE complete message from the FRONT of buf[0..len). On AWS_ES_OK
    * fills *out (views point into buf) and sets *consumed = total_length. On
    * AWS_ES_NEED_MORE / AWS_ES_ERROR sets *consumed = 0 and *out is unspecified.
    * See the STREAM + MEMORY-SAFETY contracts above. */
   aws_es_status_t aws_es_decode(const uint8_t *buf, size_t len, aws_es_message_t *out,
                                 size_t *consumed);

   const char *aws_es_status_str(aws_es_status_t st);
   const char *aws_es_msg_type_str(aws_es_msg_type_t t);

#ifdef __cplusplus
}
#endif

#endif /* DEC_AWS_EVENTSTREAM_H */
