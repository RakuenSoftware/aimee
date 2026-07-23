#include "economizer_dispatch_lease.h"
#include "economizer_json.h"
#include "economizer_provenance.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void sleep_ms(long milliseconds)
{
   struct timespec delay = {.tv_sec = milliseconds / 1000,
                            .tv_nsec = (milliseconds % 1000) * 1000000L};
   while (nanosleep(&delay, &delay) != 0)
      ;
}

static int scoped_lease_early_return(econ_dispatch_state_t *state,
                                     const econ_dispatch_facts_t *facts)
{
   ECON_DISPATCH_LEASE_SCOPED(lease);
   assert(econ_dispatch_lease_begin(state, facts, &lease) == 1);
   return 17;
}

static void test_json_compaction_preserves_non_whitespace_bytes(void)
{
   static const char source[] =
       " \n { \"a\" : [ 1.2300e+02 , \" x \\u0061 \" ], \"z\" : true } \r\n";
   static const char expected[] = "{\"a\":[1.2300e+02,\" x \\u0061 \"],\"z\":true}";
   uint8_t *out = NULL;
   size_t n = 0;
   assert(econ_json_compact(source, strlen(source), &out, &n) == ECON_JSON_OK);
   assert(n == strlen(expected));
   assert(memcmp(out, expected, n) == 0);
   free(out);

   assert(econ_json_compact(expected, strlen(expected), &out, &n) == ECON_JSON_NOT_SHORTER);
}

static void test_json_rejects_ambiguity_and_invalid_syntax(void)
{
   uint8_t *out = NULL;
   size_t n = 0;
   const char duplicate[] = "{ \"a\": 1, \"\\u0061\": 2 }";
   assert(econ_json_compact(duplicate, strlen(duplicate), &out, &n) == ECON_JSON_DUPLICATE_KEY);
   const char invalid_number[] = "[ 01 ]";
   assert(econ_json_compact(invalid_number, strlen(invalid_number), &out, &n) ==
          ECON_JSON_INVALID_SYNTAX);
   const unsigned char invalid_utf8[] = {'[', ' ', '"', 0xc0, 0x80, '"', ' ', ']'};
   assert(econ_json_compact(invalid_utf8, sizeof(invalid_utf8), &out, &n) ==
          ECON_JSON_INVALID_UTF8);
   const char lone_surrogate[] = "[ \"\\ud800\" ]";
   assert(econ_json_compact(lone_surrogate, strlen(lone_surrogate), &out, &n) ==
          ECON_JSON_INVALID_SYNTAX);
}

static void test_json_depth_bound(void)
{
   char deep[(ECON_JSON_MAX_DEPTH + 3) * 2 + 1];
   size_t p = 0;
   for (unsigned i = 0; i < ECON_JSON_MAX_DEPTH + 2; i++)
      deep[p++] = '[';
   deep[p++] = '0';
   for (unsigned i = 0; i < ECON_JSON_MAX_DEPTH + 2; i++)
      deep[p++] = ']';
   deep[p] = 0;
   uint8_t *out = NULL;
   size_t n = 0;
   assert(econ_json_compact(deep, p, &out, &n) == ECON_JSON_TOO_DEEP);
}

static void test_json_deterministic_whitespace_property(void)
{
   static const char *tokens[] = {
       "{", "\"a\"", ":", "[", "-0.25e-2", ",", "true",
       ",", "null",  "]", ",", "\"b\"",    ":", "\"space stays: \\t \\u0061\"",
       "}"};
   static const char whitespace[] = " \t\r\n";
   char expected[256] = {0};
   for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++)
      strcat(expected, tokens[i]);
   unsigned state = 0x9e3779b9u;
   for (unsigned iteration = 0; iteration < 1000; iteration++)
   {
      char source[1024];
      size_t used = 0;
      for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++)
      {
         state = state * 1664525u + 1013904223u;
         unsigned before = state & 3u;
         while (before--)
            source[used++] = whitespace[(state >> (before * 2)) & 3u];
         size_t token_len = strlen(tokens[i]);
         memcpy(source + used, tokens[i], token_len);
         used += token_len;
         state = state * 1664525u + 1013904223u;
         unsigned after = state & 3u;
         while (after--)
            source[used++] = whitespace[(state >> (after * 2)) & 3u];
      }
      uint8_t *out = NULL;
      size_t n = 0;
      assert(econ_json_compact(source, used, &out, &n) == ECON_JSON_OK);
      assert(n == strlen(expected));
      assert(memcmp(out, expected, n) == 0);
      free(out);
   }
}

typedef struct
{
   econ_provenance_t *cap;
   econ_provenance_binding_t binding;
   const char *source;
   atomic_int *wins;
} consume_arg_t;

static void *consume_thread(void *opaque)
{
   consume_arg_t *a = opaque;
   if (econ_provenance_consume(a->cap, &a->binding, a->source, strlen(a->source)) == 0)
      atomic_fetch_add(a->wins, 1);
   return NULL;
}

static void test_provenance_is_bound_and_one_shot(void)
{
   const char source[] = "{ \"large\": true }";
   econ_provenance_binding_t binding = {1, 2, 3, 4, 5, 6};
   econ_provenance_t *cap = NULL;
   assert(econ_provenance_issue_local(&binding, source, strlen(source), &cap) == 0);
   atomic_int wins;
   atomic_init(&wins, 0);
   consume_arg_t arg = {cap, binding, source, &wins};
   pthread_t threads[8];
   for (size_t i = 0; i < 8; i++)
      assert(pthread_create(&threads[i], NULL, consume_thread, &arg) == 0);
   for (size_t i = 0; i < 8; i++)
      pthread_join(threads[i], NULL);
   assert(atomic_load(&wins) == 1);
   econ_provenance_destroy(cap);

   assert(econ_provenance_issue_local(&binding, source, strlen(source), &cap) == 0);
   econ_provenance_binding_t wrong = binding;
   wrong.tenant_id++;
   assert(econ_provenance_consume(cap, &wrong, source, strlen(source)) == -1);
   assert(econ_provenance_consume(cap, &binding, "{}", 2) == -1);
   assert(econ_provenance_consume(cap, &binding, source, strlen(source)) == 0);
   econ_provenance_destroy(cap);

   assert(econ_provenance_issue_local(&binding, source, ECON_PROVENANCE_MAX_SOURCE + 1u, &cap) ==
          -1);
}

typedef struct
{
   econ_dispatch_state_t *state;
   econ_dispatch_facts_t next;
   atomic_int started;
   atomic_int finished;
} writer_arg_t;

static void *replace_thread(void *opaque)
{
   writer_arg_t *a = opaque;
   atomic_store(&a->started, 1);
   assert(econ_dispatch_state_replace(a->state, &a->next) == 0);
   atomic_store(&a->finished, 1);
   return NULL;
}

static econ_dispatch_facts_t facts(void)
{
   econ_dispatch_facts_t f = {1, 2, 3, 4, 5, 6, 7, 8, 1};
   return f;
}

static void test_first_write_lease_linearizes_invalidation(void)
{
   econ_dispatch_facts_t current = facts();
   econ_dispatch_state_t *state = NULL;
   assert(econ_dispatch_state_create(&current, &state) == 0);
   econ_dispatch_lease_t lease = {0};
   assert(econ_dispatch_lease_begin(state, &current, &lease) == 1);

   writer_arg_t writer = {.state = state, .next = current};
   writer.next.enabled = 0;
   writer.next.kill_switch_generation++;
   atomic_init(&writer.started, 0);
   atomic_init(&writer.finished, 0);
   pthread_t thread;
   assert(pthread_create(&thread, NULL, replace_thread, &writer) == 0);
   while (!atomic_load(&writer.started))
      sleep_ms(1);
   sleep_ms(20);
   assert(atomic_load(&writer.finished) == 0);
   econ_dispatch_lease_end(&lease);
   pthread_join(thread, NULL);
   assert(atomic_load(&writer.finished) == 1);

   assert(econ_dispatch_lease_begin(state, &current, &lease) == 0);
   econ_dispatch_facts_t disabled = writer.next;
   assert(econ_dispatch_lease_begin(state, &disabled, &lease) == 0);
   econ_dispatch_facts_t rollback = disabled;
   rollback.kill_switch_generation--;
   assert(econ_dispatch_state_replace(state, &rollback) == -1);
   econ_dispatch_state_destroy(state);
}

static void test_scoped_lease_releases_on_early_return(void)
{
   econ_dispatch_facts_t current = facts();
   econ_dispatch_state_t *state = NULL;
   assert(econ_dispatch_state_create(&current, &state) == 0);
   assert(scoped_lease_early_return(state, &current) == 17);
   econ_dispatch_facts_t next = current;
   next.registry_generation++;
   assert(econ_dispatch_state_replace(state, &next) == 0);
   econ_dispatch_state_destroy(state);
}

int main(void)
{
   test_json_compaction_preserves_non_whitespace_bytes();
   test_json_rejects_ambiguity_and_invalid_syntax();
   test_json_depth_bound();
   test_json_deterministic_whitespace_property();
   test_provenance_is_bound_and_one_shot();
   test_first_write_lease_linearizes_invalidation();
   test_scoped_lease_releases_on_early_return();
   puts("economizer_activation: ALL PASS");
   return 0;
}
