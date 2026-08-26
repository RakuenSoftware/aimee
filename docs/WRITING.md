# Documentation must make the next decision easier

Completeness is not the hard part. Each claim must be current, sourced, and useful to the reader.
This guide adapts the project voice for reference documentation. Where a rule and a good sentence
disagree, the sentence wins and the rule gets fixed.

## Four things carry the voice

**Find the constraint that actually binds.** Say what the obvious account misses. A faster
deployment is not better if it weakens storage ownership or makes recovery ambiguous, so write about
ownership and recovery.

**Give the opposing option its strongest form.** State the case for the approach you are arguing
against in its own terms before answering it. An option weakened on the page is one a reader will
adopt anyway, without the warning you owed them.

**Make every abstraction a mechanism and a consequence.** An idea earns its place by changing what
someone can do, what something costs, or what happens next. Name the mechanism, then follow it to
the favourable consequence and the awkward one, in that order.

**Let no sentence know more than the work has earned.** Certainty is a relationship between a
sentence and what supports it. Measured facts land bare. Inference gets `probably` or `unlikely`.
A judgement gets `I think`, once, on the judgement itself. A hedge means the evidence is thinner,
never that the author is being polite.

**Criticism ships with the replacement.** Never leave a thing knocked down and nothing standing.
Give the safer command, design, or operating path in enough detail to be argued with, and no
further.

## Lead with the reader's task

State why the page exists before explaining mechanics. Put the common path first, then boundaries,
failure behavior, and recovery. Use short examples a reader can run or adapt.

Purpose before mechanics. Anchor a new thing to something the reader already has and quantify the
delta. Give the reasoning where the choice is not obvious, and state the tradeoff instead of hiding
it. Name the exceptions on a claim beside the claim.

## Headings do the work the page is for

A reader scans a reference page. `Storage`, `Principals`, and `Network ports` are the right
headings there, because a reader arrives knowing the word they are looking for.

A page that argues gets headings that carry the finding. Release notes, roadmaps, validation
reports, and troubleshooting entries all argue. `A deploy reported success and changed nothing` and
`Power behaviour: no real TDP, but wide range` both give a reader who reads only the headings the
map and the result, and the caveat belongs up there with them rather than three paragraphs down.

Where a section argues and its best available heading is a bare label, the section has not decided
what it found. Write that heading last.

## Length is a cost the reader pays

Every rule here asks for another sentence. Run all of them on every paragraph and the result is a
page three times the length of the work inside it. Each rule applies where it is load-bearing and
nowhere else. Clarity and brevity are the exception; they never stop applying.

- **A page is as long as its work and not one line longer.** Length is never evidence the thinking
  was done. Usually it is evidence the cutting was not.
- **One movement, then move on.** A paragraph states one claim, gives it the support it needs, and
  stops.
- **Say it once.** Repeat the operative term where the detail turns on it, then use a pronoun. Do
  not restate a point in fresh words because the first statement felt thin; fix it in place.
- **One worked example where one settles it.** Three is the ceiling, not the rate.
- **Cut the setup, not the turn.** Context the reader can infer and alternatives rejected early both
  go, unless the guidance turns on them.
- **Stop at the consequence.** Stop once the reader has a decision they can act on.
- **The second pass cuts.** Draft at whatever length comes out, then take a third away, out of the
  explaining rather than the evidence.

## Sentence mechanics

- **Short by default, varied hard.** A long chained sentence, then a short one that lands. The long
  one stops working once it is the norm.
- **Verbs carry the weight.** Adjectives are rationed and the ones spent are distinctive.
- **Active voice unless the hidden actor is the point.** Passive is where responsibility hides.
- **The shorter word, unless the longer one is more precise.** `Utilise`, `leverage`, and
  `facilitate` do not become precise by sounding formal.
- **Cut every word that survives its own deletion.** Read the sentence without it. If no meaning,
  rhythm, or pressure was lost, it was not there for the reader.
- **Use `you` for an action the reader takes.** Use `we` only for a deliberate project decision.
  Use `I` for a recommendation the reader is being asked to trust.

## What this voice never does

- **Em dashes as connective tissue.** The most reliable tell that a page was not written here. Use a
  full stop, a comma, a colon, or brackets. A doubled hyphen is the same habit in ASCII. Quoted
  material keeps whatever punctuation its source used, because correcting a quotation to a house
  rule is a provenance fault.
- **The negation that sets up an assertion.** `X is not A. It is B.`, `X, not Y`, `A rather than B`,
  `not X but Y`. One is a sentence. Six is the rhythm of the page, and by then the writing sounds
  like it is correcting a misconception nobody held. Say the thing positively and let the reader
  supply the contrast. Where the negative carries real information, give it a plain sentence with no
  assertion riding on its back.
- **Demonstrative paragraph openers.** `That is why`, `Which is why`, `This is the part that`. The
  link they announce is already carried by position on the page. Cut the opener and start with the
  subject. One `So` per section is the budget, and swapping every `That is why` for `So the`
  relocates the tic instead of removing it.
- **Hype adjectives.** `revolutionary`, `game-changing`, `seamless`, `powerful`, `robust`.
- **Intensifiers.** `very`, `extremely`, `incredibly`, `massively`, `highly`, `truly`.
- **Decorative questions.** A question is allowed when the reader can answer it and the guidance
  depends on their attempt. A topic announcement wearing a question mark is not. Test: if you
  supplied the answer yourself, would anything be lost?
- **Restating a point in different words.** If the second version is better, it replaces the first.
- **The paragraph that recaps before continuing.** Structure carries the argument.
- **Authority without a basis, and a hedge without a stated reason.**
- **Emoji.** A tick in a table cell is carrying a verdict; write the verdict.

## Keep evidence attached to claims

A number without provenance is decoration. Link the checked-in result, fixture, source constant, or
command that produced it. Record the commit, configuration, corpus, hardware, and sample count when
they can change the result. Label examples and deployment estimates as such.

An adjective is licensed by the number it sits beside. `a staggering 428 W` works because 428 W is
in the sentence; `staggering performance` does not. Spend that reaction once per page.

State an absence as yours: `we have not measured this`, not `it does not happen`. State a
disqualifying caveat beside the claim it limits, not in a later section.

## Say what does not work yet

A command that does not exist is worse than an undocumented one, because a reader plans around it.
Before documenting a command, config key, or environment variable, check it against the generated
reference or the binary's own help. Where a page describes a design that has not shipped, say so in
bold, in place, before the code block.

Present state first, flatly: what exists today, before what is coming. Ship the roadmap item with
its unsolved problem attached, and name the concrete failure mode if it stays unsolved. Calibrate
the difficulty honestly. Mark each date with its own confidence, because `expect` and `hoping` are
different words for a reason.

Revise in public. A corrected page needs no retraction notice and no apology.

## Keep current guidance separate from history

Put current commands and behavior in product guides. Keep design decisions under `docs/proposals/`
and point-in-time results under `docs/validation/` or `benchmarks/`. A completed proposal explains
why a decision was made; it does not override the current guide or code.

Files under `docs/gen/` come from source descriptors. Change the source and regenerate them. Never
repair generated Markdown by hand.

## Document conventions

- Follow the spelling already in the document you are editing, and in the files around it.
  Identifiers keep whatever spelling the code gave them, so prose sits next to a command name that
  may disagree with it.
- Bold marks numbers and rules, not emphasis for its own sake.
- Backticks for tool, file, and command names.
- Parallel things stay rigidly parallel, and the ones that break the pattern are flagged.
- Bullet items lead with a bolded imperative or noun phrase, never a bare list of nouns.
- Links inline and named, so the reader sees whose evidence it is before clicking.

## Use visuals only when they carry information

Add a diagram when relationships, ownership, or a sequence is harder to understand in prose. Prefer
SVG for stable architecture and process diagrams because it stays legible at different sizes.

Use a screenshot only when the exact interface matters. Capture it from the current build, remove
credentials and personal data, crop unrelated chrome, give it specific alt text, and update it with
the interface. Decorative images create maintenance work without helping the reader.

Store shared documentation assets under `docs/images/`. Use relative paths so local and hosted
renderers resolve them the same way.

## Keep the documentation navigable

Give each maintained page one H1 and descriptive headings. Link a new public guide from
[`docs/README.md`](README.md). Prefer one canonical explanation and link to it instead of copying a
table or procedure.

## Before you send a documentation change

```bash
make -C src docs-gen
python3 scripts/check-docs.py
git diff --check
```

`check-docs.py` enforces the mechanical rules on every maintained page: em dashes, the hype and
intensifier list, emoji and status marks, demonstrative openers, the negation frame, one H1 per
page, local link targets, and image alt text. It cannot check the rest, so read the rendered page as
well as the source and work through the list below.

- [ ] Does the page move attention from the obvious account to the constraint that binds?
- [ ] Does every abstraction become a mechanism and a consequence?
- [ ] Does each heading give its section's finding, where the section argues?
- [ ] Is every number attached to the thing that produced it?
- [ ] Does every command, key, and variable on the page exist in the shipped build?
- [ ] Are the author's limits and anything undecided stated in the text?
- [ ] Zero em dashes. Zero intensifiers. Zero hype adjectives. Zero emoji.
- [ ] Was a third cut on the second pass, out of the explaining rather than the evidence?

A link can exist and still point the reader to the wrong decision.
