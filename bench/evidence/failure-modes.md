# Raw failure modes, as emitted

Model output samples taken directly from the prediction files, not paraphrased.
Each one is the first N rows of that model's run, so nothing is cherry-picked
past the ordering of the gold set. The point of this file is that 'scored
0.0000' covers at least five distinct failures, and only one of them is the
model being incapable.

## gemma-3-270m: returns the note instead of extracting from it

The only unambiguous total failure in the set. Valid JSON every time (parse
rate 1.00), schema rate 0.00: it echoes the note back under a `content` key and
never attempts a triple. No gate, cap or prompt change recovers this.

Source: `bench/tier-a/results/gpu/unsloth_gemma-3-270m-it.pred.jsonl`

```
note fp01  parse_ok=True schema_ok=False truncated=False completion_tokens=22
  '```json\n{"content": "I work for Rakuen Software as of this month."}\n```'
note fp02  parse_ok=True schema_ok=False truncated=False completion_tokens=21
  '```json\n{"content": "I live in Wellington now, moved back last year"}\n```'
note fp03  parse_ok=True schema_ok=False truncated=False completion_tokens=22
  '```json\n{"content": "I\'m the principal engineer on the retrieval team."}\n```'
```

## SmolLM2-360M: right fields, no wrapper

Emits a bare fact object rather than the `{"facts": [...]}` envelope the prompt
specifies. mf_commit_facts() requires a `facts` array, so this commits nothing
in production too. A real failure, and a narrow one: the field names are
correct.

Source: `bench/tier-a/results/gpu/HuggingFaceTB_SmolLM2-360M-Instruct.pred.jsonl`

```
note fp01  parse_ok=True schema_ok=False truncated=False completion_tokens=23
  '{"subject":"Rakuen Software","relation":"as of","object":"","confidence":0.0}'
note fp02  parse_ok=True schema_ok=False truncated=False completion_tokens=17
  '{"subject":"","relation":"","object":"","confidence":0.0}'
```

## LFM2.5-230M: correct relation and object, no subject

The envelope is right and the relations are drawn from the seed ontology
(`works_for`, `lives_in`, `located_in`). Every entry omits `subject`, which the
commit path requires. One missing field between this and a usable extraction.

Source: `bench/tier-a/results/gpu/LiquidAI_LFM2.5-230M.pred.jsonl`

```
note fp01  parse_ok=False schema_ok=False truncated=False completion_tokens=160
  '{"facts": [{"relation": "works_for", "object": "Rakuen Software"}, {"relation": "member_of", "object": "Rakuen Software"}, {"relation": "has_role", "object": "Software"}, {"relation": "spouse", "object": "Rakuen Software"}, {"relation": "knows", "object": "Rakuen Software"}, {"relation": "parent_of"... [492 chars total]'
note fp02  parse_ok=False schema_ok=False truncated=False completion_tokens=161
  '{"facts": [{"relation": "lives_in", "object": "Wellington"}, {"relation": "born_in", "object": "Wellington"}, {"relation": "located_in", "object": "Wellington"}, {"relation": "device_has_ip", "object": "Wellington"}, {"relation": "has_hostname", "object": "Wellington"}, {"relation": "has_hostname", ... [518 chars total]'
```

## granite-4.0-h-350m: correct extraction, JSON one brace short

The content is right. The framing is not: the object closes but the array and
envelope do not, so cJSON_Parse fails and production drops it too.
mf_commit_facts() takes the span from the first '{' to the last '}', which here
lands inside the fact object.

Source: `bench/tier-a/results/gpu/ibm-granite_granite-4.0-h-350m.pred.jsonl`

```
note fp01  parse_ok=False schema_ok=False truncated=False completion_tokens=34
  '{"facts": [{"subject": "user", "relation": "works_for", "object": "Rakuen Software", "confidence": 0.0}]'
note fp02  parse_ok=False schema_ok=False truncated=False completion_tokens=26
  '{"facts": [{"subject":"user","relation":"lives_in","object":"Wellington","confidence":0.9}]'
```

## LFM2-350M-Extract: repeats one fact to the token cap

An extraction-tuned model, and the shape is exactly right. It then repeats the
same fact until the completion budget runs out, so the JSON is unterminated and
the parse rate is 0.13. Production sets no repetition penalty, so this is what
the shipped configuration gets.

Source: `bench/tier-a/results/gpu/LiquidAI_LFM2-350M-Extract.pred.jsonl`

```
note fp01  parse_ok=False schema_ok=False truncated=True completion_tokens=512
  '{\n  "facts": [\n    {\n      "subject": "Rakuen Software",\n      "relation": "Employee",\n      "object": "Work",\n      "confidence": 0.8\n    },\n    {\n      "subject": "Rakuen Software",\n      "relation": "Employee",\n      "object": "Member",\n      "confidence": 0.7\n    },\n    {\n      "subject": "Rakue... [1640 chars total]'
note fp02  parse_ok=False schema_ok=False truncated=True completion_tokens=512
  '{\n  "facts": [\n    {\n      "subject": "I live in Wellington now",\n      "relation": "now",\n      "object": "moved back last year"\n    },\n    {\n      "subject": "I live in Wellington now",\n      "relation": "now",\n      "object": "moved back last year"\n    },\n    {\n      "subject": "I live in Welling... [1889 chars total]'
```

## Qwen3-0.6B: the config artefact, not a model failure

Schema rate 0.97, 72 triples extracted, and F1 0.0000 under the retired
MF_CONF_FLOOR because it copies the literal `"confidence":0.0` out of the
prompt's own schema example. Under the gate that actually ships (fact_grounded)
it scores 0.4058. See MEASUREMENT_LOG.md defect 17.

Source: `bench/tier-a/results/gpu/Qwen_Qwen3-0.6B.pred.jsonl`

```
note fp01  parse_ok=True schema_ok=True truncated=False completion_tokens=31
  '```json\n{"facts":[{"subject":"user","relation":"works_for","object":"Rakuen Software","confidence":0.0}]}\n```'
note fp02  parse_ok=True schema_ok=True truncated=False completion_tokens=29
  '```json\n{"facts":[{"subject":"user","relation":"located_in","object":"Wellington","confidence":0.0}]}\n```'
note fp03  parse_ok=True schema_ok=True truncated=False completion_tokens=29
  '```json\n{"facts":[{"subject":"user","relation":"works_for","object":"principal engineer","confidence":0.0}]}\n```'
```

## GLM-4.7-Flash on Vulkan: not language

Q6_K on a 7900 XTX under RADV. Emits the '?' character to the cap on all 70
notes. The server logged no warning and reported the model loaded normally.
Cause not yet established: quant, backend or GGUF. Not a model quality result
and must not be read as one.

Source: `bench/tier-a/results/challenger-254/GLM-4.7-Flash.q6.pred.jsonl`

```
note fp01  parse_ok=False schema_ok=False truncated=True completion_tokens=512
  '????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????... [512 chars total]'
note fp02  parse_ok=False schema_ok=False truncated=True completion_tokens=512
  '????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????... [512 chars total]'
```

