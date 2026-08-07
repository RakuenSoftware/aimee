/* xml_fallback_corpus.h: the shared input corpus for the tool-call rescue parser.
 *
 * One list, used twice: the C golden generator emits this parser's output for
 * every entry, and the Go port is asserted to reproduce that output exactly. A
 * port of a parser is only trustworthy against a differential, so the corpus
 * lives in one place rather than being retyped on each side.
 *
 * Entries are named so a differential failure says WHICH shape diverged.
 */
#ifndef AIMEE_TEST_XML_FALLBACK_CORPUS_H
#define AIMEE_TEST_XML_FALLBACK_CORPUS_H

typedef struct
{
   const char *name;
   const char *text;
   int allow_json;
} xml_corpus_entry_t;

static const xml_corpus_entry_t XML_CORPUS[] = {
    /* --- the explicit block form --- */
    {"basic", "<tool_call><name>bash</name><arguments>{\"command\":\"ls -la\"}</arguments>"
              "</tool_call>", 1},
    {"content_prefix", "Sure thing.<tool_call><name>bash</name><arguments>{\"a\":1}</arguments>"
                       "</tool_call>", 1},
    {"two_blocks", "<tool_call><name>bash</name><arguments>{\"a\":1}</arguments></tool_call>"
                   "<tool_call><name>read</name><arguments>{\"b\":2}</arguments></tool_call>", 1},
    {"content_between_blocks",
     "first<tool_call><name>bash</name><arguments>{\"a\":1}</arguments></tool_call>"
     "middle<tool_call><name>read</name><arguments>{\"b\":2}</arguments></tool_call>tail", 1},
    {"name_needs_trim",
     "<tool_call><name>  bash  </name><arguments>{\"a\":1}</arguments></tool_call>", 1},
    {"name_normalized_Bash",
     "<tool_call><name>Bash</name><arguments>{\"a\":1}</arguments></tool_call>", 1},
    {"name_normalized_dash",
     "<tool_call><name>Read-File</name><arguments>{\"a\":1}</arguments></tool_call>", 1},
    {"name_empty", "<tool_call><name></name><arguments>{\"a\":1}</arguments></tool_call>", 1},
    {"name_whitespace", "<tool_call><name>   </name><arguments>{\"a\":1}</arguments></tool_call>",
     1},
    {"name_missing", "<tool_call><arguments>{\"a\":1}</arguments></tool_call>", 1},
    {"args_missing", "<tool_call><name>bash</name></tool_call>", 1},
    {"args_empty", "<tool_call><name>bash</name><arguments></arguments></tool_call>", 1},
    {"args_not_object",
     "<tool_call><name>bash</name><arguments>\"plain string\"</arguments></tool_call>", 1},
    {"args_nested_braces",
     "<tool_call><name>bash</name><arguments>{\"o\":{\"i\":{\"deep\":1}}}</arguments></tool_call>",
     1},
    {"args_brace_in_string",
     "<tool_call><name>bash</name><arguments>{\"s\":\"a}b\"}</arguments></tool_call>", 1},
    {"unterminated_block", "<tool_call><name>bash</name><arguments>{\"a\":1}", 1},
    {"close_before_open", "</tool_call><tool_call><name>bash</name>"
                          "<arguments>{\"a\":1}</arguments></tool_call>", 1},
    {"namespaced_block", "<tools:tool_call><name>bash</name><arguments>{\"a\":1}</arguments>"
                         "</tools:tool_call>", 1},
    {"namespaced_short",
     "<ns:tool_call><name>bash</name><arguments>{\"a\":1}</arguments></ns:tool_call>", 1},

    /* --- the other explicit formats --- */
    {"invoke", "<invoke name=\"bash\"><parameter name=\"command\">ls -la</parameter></invoke>", 1},
    {"invoke_two",
     "<invoke name=\"bash\"><parameter name=\"command\">a</parameter></invoke>"
     "<invoke name=\"read\"><parameter name=\"path\">b</parameter></invoke>", 1},
    {"qwen_in_block", "<tool_call><function=bash>\n<parameter=command>\nls -la\n"
                      "</parameter>\n</function></tool_call>", 1},
    {"qwen_bare", "<function=bash><parameter=command>ls</parameter></function>", 1},
    {"channel", "<|channel>call:bash {\"command\": \"ls\"}", 1},
    {"channel_no_space", "<|channel>call:bash{\"command\":\"ls\"}", 1},
    {"channel_nested", "<|channel>call:bash {\"opts\": {\"deep\": true}}", 1},
    {"mistral", "[TOOL_CALLS][{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}]", 1},
    {"mistral_prose_before", "Sure, I will list them.\n[TOOL_CALLS]"
                             "[{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}]", 1},

    /* --- the JSON rescue, which is gated on the tool registry --- */
    {"json_known", "{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}", 1},
    {"json_unknown", "{\"name\": \"nope\", \"arguments\": {}}", 1},
    {"json_disallowed", "{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}", 0},
    {"json_prose_around", "Sure.\n{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}\nDone.",
     1},
    {"json_array", "[{\"name\": \"bash\", \"arguments\": {\"a\":1}}, "
                   "{\"name\": \"read\", \"arguments\": {\"b\":2}}]", 1},
    {"json_tool_parameters", "{\"tool\": \"bash\", \"parameters\": {\"command\": \"ls -la\"}}", 1},
    {"json_name_args", "{\"name\": \"bash\", \"args\": {\"command\": \"x\"}}", 1},
    {"json_fenced", "```json\n{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}\n```", 1},

    /* --- reasoning blocks --- */
    {"think_then_block", "<think>I should list files</think>"
                         "<tool_call><name>bash</name><arguments>{\"command\":\"ls\"}</arguments>"
                         "</tool_call>", 1},
    {"think_only", "<think>just reasoning, no call</think>plain answer", 1},
    {"think_bracket_form", "[THINK]reasoning[/THINK]"
                           "<tool_call><name>bash</name><arguments>{\"a\":1}</arguments>"
                           "</tool_call>", 1},
    {"think_then_json", "<think>plan</think>{\"name\": \"bash\", \"arguments\": {\"a\":1}}", 1},

    /* --- nothing to find --- */
    {"empty", "", 1},
    {"plain_prose", "Here is an explanation with no calls at all.", 1},
    {"prose_with_braces", "Use the {foo} placeholder in your config.", 1},
};

#define XML_CORPUS_COUNT ((int)(sizeof(XML_CORPUS) / sizeof(XML_CORPUS[0])))

#endif /* AIMEE_TEST_XML_FALLBACK_CORPUS_H */
