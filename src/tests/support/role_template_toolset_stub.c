/* No operator named a toolset, so the built-in map answers.
 *
 * For suites that link the tool filter (server/agent_tools.o) without the role
 * template reader. Filtering is what they are about; where a `toolset:`
 * frontmatter comes from is proved in unit-test-role-templates, which links the
 * real thing.
 *
 * A suite that wants to exercise an operator-named toolset must not link this:
 * define the stub locally with the answer it needs, or link role_templates.o. */

#include <stddef.h>

const char *role_template_toolset(const char *project_root, const char *role)
{
   (void)project_root;
   (void)role;
   return NULL;
}
