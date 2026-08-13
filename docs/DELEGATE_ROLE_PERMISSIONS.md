# Delegate role permissions

What a delegate may do is declared, not inferred.

A role holds a set of permissions. They are resolved once, when the delegate is
created, and carried for the life of the run. The mount reads them, the tool
allowlist reads them, the API reads them. None of those places works the answer
out again.

That last part is the whole design. A permission derived twice is a permission
that can disagree with itself, and this codebase has done it twice: a task type
re-classified from prompt keywords at every context refresh, so a long review was
handed execution-agent instructions partway through the job; and a role list
duplicated in two functions that drifted, so `--role reviewer` quietly held
powers `--role review` was denied.

Reads are implicit. Any delegate may read the checkout it was given and query
what aimee knows. A permission authorises leaving something behind, or reaching
further out. Nothing else.

## A permission has three parts

Name says what is authorised. Scope says which objects it covers. Enforcement
point says where it is evaluated.

```yaml
permissions:
  - tools
  - name: repo_write
    scopes: [/srv/repo-a, /srv/repo-b]
    enforced_at: mount
```

All three are declarable, and the vocabulary is open. A role may be composed of
permissions aimee has never heard of, bound to enforcement points you run.

## What ships

Four permissions, each bound to a default enforcement point.

| permission | grants | enforced at | binds a delegate holding `shell`? |
|---|---|---|:---:|
| `tools` | calling tools at all | tool dispatch | |
| `shell` | running commands (`bash`, `execute_script`) | tool dispatch | |
| `repo_write` | modifying the checkout | mount | **yes** |
| `knowledge_write` | mutating aimee's memory, code index and docs | API | **yes** |

The built-in roles:

| role | `tools` | `shell` | `repo_write` | `knowledge_write` |
|---|:---:|:---:|:---:|:---:|
| `code`, `refactor` | ● | ● | ● | ● |
| `execute`, `validate` | ● | ● | | ● |
| `diagnose` | ● | ● | | |
| `review` | ● | | | |
| `search`, `continuity`, `beat-check` | ● | | | ● |
| `explain`, `draft`, `summarize`, `format`, `plan`, `reason` | | | | ● |

Aliases resolve before permissions are read, so `reviewer` holds what `review`
holds.

## The brief does not change what a delegate may do

A task that says "read-only, do not edit anything" no longer makes a `code`
delegate read-only. It never really did. The wording changed one derivation and
not the others, so the same delegate could be planned read-only and mounted
writable, and which one you got depended on which code path asked.

**Pick the role that matches the powers you want.** A read-only run is a
`review`, `diagnose` or `explain` delegate. Wording a `code` brief carefully is
not a permission boundary, and treating it as one hid that the mount had already
been made writable.

The brief still shapes the work. It no longer shapes the powers.

A role that is neither built in nor defined at runtime holds nothing. An
unrecognised role is a question nobody answered, and "it probably just reads" is
still an answer nobody gave.

## Enforcement points are not interchangeable

A permission is as strong as its choke point. It holds when every path to the
effect passes through the thing doing the checking.

**`mount`** is the container's filesystem mount. The kernel decides, so there is
no way around it.

**`api`** is the server-side handler that performs the effect. Anything that has
to come back through aimee passes it, including calls from inside a container
holding the control socket.

**`tools`** is the tool dispatch allowlist. It binds a delegate that has no
shell, and only that one.

That third point is the one to hold on to. A delegate with `shell` reaches the
same effects by running commands, so a tool-layer rule cannot bind it. This is
why `repo_write` lives at the mount rather than removing `write_file` from a
toolset: with a shell available, the absence of a tool stops nothing.

`shell` itself is refused at dispatch, and deliberately not by choosing a toolset
without `bash`. Which toolset a role gets is a map keyed on the role name, so a
role you define without `shell` still resolves to whichever toolset its name
implies. The permission is checked whatever that map returns.

Scope inherits the same limit, because a scope is only real where the
enforcement point can see the object.

- **`repo_write` narrowed to a list of repositories is enforced.** The object
  matched is the repository the caller pointed the delegate at. Named one that is
  not on the list, or none at all, and the delegate runs read-only.
- **It is the only built-in that can be scoped, and the others are refused.**
  Nothing evaluates a scope for `knowledge_write` or `tools`, and `shell` cannot
  be scoped at the tool layer at all: a shell goes wherever the filesystem lets
  it. Writing one is an error that names what can be scoped, rather than a
  narrowing that silently does nothing.
- **A permission you define yourself may be scoped however your point evaluates
  it.** The scope is carried to you untouched; what it means is yours.

Scopes match exactly. A prefix rule would make `/srv/repo` grant
`/srv/repo-secrets`, and nobody writing the first means the second. A
subdirectory is a different object too: `/srv/repo` does not cover
`/srv/repo/sub`, so a delegate pointed at the subdirectory runs read-only. List
what you mean.

## Defining a role at runtime

A role is defined in its template's frontmatter, beside `max_turns`:

```markdown
---
max_turns: 40
permissions:
  - tools
  - shell
  - name: repo_write
    scopes: [/srv/repo-a, /srv/repo-b]
---

You are a delegate that ...
```

That role runs commands anywhere it can reach, and writes only those two
repositories, because only those two are mounted read-write.

Templates resolve project first, then user, then bundled:

| where | path |
|---|---|
| project | `.aimee/role_templates/<role>.md` |
| user | `~/.config/aimee/role_templates/<role>.md` |

**A definition replaces the built-in rather than adding to it.** "Composed of
these permissions" has to mean the list is the whole answer, or reading a
definition would not tell you what the role can do.

**A definition granting nothing is a deliberate powerless role.** It is not the
same as having no definition, which falls back to the built-in. A template with
no `permissions:` key has no definition.

**A block that cannot be read is refused, and the delegate does not run.** Not
partly applied, and not quietly replaced by the built-in set: either of those
would hand a delegate powers while you believe it holds the ones you wrote.
Unknown fields, a permission with no name, block-style scope lists and unclosed
brackets all fail this way.

**Write scopes in the flow form, `[a, b]`, and never empty.** A permission scoped
to nothing is a permission you did not grant. Leave it out instead.

**Say each permission once.** A name listed twice is refused, naming the name.
Merging them would mean choosing which line you meant, and the permissive choice
is the dangerous one: an unscoped mention beside a scoped one reads as a grant
over everything you had just narrowed away.

**There are limits, and passing one is a refusal rather than a trim.** A role may
hold up to **16** permissions with up to **8** scopes each, and a permission name
may be up to **63** characters. A definition that exceeds any of them fails to
resolve, and the delegate does not run: a set quietly shortened to fit would be a
delegate holding something other than what you wrote. The log line names the
limit and the value.

## Permissions are a ceiling, not a toolset

A role's permissions say what it may do. Its **toolset** says which tools it is
handed to do it with. They are different questions, and both are answered.

A template names its toolset in the same frontmatter:

```markdown
---
toolset: readonly
permissions:
  - tools
---
```

Resolution runs template, then the built-in map for the roles that ship, then
`readonly`. That last step matters: a role you define matches no built-in entry,
and the filter used to read "no toolset" as "do not filter", so a custom role was
handed every tool aimee has. **A role nobody described gets the set that can do
the least.** Ask for more by naming a toolset.

Whatever the toolset offers, the permissions clamp:

| withheld | tools withheld with it |
|---|---|
| `shell` | `bash`, `execute_script`, and the background-process tools |
| `repo_write` | `write_file`, `edit_file`, `git_commit`, `git_push`, `git_branch`, `git_pr` |
| `knowledge_write` | `create_note`, `record_attempt` |

So a role you define as `code` without `repo_write` does not keep `write_file`,
even though the name resolves to a toolset that carries it. The same list drives
what is advertised to the model and what dispatch will run, so a delegate is
never offered a tool that would be refused.

Tools not in that table need no permission beyond `tools`. Reading is implicit,
and a tool nobody has classified is not treated as privileged.

## Your own permissions and enforcement points

Declare permissions aimee does not ship, and bind them to points you operate:

```yaml
permissions:
  - tools
  - name: deploy
    scopes: [staging]
    enforced_at: deploy-gate
```

This is supported, and it is yours to make work. We guarantee the built-in
permissions and the points they are bound to. We do not guarantee that
`deploy-gate` exists, is reachable, is correct, or is consulted on every path to
a deployment. A point you supply is a control only when every route to the effect
passes through it, which is the test the built-in points have to meet.

What aimee owes you is the gap, stated:

**A permission with no enforcement point is reported, not assumed.**

Declare `deploy` with nothing bound to evaluate it and that permission is listed
as unenforced when the delegate is created. It is still carried. It is not
silently treated as granted, nor silently as denied, because both are guesses.

**`aimee roles show <role>` is where you read this.** It prints what the role
came to, not what you wrote: each permission with the point it is bound to, the
ones nothing enforces, and the tools the set withholds. The last two are
invisible in the frontmatter and are the ones that change what a delegate can
do.

```
Permissions:
  tools            enforced at tools
  deploy           enforced at deploy-gate

Nothing enforces: deploy
  These are carried and evaluated by no one, so they grant nothing and
  deny nothing. Bind a point that consults them, or drop them.

Tools withheld: bash write_file git_push
  Refused whatever toolset this role runs with.
```

A role whose permissions cannot be resolved says so there too, and a delegate for
it is refused rather than run holding nothing.

Three things decide whether a custom point is worth having.

- **Put it where you control every route.** In-process checks are defeated by a
  delegate holding `shell`. A mount, a network egress point, or an API the
  delegate must call are not.
- **Choose the failure mode deliberately.** Your evaluator will one day be slow,
  crashed, or absent. Failing closed stops work. Failing open makes the control
  theatre. Both are defensible; document which one you picked, where your
  operators will find it.
- **Scope an existing permission before inventing a new one.** `repo_write`
  narrowed to a repository list runs on machinery that already exists. A new
  permission needs machinery that does not.

## What this replaced

Per-role behaviour used to live in separate predicates, each with its own
hardcoded list: `RoleIsWrite`, `RoleEnablesToolsByDefault`,
`RoleSeesCurrentCodeOnly`, and the per-role toolset mapping. The defaults above
were extracted from those functions programmatically rather than transcribed, and
tests pin the equivalence, so adopting them changed no delegate's powers.

Two of them are worth keeping as cautionary tales.

`current_code_only` was named for a concern that containers made obsolete, a
stale index, while its live effect was something else: this delegate may read
aimee's state but not mutate it. That is now `knowledge_write`, named for what it
does.

Its enforcement was a substring match for `"aimee "` in shell command text. A
delegate defeats that by writing a script, by word-splitting, or by curling the
control socket the container was handed. `knowledge_write` is enforced at the
API, where the mutation lands.
