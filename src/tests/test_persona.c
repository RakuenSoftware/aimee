/* test_persona.c: unit tests for the server-side persona registry. */
#include "persona.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_role(const persona_t *p, const char *role)
{
   for (int i = 0; i < p->roles_count; i++)
      if (strcmp(p->roles[i], role) == 0)
         return 1;
   return 0;
}

static void write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(content, f);
   fclose(f);
}

int main(void)
{
   printf("persona: ");

   /* Point config_default_dir() at a temp AIMEE_HOME so user-level personas and
    * install-defaults land in isolation. */
   char home[PATH_MAX];
   snprintf(home, sizeof(home), "%s/aimee-persona-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   platform_setenv("AIMEE_HOME", home);

   /* --- built-in metadata --- */
   {
      persona_t p;
      assert(persona_load(NULL, "engineer", &p) == 0);
      assert(p.builtin == 1);
      assert(has_role(&p, "code") && has_role(&p, "review"));
      assert(p.check_role[0] == '\0');          /* engineer has no done-gate */
      assert(p.persona_text == NULL);           /* built-in prose via enum path */
      assert(strcmp(p.delegates, "full") == 0); /* full delegate policy */
      assert(persona_delegates_enabled(&p) && persona_delegates_writes(&p));
      persona_free(&p);

      assert(persona_load(NULL, "novel", &p) == 0);
      assert(strcmp(p.check_role, "continuity") == 0);
      assert(strcmp(p.check_marker, "CONTINUITY") == 0);
      assert(has_role(&p, "continuity") && has_role(&p, "beat-check")); /* read-only roles */
      assert(!has_role(&p, "prose") && !has_role(&p, "line-edit"));     /* no write delegates */
      assert(strcmp(p.delegates, "readonly") == 0);
      assert(persona_delegates_enabled(&p) && !persona_delegates_writes(&p));
      assert(p.brief_text && strstr(p.brief_text, "world bible") != NULL);
      persona_free(&p);

      assert(persona_load(NULL, "songwriter", &p) == 0);
      assert(strcmp(p.delegates, "none") == 0); /* no delegates */
      assert(!persona_delegates_enabled(&p));
      assert(p.roles_count == 0); /* no delegate roles */
      persona_free(&p);

      /* --- reviewer built-ins: read-only, review-oriented roles, no gate --- */
      const char *reviewers[] = {"qa", "security", "reviewer", "architect",
                                 "reviewer-constructive"};
      for (int i = 0; i < 5; i++)
      {
         assert(persona_load(NULL, reviewers[i], &p) == 0);
         assert(p.builtin == 1);
         assert(strcmp(p.name, reviewers[i]) == 0);
         assert(p.persona_text == NULL);  /* built-in prose via enum path */
         assert(p.check_role[0] == '\0'); /* reviewers have no done-gate */
         assert(strcmp(p.delegates, "readonly") == 0);
         assert(persona_delegates_enabled(&p) && !persona_delegates_writes(&p));
         assert(has_role(&p, "review") && has_role(&p, "diagnose"));
         assert(!has_role(&p, "code") && !has_role(&p, "refactor")); /* no write roles */
         assert(p.brief_text && strstr(p.brief_text, "READ-ONLY delegates") != NULL);
         persona_free(&p);
      }

      /* reviewer-constructive is a DISTINCT lens from the contrarian reviewer:
       * its composed system prompt is constructive ("assess as written") and
       * differs from the contrarian reviewer's prose. */
      char *constructive = persona_compose_delegate_prompt("reviewer-constructive", NULL, NULL);
      char *contrarian = persona_compose_delegate_prompt("reviewer", NULL, NULL);
      assert(constructive && contrarian);
      assert(strstr(constructive, "constructive") != NULL);
      assert(strstr(constructive, "as written") != NULL);
      assert(strcmp(constructive, contrarian) != 0);
      free(constructive);
      free(contrarian);
   }

   /* --- unknown name falls back to engineer --- */
   {
      persona_t p;
      assert(persona_load(NULL, "does-not-exist", &p) == 0);
      assert(strcmp(p.name, "engineer") == 0);
      assert(p.builtin == 1);
      persona_free(&p);
   }

   /* --- custom user persona file overrides / defines --- */
   {
      char dir[PATH_MAX];
      snprintf(dir, sizeof(dir), "%s/personas", home);
      platform_mkdir_p(dir, 0755);
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/noir.md", dir);
      write_file(path, "---\n"
                       "name: noir\n"
                       "description: Hard-boiled detective narrator\n"
                       "roles: [prose, line-edit, continuity]\n"
                       "check_role: continuity\n"
                       "check_marker: CONTINUITY\n"
                       "---\n\n"
                       "## Persona\n"
                       "You are a hard-boiled detective novelist working in %s.\n\n"
                       "## Principles\n"
                       "# Craft Principles\n"
                       "- Keep the voice clipped.\n\n"
                       "## Brief\n"
                       "- Recall the case facts before writing.\n");

      persona_t p;
      assert(persona_load(NULL, "noir", &p) == 0);
      assert(p.builtin == 0);
      assert(strcmp(p.description, "Hard-boiled detective narrator") == 0);
      assert(strcmp(p.check_role, "continuity") == 0);
      assert(has_role(&p, "prose") && has_role(&p, "line-edit") && has_role(&p, "continuity"));
      assert(p.persona_text && strstr(p.persona_text, "hard-boiled detective novelist") != NULL);
      assert(p.persona_text && strstr(p.persona_text, "%s") != NULL); /* cwd placeholder kept */
      assert(p.principles_text && strstr(p.principles_text, "clipped") != NULL);
      assert(p.brief_text && strstr(p.brief_text, "case facts") != NULL);
      assert(strcmp(p.delegates, "full") == 0); /* unset -> default full */
      persona_free(&p);

      /* a custom persona with an explicit readonly policy */
      snprintf(path, sizeof(path), "%s/critic.md", dir);
      write_file(path, "---\nname: critic\ndescription: Read-only reviewer\n"
                       "delegates: readonly\nroles: [review, research]\n---\n\n"
                       "## Persona\nYou are a reviewer.\n");
      assert(persona_load(NULL, "critic", &p) == 0);
      assert(strcmp(p.delegates, "readonly") == 0);
      assert(persona_delegates_enabled(&p) && !persona_delegates_writes(&p));
      persona_free(&p);
   }

   /* --- persona_compose_delegate_prompt: assign a persona to a delegate --- */
   {
      const char *base = "ROLE TEMPLATE BODY";

      /* engineer: principles only, no identity prose, base preserved */
      char *eng = persona_compose_delegate_prompt("engineer", "/tmp/wd", base);
      assert(eng);
      assert(strstr(eng, "# Code Principles") == eng);
      assert(strstr(eng, base) != NULL);
      assert(strstr(eng, "You are") == NULL); /* engineer injects no identity prose */
      free(eng);

      /* security: review principles lead, then the security identity + base,
       * with the persona prose's %s substituted by cwd */
      char *sec = persona_compose_delegate_prompt("security", "/tmp/wd", base);
      assert(sec);
      assert(strstr(sec, "# Review Principles") == sec);
      assert(strstr(sec, "application-security reviewer") != NULL);
      assert(strstr(sec, "/tmp/wd") != NULL); /* %s -> cwd */
      assert(strstr(sec, base) != NULL);
      free(sec);

      /* qa is distinct from security (different identity prose) */
      char *qa = persona_compose_delegate_prompt("qa", "/tmp/wd", base);
      assert(qa && strstr(qa, "senior QA engineer") != NULL);
      assert(strstr(qa, "application-security reviewer") == NULL);
      free(qa);

      /* custom persona: its own identity + principles from the file */
      char *noir = persona_compose_delegate_prompt("noir", "/tmp/wd", base);
      assert(noir);
      assert(strstr(noir, "clipped") != NULL);                        /* custom principles */
      assert(strstr(noir, "hard-boiled detective novelist") != NULL); /* custom identity */
      assert(strstr(noir, "/tmp/wd") != NULL);                        /* %s -> cwd */
      free(noir);

      /* unknown persona falls back to engineer principles (no identity prose) */
      char *unk = persona_compose_delegate_prompt("nope-not-real", "/tmp/wd", base);
      assert(unk && strstr(unk, "# Code Principles") == unk && strstr(unk, "You are") == NULL);
      free(unk);

      /* NULL base is tolerated */
      char *nb = persona_compose_delegate_prompt("qa", "/tmp/wd", NULL);
      assert(nb && strstr(nb, "senior QA engineer") != NULL);
      free(nb);
   }

   /* --- list includes built-ins + the custom one --- */
   {
      char names[PERSONA_MAX_NAMES][PERSONA_NAME_MAX];
      int n = persona_list(NULL, names, PERSONA_MAX_NAMES);
      int eng = 0, nov = 0, song = 0, noir = 0, qa = 0, sec = 0, rev = 0, arch = 0;
      for (int i = 0; i < n; i++)
      {
         if (strcmp(names[i], "engineer") == 0)
            eng = 1;
         if (strcmp(names[i], "novel") == 0)
            nov = 1;
         if (strcmp(names[i], "songwriter") == 0)
            song = 1;
         if (strcmp(names[i], "noir") == 0)
            noir = 1;
         if (strcmp(names[i], "qa") == 0)
            qa = 1;
         if (strcmp(names[i], "security") == 0)
            sec = 1;
         if (strcmp(names[i], "reviewer") == 0)
            rev = 1;
         if (strcmp(names[i], "architect") == 0)
            arch = 1;
      }
      assert(eng && nov && song && noir);
      assert(qa && sec && rev && arch);
   }

   /* --- install-defaults writes the 3 built-ins, idempotent --- */
   {
      char dir[PATH_MAX];
      snprintf(dir, sizeof(dir), "%s/defaults", home);
      int w = persona_install_defaults(dir);
      assert(w == 8); /* engineer, novel, songwriter, qa, security, reviewer, architect,
                         reviewer-constructive */
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/novel.md", dir);
      FILE *f = fopen(path, "r");
      assert(f);
      char buf[4096];
      size_t r = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      buf[r] = '\0';
      assert(strstr(buf, "check_role: continuity") != NULL);
      assert(strstr(buf, "## Persona") != NULL);
      assert(strstr(buf, "fiction author and worldbuilder") != NULL);
      /* second call writes nothing (all exist) */
      assert(persona_install_defaults(dir) == 0);
   }

   /* --- persona_name_valid: filename-safe single segment --- */
   {
      assert(persona_name_valid("engineer"));
      assert(persona_name_valid("staff-eng_2.0"));
      assert(!persona_name_valid(""));
      assert(!persona_name_valid(NULL));
      assert(!persona_name_valid("../etc/passwd")); /* path traversal */
      assert(!persona_name_valid("a/b"));           /* separator */
      assert(!persona_name_valid(".hidden"));       /* leading dot */
      assert(!persona_name_valid("has space"));
   }

   /* --- persona_write + reload round-trip (custom persona) --- */
   {
      persona_t p;
      memset(&p, 0, sizeof(p));
      snprintf(p.name, sizeof(p.name), "%s", "staff-eng");
      snprintf(p.description, sizeof(p.description), "%s", "Staff engineer");
      snprintf(p.delegates, sizeof(p.delegates), "%s", "readonly");
      snprintf(p.roles[p.roles_count++], PERSONA_NAME_MAX, "%s", "review");
      snprintf(p.roles[p.roles_count++], PERSONA_NAME_MAX, "%s", "research");
      p.persona_text = strdup("You are a staff engineer in %s.");
      p.principles_text = strdup("# Principles\n- Think in systems.\n");
      assert(persona_write(&p) == 0);
      persona_free(&p);

      persona_t r;
      assert(persona_load(NULL, "staff-eng", &r) == 0);
      assert(r.builtin == 0);
      assert(strcmp(r.description, "Staff engineer") == 0);
      assert(strcmp(r.delegates, "readonly") == 0);
      assert(has_role(&r, "review") && has_role(&r, "research"));
      assert(r.persona_text && strstr(r.persona_text, "staff engineer") != NULL);
      assert(r.persona_text && strstr(r.persona_text, "%s") != NULL);
      assert(r.principles_text && strstr(r.principles_text, "systems") != NULL);
      persona_free(&r);

      /* delete removes it; reload no longer finds a file (unknown -> engineer) */
      assert(persona_delete("staff-eng") == 0);
      assert(persona_delete("staff-eng") == -1); /* gone */
      persona_t r2;
      assert(persona_load(NULL, "staff-eng", &r2) == 0);
      assert(strcmp(r2.name, "engineer") == 0); /* fell back */
      persona_free(&r2);
   }

   /* --- a seeded built-in is read FROM the file: config is the source --- */
   {
      persona_t p;
      memset(&p, 0, sizeof(p));
      snprintf(p.name, sizeof(p.name), "%s", "engineer");
      snprintf(p.description, sizeof(p.description), "%s", "Engineer (edited)");
      snprintf(p.delegates, sizeof(p.delegates), "%s", "full");
      p.persona_text = strdup("CUSTOM ENGINEER PROSE for %s");
      p.principles_text = strdup("# Edited Principles\n- one.\n");
      assert(persona_write(&p) == 0);
      persona_free(&p);

      persona_t r;
      assert(persona_load(NULL, "engineer", &r) == 0);
      assert(r.builtin == 1); /* still recognized as the engineer built-in */
      assert(r.persona_text && strstr(r.persona_text, "CUSTOM ENGINEER PROSE") != NULL);
      assert(r.principles_text && strstr(r.principles_text, "Edited Principles") != NULL);
      persona_free(&r);

      /* an engineer-persona delegate still gets principles-only framing even
       * when engineer.md exists (no identity prose injected). */
      char *eng = persona_compose_delegate_prompt("engineer", "/tmp/wd", "BASE");
      assert(eng);
      assert(strstr(eng, "Edited Principles") != NULL);     /* file principles used */
      assert(strstr(eng, "CUSTOM ENGINEER PROSE") == NULL); /* identity withheld */
      free(eng);

      /* removing the override reverts engineer to the code default */
      assert(persona_delete("engineer") == 0);
      assert(persona_load(NULL, "engineer", &r) == 0);
      assert(r.builtin == 1 && r.persona_text == NULL); /* back to enum fallback */
      persona_free(&r);
   }

   platform_test_rmrf(home);
   printf("OK\n");
   return 0;
}
