/* code_treesitter.c: §2 tree-sitter extraction front-end. See code_treesitter.h.
 *
 * Real implementation under -DAIMEE_TREESITTER (links the fetched tree-sitter runtime +
 * grammars); otherwise a stub that reports "unavailable" so callers fall back to the
 * hand-rolled extractors and the default build needs no tree-sitter at all.
 *
 * Languages mirror the hand-rolled supported set (extractors.c): C, C++, C#, Python, Go,
 * JavaScript, TypeScript, Rust, Java, Ruby, PHP, Lua, Bash, Swift, Kotlin, Dart, CSS. Add
 * one by vendoring its grammar (scripts/fetch-treesitter.sh + src/Makefile), declaring its
 * entry point, mapping its extension(s) in ts_language_for_ext, adding a ts_lang_t, and a
 * classify_<lang> for its definition node types. */

#include "headers/code_treesitter.h"

#include <string.h>

#ifdef AIMEE_TREESITTER

#include "tree_sitter/api.h"
#include <stdio.h>

/* Grammar entry points (defined in the vendored parser.c files). */
const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_cpp(void);
const TSLanguage *tree_sitter_c_sharp(void);
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_go(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);
const TSLanguage *tree_sitter_rust(void);
const TSLanguage *tree_sitter_java(void);
const TSLanguage *tree_sitter_ruby(void);
const TSLanguage *tree_sitter_php(void);
const TSLanguage *tree_sitter_lua(void);
const TSLanguage *tree_sitter_bash(void);
const TSLanguage *tree_sitter_swift(void);
const TSLanguage *tree_sitter_kotlin(void);
const TSLanguage *tree_sitter_dart(void);
const TSLanguage *tree_sitter_css(void);
const TSLanguage *tree_sitter_scala(void);
const TSLanguage *tree_sitter_groovy(void);
const TSLanguage *tree_sitter_objc(void);
const TSLanguage *tree_sitter_elixir(void);
const TSLanguage *tree_sitter_powershell(void);

typedef enum
{
   TSL_C,
   TSL_CPP,
   TSL_CSHARP,
   TSL_PY,
   TSL_GO,
   TSL_JS,
   TSL_TS,
   TSL_RUST,
   TSL_JAVA,
   TSL_RUBY,
   TSL_PHP,
   TSL_LUA,
   TSL_BASH,
   TSL_SWIFT,
   TSL_KOTLIN,
   TSL_DART,
   TSL_CSS,
   TSL_SCALA,
   TSL_GROOVY,
   TSL_OBJC,
   TSL_ELIXIR,
   TSL_POWERSHELL
} ts_lang_t;

static const TSLanguage *ts_language_for_ext(const char *ext, ts_lang_t *which)
{
   if (!ext)
      return NULL;
   static const struct
   {
      const char *ext;
      ts_lang_t lang;
      const TSLanguage *(*fn)(void);
   } map[] = {
       {".c", TSL_C, tree_sitter_c},
       {".h", TSL_C, tree_sitter_c},
       {".cpp", TSL_CPP, tree_sitter_cpp},
       {".cc", TSL_CPP, tree_sitter_cpp},
       {".cxx", TSL_CPP, tree_sitter_cpp},
       {".hpp", TSL_CPP, tree_sitter_cpp},
       {".hh", TSL_CPP, tree_sitter_cpp},
       {".cs", TSL_CSHARP, tree_sitter_c_sharp},
       {".py", TSL_PY, tree_sitter_python},
       {".go", TSL_GO, tree_sitter_go},
       {".js", TSL_JS, tree_sitter_javascript},
       {".mjs", TSL_JS, tree_sitter_javascript},
       {".cjs", TSL_JS, tree_sitter_javascript},
       {".jsx", TSL_JS, tree_sitter_javascript},
       {".ts", TSL_TS, tree_sitter_typescript},
       {".mts", TSL_TS, tree_sitter_typescript},
       {".cts", TSL_TS, tree_sitter_typescript},
       {".tsx", TSL_TS, tree_sitter_tsx},
       {".rs", TSL_RUST, tree_sitter_rust},
       {".java", TSL_JAVA, tree_sitter_java},
       {".rb", TSL_RUBY, tree_sitter_ruby},
       {".php", TSL_PHP, tree_sitter_php},
       {".lua", TSL_LUA, tree_sitter_lua},
       {".sh", TSL_BASH, tree_sitter_bash},
       {".bash", TSL_BASH, tree_sitter_bash},
       {".zsh", TSL_BASH, tree_sitter_bash},
       {".swift", TSL_SWIFT, tree_sitter_swift},
       {".kt", TSL_KOTLIN, tree_sitter_kotlin},
       {".kts", TSL_KOTLIN, tree_sitter_kotlin},
       {".dart", TSL_DART, tree_sitter_dart},
       {".css", TSL_CSS, tree_sitter_css},
       {".scala", TSL_SCALA, tree_sitter_scala},
       {".sc", TSL_SCALA, tree_sitter_scala},
       {".groovy", TSL_GROOVY, tree_sitter_groovy},
       {".gradle", TSL_GROOVY, tree_sitter_groovy},
       /* .m is Objective-C here (also MATLAB's extension — ObjC is the intended
        * target; a MATLAB .m simply won't parse cleanly and falls through). */
       {".m", TSL_OBJC, tree_sitter_objc},
       {".mm", TSL_OBJC, tree_sitter_objc},
       {".ex", TSL_ELIXIR, tree_sitter_elixir},
       {".exs", TSL_ELIXIR, tree_sitter_elixir},
       {".ps1", TSL_POWERSHELL, tree_sitter_powershell},
       {".psm1", TSL_POWERSHELL, tree_sitter_powershell},
   };
   for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
      if (strcmp(ext, map[i].ext) == 0)
      {
         if (which)
            *which = map[i].lang;
         return map[i].fn();
      }
   return NULL;
}

int code_treesitter_available(const char *ext)
{
   return ts_language_for_ext(ext, NULL) != NULL;
}

/* Copy a node's source span into out[cap] (NUL-terminated, truncated to fit). */
static void node_text(TSNode node, const char *src, char *out, int cap)
{
   uint32_t s = ts_node_start_byte(node), e = ts_node_end_byte(node);
   int n = (int)(e - s);
   if (n < 0)
      n = 0;
   if (n > cap - 1)
      n = cap - 1;
   memcpy(out, src + s, (size_t)n);
   out[n] = '\0';
}

static int is_identifier_type(const char *t)
{
   return strcmp(t, "identifier") == 0 || strcmp(t, "type_identifier") == 0 ||
          strcmp(t, "field_identifier") == 0 || strcmp(t, "simple_identifier") == 0 ||
          strcmp(t, "constant") == 0 || strcmp(t, "name") == 0 ||
          strcmp(t, "property_identifier") == 0 ||
          /* PowerShell: the invoked command name behaves as the callee identifier */
          strcmp(t, "command_name") == 0;
}

/* Pre-order DFS for the first descendant whose type is one of the identifier kinds — used
 * to dig a name out of a C/C++ declarator (where the name nests under pointer/array/
 * function declarators). Recurses, so it must only be given the declarator subtree. */
static int first_identifier(TSNode node, TSNode *out)
{
   if (is_identifier_type(ts_node_type(node)))
   {
      *out = node;
      return 1;
   }
   uint32_t n = ts_node_named_child_count(node);
   for (uint32_t i = 0; i < n; i++)
      if (first_identifier(ts_node_named_child(node, i), out))
         return 1;
   return 0;
}

/* The first DIRECT identifier-like child. The name of a definition is always a direct
 * child (the deeper recursion of first_identifier would wrongly pick up identifiers
 * inside a preceding modifier/annotation, type-parameter list, or extension receiver). */
static int direct_identifier(TSNode node, TSNode *out)
{
   uint32_t n = ts_node_named_child_count(node);
   for (uint32_t i = 0; i < n; i++)
   {
      TSNode c = ts_node_named_child(node, i);
      if (is_identifier_type(ts_node_type(c)))
      {
         *out = c;
         return 1;
      }
   }
   return 0;
}

/* The name node for a definition: its `name` field if it has one, else the first direct
 * identifier-like child. Returns a possibly-null node (caller re-checks). */
static TSNode name_node(TSNode node)
{
   TSNode nm = ts_node_child_by_field_name(node, "name", 4);
   if (!ts_node_is_null(nm))
      return nm;
   TSNode id;
   if (direct_identifier(node, &id))
      return id;
   return nm; /* null */
}

/* Find a direct named child of a given type; returns 1 and sets *out on a match. */
static int child_of_type(TSNode node, const char *type, TSNode *out)
{
   uint32_t n = ts_node_named_child_count(node);
   for (uint32_t i = 0; i < n; i++)
   {
      TSNode c = ts_node_named_child(node, i);
      if (strcmp(ts_node_type(c), type) == 0)
      {
         *out = c;
         return 1;
      }
   }
   return 0;
}

/* Map a node type to a kind via two NULL-terminated type lists; sets *kind and returns 1
 * on a match. Used by the languages whose definition name lives in a `name` field. */
static int kind_lookup(const char *t, const char *const *fns, const char *const *types,
                       const char **kind)
{
   for (int i = 0; fns[i]; i++)
      if (strcmp(t, fns[i]) == 0)
      {
         *kind = "function";
         return 1;
      }
   for (int i = 0; types[i]; i++)
      if (strcmp(t, types[i]) == 0)
      {
         *kind = "type";
         return 1;
      }
   return 0;
}

/* C: the function name lives in the declarator (survives pointer/array wrapping), so dig
 * it out explicitly rather than via name_node (which would grab a named return type). */
static int classify_c(TSNode node, const char **kind, TSNode *name_root)
{
   const char *t = ts_node_type(node);
   if (strcmp(t, "function_definition") == 0)
   {
      TSNode d = ts_node_child_by_field_name(node, "declarator", 10);
      if (ts_node_is_null(d) || !first_identifier(d, name_root))
         return 0;
      *kind = "function";
      return 1;
   }
   if (strcmp(t, "declaration") == 0)
   {
      /* a top-level function prototype: declarator is a function_declarator. */
      TSNode d = ts_node_child_by_field_name(node, "declarator", 10);
      if (!ts_node_is_null(d) && strstr(ts_node_type(d), "function_declarator") &&
          first_identifier(d, name_root))
      {
         *kind = "function";
         return 1;
      }
      return 0;
   }
   if (strcmp(t, "type_definition") == 0)
   {
      TSNode d = ts_node_child_by_field_name(node, "declarator", 10);
      if (ts_node_is_null(d) || !first_identifier(d, name_root))
         return 0;
      *kind = "type"; /* typedef: the new name is the declarator */
      return 1;
   }
   if (strcmp(t, "struct_specifier") == 0 || strcmp(t, "union_specifier") == 0 ||
       strcmp(t, "enum_specifier") == 0)
   {
      TSNode nm = ts_node_child_by_field_name(node, "name", 4);
      if (ts_node_is_null(nm))
         return 0; /* anonymous */
      *kind = "type";
      *name_root = nm;
      return 1;
   }
   return 0;
}

/* C++: C's rules plus class_specifier; namespaces/templates are containers (see below). */
static int classify_cpp(TSNode node, const char **kind, TSNode *name_root)
{
   if (classify_c(node, kind, name_root))
      return 1;
   if (strcmp(ts_node_type(node), "class_specifier") == 0)
   {
      TSNode nm = ts_node_child_by_field_name(node, "name", 4);
      if (ts_node_is_null(nm))
         return 0;
      *kind = "type";
      *name_root = nm;
      return 1;
   }
   return 0;
}

/* Python: function/class definitions. A decorated_definition is descended through (see
 * is_descendable), so the inner def is classified here directly. */
static int classify_py(TSNode node, const char **kind, TSNode *name_root)
{
   const char *t = ts_node_type(node);
   if (strcmp(t, "function_definition") == 0)
      *kind = "function";
   else if (strcmp(t, "class_definition") == 0)
      *kind = "type";
   else
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* Go: func/method declarations and named types (type_spec inside a type_declaration). */
static int classify_go(TSNode node, const char **kind, TSNode *name_root)
{
   const char *t = ts_node_type(node);
   if (strcmp(t, "function_declaration") == 0 || strcmp(t, "method_declaration") == 0)
   {
      *kind = "function";
      *name_root = name_node(node);
      return 1;
   }
   if (strcmp(t, "type_declaration") == 0)
   {
      uint32_t n = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < n; i++)
      {
         TSNode spec = ts_node_named_child(node, i);
         const char *st = ts_node_type(spec);
         if (strcmp(st, "type_spec") == 0 || strcmp(st, "type_alias") == 0)
         {
            TSNode nm = ts_node_child_by_field_name(spec, "name", 4);
            if (!ts_node_is_null(nm))
            {
               *kind = "type";
               *name_root = nm;
               return 1;
            }
         }
      }
      return 0;
   }
   return 0;
}

/* JavaScript / TypeScript: function (incl. generator), class methods, arrow/function
 * expressions bound to a const/let/var or a class field, and class/interface/type/enum
 * declarations. `export`/`export default`, class bodies, and the const/let/var
 * declarations are descended through (see is_descendable), so this fires on the
 * declarator/declaration itself. */
static int classify_jsts(TSNode node, const char **kind, TSNode *name_root)
{
   const char *t = ts_node_type(node);
   /* `const f = () => {}`, `const g = function(){}`, or a class field `f = () => {}`:
    * a named binding whose value is a function expression — treat it as a function. */
   if (strcmp(t, "variable_declarator") == 0 || strcmp(t, "field_definition") == 0 ||
       strcmp(t, "public_field_definition") == 0)
   {
      TSNode val = ts_node_child_by_field_name(node, "value", 5);
      if (ts_node_is_null(val))
         return 0;
      const char *vt = ts_node_type(val);
      if (strcmp(vt, "arrow_function") != 0 && strcmp(vt, "function_expression") != 0 &&
          strcmp(vt, "function") != 0 && strcmp(vt, "generator_function") != 0)
         return 0;
      *kind = "function";
      /* variable_declarator's name is an identifier `name` field; a class field's name is a
       * property_identifier child (no `name` field), which name_node digs out. A destructuring
       * binding never reaches here — its value is not a function expression. */
      *name_root = name_node(node);
      return 1;
   }
   static const char *const F[] = {"function_declaration", "generator_function_declaration",
                                   "method_definition", NULL};
   static const char *const T[] = {"class_declaration", "interface_declaration",
                                   "type_alias_declaration", "enum_declaration", NULL};
   if (!kind_lookup(t, F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* Rust: functions (incl. trait method signatures) and the named type-like items. */
static int classify_rust(TSNode node, const char **kind, TSNode *name_root)
{
   static const char *const F[] = {"function_item", "function_signature_item", NULL};
   static const char *const T[] = {"struct_item", "enum_item",  "trait_item",
                                   "type_item",   "union_item", NULL};
   if (!kind_lookup(ts_node_type(node), F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* C#: types live under namespace_declaration → declaration_list (a container, descended
 * below), so this fires on the type/method nodes themselves. */
static int classify_csharp(TSNode node, const char **kind, TSNode *name_root)
{
   static const char *const F[] = {"method_declaration", "constructor_declaration",
                                   "local_function_statement", NULL};
   static const char *const T[] = {"class_declaration",
                                   "struct_declaration",
                                   "interface_declaration",
                                   "enum_declaration",
                                   "record_declaration",
                                   "delegate_declaration",
                                   NULL};
   if (!kind_lookup(ts_node_type(node), F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* Java: top-level types (methods are nested in class bodies — a follow-up). */
static int classify_java(TSNode node, const char **kind, TSNode *name_root)
{
   static const char *const F[] = {"method_declaration", "constructor_declaration", NULL};
   static const char *const T[] = {
       "class_declaration",  "interface_declaration",       "enum_declaration",
       "record_declaration", "annotation_type_declaration", NULL};
   if (!kind_lookup(ts_node_type(node), F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* Ruby: methods and class/module definitions. */
static int classify_ruby(TSNode node, const char **kind, TSNode *name_root)
{
   static const char *const F[] = {"method", "singleton_method", NULL};
   static const char *const T[] = {"class", "module", "singleton_class", NULL};
   if (!kind_lookup(ts_node_type(node), F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* PHP: functions/methods and class-like types. */
static int classify_php(TSNode node, const char **kind, TSNode *name_root)
{
   static const char *const F[] = {"function_definition", "method_declaration", NULL};
   static const char *const T[] = {"class_declaration", "interface_declaration",
                                   "trait_declaration", "enum_declaration", NULL};
   if (!kind_lookup(ts_node_type(node), F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* Lua: function declarations (the name field carries dotted names like `T.m`). */
static int classify_lua(TSNode node, const char **kind, TSNode *name_root)
{
   if (strcmp(ts_node_type(node), "function_declaration") != 0)
      return 0;
   TSNode nm = ts_node_child_by_field_name(node, "name", 4);
   if (ts_node_is_null(nm))
      return 0;
   *kind = "function";
   *name_root = nm;
   return 1;
}

/* Bash: function definitions. */
static int classify_bash(TSNode node, const char **kind, TSNode *name_root)
{
   if (strcmp(ts_node_type(node), "function_definition") != 0)
      return 0;
   *kind = "function";
   *name_root = name_node(node);
   return 1;
}

/* Swift: functions and class/struct/enum (all class_declaration) + protocols. */
static int classify_swift(TSNode node, const char **kind, TSNode *name_root)
{
   static const char *const F[] = {"function_declaration", "init_declaration", NULL};
   static const char *const T[] = {"class_declaration", "protocol_declaration", NULL};
   if (!kind_lookup(ts_node_type(node), F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* Kotlin: definitions expose no `name` field, so name_node digs the simple_identifier /
 * type_identifier out of the node. */
static int classify_kotlin(TSNode node, const char **kind, TSNode *name_root)
{
   static const char *const F[] = {"function_declaration", NULL};
   static const char *const T[] = {"class_declaration", "object_declaration", NULL};
   if (!kind_lookup(ts_node_type(node), F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* Dart: function signatures and class-like types. A class method is a method_signature
 * wrapping a function_signature; method_signature is descended through (see is_descendable)
 * so the function_signature inside carries the name. */
static int classify_dart(TSNode node, const char **kind, TSNode *name_root)
{
   static const char *const F[] = {"function_signature", NULL};
   static const char *const T[] = {"class_definition", "enum_declaration", "mixin_declaration",
                                   "extension_declaration", NULL};
   if (!kind_lookup(ts_node_type(node), F, T, kind))
      return 0;
   *name_root = name_node(node);
   return 1;
}

/* The name a CSS selector node introduces: its OWN class_name/id_name, which is the
 * last such child. Compound and combinator selectors nest (`a.btn` is a class_selector
 * holding a tag_name; `.a.b` is a class_selector holding a class_selector), so taking
 * the first would name the wrong one -- the walk reaches the nested selectors on its
 * own and each names itself. */
static int css_own_name(TSNode node, const char *want, TSNode *out)
{
   int found = 0;
   uint32_t n = ts_node_named_child_count(node);
   for (uint32_t i = 0; i < n; i++)
   {
      TSNode child = ts_node_named_child(node, i);
      if (strcmp(ts_node_type(child), want) == 0)
      {
         *out = child;
         found = 1;
      }
   }
   return found;
}

/* CSS's named, referenceable constructs: a class or id selector, which is what a
 * component's markup names to reach a style, and a @keyframes animation. A class is
 * exactly the kind of symbol the graph exists to join across files -- `.btn` defined in
 * a stylesheet and named by a component -- so it is emitted as a definition like any
 * other language's, rather than left to a CSS-only index.
 *
 * Kinds stay inside the vocabulary code_projection maps: "class" and "type" both become
 * a struct-like node there. A tag or attribute selector names nothing of its own and is
 * not a definition. */
static int classify_css(TSNode node, const char **kind, TSNode *name_root)
{
   const char *type = ts_node_type(node);

   if (strcmp(type, "keyframes_statement") == 0)
   {
      TSNode nm;
      if (!child_of_type(node, "keyframes_name", &nm))
         return 0;
      *kind = "type";
      *name_root = nm;
      return 1;
   }
   if (strcmp(type, "class_selector") == 0)
   {
      if (!css_own_name(node, "class_name", name_root))
         return 0;
      *kind = "class";
      return 1;
   }
   if (strcmp(type, "id_selector") == 0)
   {
      if (!css_own_name(node, "id_name", name_root))
         return 0;
      *kind = "class";
      return 1;
   }
   return 0;
}

/* Scala: `def` → function; class/object/trait/type/enum → type. The name comes via
 * name_node (the `name` field, else a direct identifier child). Members live in a
 * template_body (descended below). */
static int classify_scala(TSNode node, const char **kind, TSNode *name_root)
{
   const char *t = ts_node_type(node);
   static const char *const fns[] = {"function_definition", "function_declaration", NULL};
   static const char *const types[] = {"class_definition", "object_definition", "trait_definition",
                                       "type_definition",  "enum_definition",   NULL};
   if (kind_lookup(t, fns, types, kind))
   {
      *name_root = name_node(node);
      return 1;
   }
   return 0;
}

/* Groovy: function_definition/declaration → function, but the name is in the
 * `function` field (a preceding `type` field would fool name_node). class_definition
 * → type (its name is a plain `name` field). Class members live in a `closure` body
 * (descended below). */
static int classify_groovy(TSNode node, const char **kind, TSNode *name_root)
{
   const char *t = ts_node_type(node);
   if (strcmp(t, "function_definition") == 0 || strcmp(t, "function_declaration") == 0)
   {
      TSNode fn = ts_node_child_by_field_name(node, "function", 8);
      if (ts_node_is_null(fn))
         return 0;
      *name_root = fn;
      *kind = "function";
      return 1;
   }
   if (strcmp(t, "class_definition") == 0)
   {
      *kind = "type";
      *name_root = name_node(node);
      return 1;
   }
   return 0;
}

/* Objective-C / Objective-C++: C function_definition (declarator-based, like C);
 * a method_definition's selector is a direct identifier (simple methods; keyword/
 * multi-arg selectors are a known gap); @interface/@implementation/@protocol name
 * is the first identifier child (the superclass/category come after). Members live
 * in the class/implementation body (descended below); ObjC message sends are
 * handled as calls via message_expression's `method` field. */
static int classify_objc(TSNode node, const char **kind, TSNode *name_root)
{
   const char *t = ts_node_type(node);
   if (strcmp(t, "function_definition") == 0)
   {
      TSNode d = ts_node_child_by_field_name(node, "declarator", 10);
      if (ts_node_is_null(d) || !first_identifier(d, name_root))
         return 0;
      *kind = "function";
      return 1;
   }
   if (strcmp(t, "method_definition") == 0)
   {
      if (!direct_identifier(node, name_root))
         return 0;
      *kind = "function";
      return 1;
   }
   if (strcmp(t, "class_interface") == 0 || strcmp(t, "class_implementation") == 0 ||
       strcmp(t, "protocol_declaration") == 0)
   {
      if (!direct_identifier(node, name_root))
         return 0;
      *kind = "type";
      return 1;
   }
   return 0;
}

/* Elixir: def/defmodule are MACRO CALLS, not definition nodes. A `call` whose head
 * identifier is def/defp/defmacro(p) defines a function; defmodule/defprotocol/defimpl
 * defines a module (type). Unlike every other classifier this needs the source text
 * (to read the macro keyword), so classify() threads `content` through to here. The
 * defined name lives in the first `arguments` child: an alias for a module, a nested
 * call's identifier for `def foo(x)`, or a bare identifier for a no-arg `def foo`. */
static int classify_elixir(TSNode node, const char *content, const char **kind, TSNode *name_root)
{
   if (strcmp(ts_node_type(node), "call") != 0 || ts_node_named_child_count(node) == 0)
      return 0;
   TSNode head = ts_node_named_child(node, 0);
   if (strcmp(ts_node_type(head), "identifier") != 0)
      return 0;
   char macro[16];
   node_text(head, content, macro, (int)sizeof(macro));
   int is_fn = strcmp(macro, "def") == 0 || strcmp(macro, "defp") == 0 ||
               strcmp(macro, "defmacro") == 0 || strcmp(macro, "defmacrop") == 0;
   int is_mod = strcmp(macro, "defmodule") == 0 || strcmp(macro, "defprotocol") == 0 ||
                strcmp(macro, "defimpl") == 0;
   if (!is_fn && !is_mod)
      return 0;
   TSNode args;
   if (!child_of_type(node, "arguments", &args) || ts_node_named_child_count(args) == 0)
      return 0;
   TSNode first = ts_node_named_child(args, 0);
   const char *ft = ts_node_type(first);
   if (is_mod)
   {
      /* module name is an alias (Foo, Foo.Bar) */
      *name_root = first;
      *kind = "type";
      return 1;
   }
   /* function: `def foo(x)` -> nested call whose first id is the name; `def foo` -> id */
   if (strcmp(ft, "call") == 0)
   {
      if (ts_node_named_child_count(first) == 0)
         return 0;
      *name_root = ts_node_named_child(first, 0);
   }
   else if (strcmp(ft, "identifier") == 0)
   {
      *name_root = first;
   }
   else
   {
      return 0; /* guarded defs (when clauses) etc. — a known gap */
   }
   *kind = "function";
   return 1;
}

/* PowerShell: function_statement/class_method_definition → function, class_statement
 * → type. The name is a specific child node type (function_name or simple_name), not a
 * `name` field or a generic identifier, so pull it with child_of_type. Command
 * invocations are calls (see is_call_node/is_identifier_type for command_name). */
static int classify_powershell(TSNode node, const char **kind, TSNode *name_root)
{
   const char *t = ts_node_type(node);
   const char *name_type;
   if (strcmp(t, "function_statement") == 0)
   {
      *kind = "function";
      name_type = "function_name";
   }
   else if (strcmp(t, "class_method_definition") == 0)
   {
      *kind = "function";
      name_type = "simple_name";
   }
   else if (strcmp(t, "class_statement") == 0)
   {
      *kind = "type";
      name_type = "simple_name";
   }
   else
      return 0;
   return child_of_type(node, name_type, name_root);
}

static int classify(ts_lang_t lang, TSNode node, const char *content, const char **kind,
                    TSNode *name_root)
{
   switch (lang)
   {
   case TSL_C:
      return classify_c(node, kind, name_root);
   case TSL_CPP:
      return classify_cpp(node, kind, name_root);
   case TSL_CSHARP:
      return classify_csharp(node, kind, name_root);
   case TSL_PY:
      return classify_py(node, kind, name_root);
   case TSL_GO:
      return classify_go(node, kind, name_root);
   case TSL_JS:
   case TSL_TS:
      return classify_jsts(node, kind, name_root);
   case TSL_RUST:
      return classify_rust(node, kind, name_root);
   case TSL_JAVA:
      return classify_java(node, kind, name_root);
   case TSL_RUBY:
      return classify_ruby(node, kind, name_root);
   case TSL_PHP:
      return classify_php(node, kind, name_root);
   case TSL_LUA:
      return classify_lua(node, kind, name_root);
   case TSL_BASH:
      return classify_bash(node, kind, name_root);
   case TSL_SWIFT:
      return classify_swift(node, kind, name_root);
   case TSL_KOTLIN:
      return classify_kotlin(node, kind, name_root);
   case TSL_DART:
      return classify_dart(node, kind, name_root);
   case TSL_CSS:
      return classify_css(node, kind, name_root);
   case TSL_SCALA:
      return classify_scala(node, kind, name_root);
   case TSL_GROOVY:
      return classify_groovy(node, kind, name_root);
   case TSL_OBJC:
      return classify_objc(node, kind, name_root);
   case TSL_ELIXIR:
      return classify_elixir(node, content, kind, name_root);
   case TSL_POWERSHELL:
      return classify_powershell(node, kind, name_root);
   }
   return 0;
}

/* Node types we descend THROUGH to reach the definitions inside: organizational wrappers
 * (namespaces, export/decorator wrappers, C++ templates/extern blocks) and the member
 * bodies of types (class/struct/interface/trait/impl/enum bodies). We do NOT list function
 * bodies, type aliases, or typedefs — descending a typedef would re-emit its inner struct
 * tag, and descending a function body would surface locals. Because the walk also stops at
 * any matched FUNCTION (see visit), method bodies are never descended either. */
static int is_descendable(const char *t)
{
   static const char *const set[] = {
       /* organizational wrappers */
       "namespace_declaration", "file_scoped_namespace_declaration", "namespace_definition",
       "declaration_list", "template_declaration", "linkage_specification", "decorated_definition",
       "export_statement", "lexical_declaration", "variable_declaration",
       /* C/C++/C# preprocessor conditionals — top-level defs are commonly wrapped in platform
        * or feature guards (#ifdef AIMEE_POSIX, #if DEBUG); descend them so those defs are not
        * lost (function bodies are still never descended, so this stays safe). */
       "preproc_ifdef", "preproc_if", "preproc_else", "preproc_elif", "preproc_elifdef",
       /* member bodies */
       "class_body", "field_declaration_list", "body_statement", "enum_body",
       "enum_body_declarations", "interface_body", "block", "method_signature",
       /* member-containing type nodes (descended to reach their bodies; also emitted by
        * classify where they are named) */
       "class_declaration", "class_specifier", "struct_specifier", "union_specifier",
       "class_definition", "class", "interface_declaration", "trait_item", "impl_item", "mod_item",
       "trait_declaration", "object_declaration", "protocol_declaration", "struct_declaration",
       "record_declaration", "mixin_declaration", "extension_declaration", "enum_declaration",
       "enum_specifier", "enum_item",
       /* Scala: object/trait bodies + the shared template_body that holds members */
       "object_definition", "trait_definition", "template_body", "enum_definition",
       /* Groovy: class body is a closure */
       "closure",
       /* Objective-C: @interface/@implementation/@protocol bodies */
       "class_interface", "class_implementation", "implementation_definition",
       "protocol_declaration",
       /* Elixir: a module is a `call` (defmodule …) whose members live in its do_block;
        * both must be descended. Safe: visit() halts on any matched function, so a
        * `def`'s body (also a do_block) is never descended. */
       "call", "do_block",
       /* PowerShell: top-level defs are wrapped in a statement_list under `program`;
        * class bodies hold their method definitions */
       "statement_list", "class_statement",
       /* CSS: a rule's selectors hold the class and id names it defines, and at-rules
        * hold rules. The compound and combinator selectors are descended because they
        * nest -- `.card .btn` is a descendant_selector of two class_selectors, and both
        * are names the stylesheet defines. These types belong to no other grammar. */
       "rule_set", "selectors", "class_selector", "id_selector", "descendant_selector",
       "child_selector", "sibling_selector", "adjacent_sibling_selector", "namespace_selector",
       "pseudo_class_selector", "pseudo_element_selector", "media_statement", "supports_statement",
       "scope_statement", "at_rule", NULL};
   for (int i = 0; set[i]; i++)
      if (strcmp(t, set[i]) == 0)
         return 1;
   return 0;
}

/* Emit `node` if it is a definition, then descend into it to surface nested definitions
 * (members of a type, types in a namespace) — but never into a function body. */
static void visit(ts_lang_t lang, TSNode node, const char *content, definition_t *out, int max,
                  int *count)
{
   if (*count >= max)
      return;
   const char *kind = NULL;
   TSNode name_root;
   int matched = classify(lang, node, content, &kind, &name_root);
   if (matched && !ts_node_is_null(name_root))
   {
      node_text(name_root, content, out[*count].name, (int)sizeof(out[*count].name));
      if (out[*count].name[0])
      {
         snprintf(out[*count].kind, sizeof(out[*count].kind), "%s", kind);
         out[*count].line = (int)ts_node_start_point(node).row + 1;
         out[*count].line_end = (int)ts_node_end_point(node).row + 1;
         (*count)++;
      }
   }
   if (matched && strcmp(kind, "function") == 0)
      return; /* a function: emit it but never descend into its body */
   if (is_descendable(ts_node_type(node)))
   {
      uint32_t n = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < n && *count < max; i++)
         visit(lang, ts_node_named_child(node, i), content, out, max, count);
   }
}

int code_treesitter_definitions(const char *ext, const char *content, definition_t *out, int max)
{
   ts_lang_t which;
   const TSLanguage *lang = ts_language_for_ext(ext, &which);
   if (!lang || !content || !out || max <= 0)
      return -1;

   TSParser *parser = ts_parser_new();
   if (!parser)
      return -1;
   if (!ts_parser_set_language(parser, lang))
   {
      ts_parser_delete(parser);
      return -1;
   }
   TSTree *tree = ts_parser_parse_string(parser, NULL, content, (uint32_t)strlen(content));
   if (!tree)
   {
      ts_parser_delete(parser);
      return -1;
   }

   TSNode root = ts_tree_root_node(tree);
   int count = 0;
   uint32_t top = ts_node_named_child_count(root);
   for (uint32_t i = 0; i < top && count < max; i++)
      visit(which, ts_node_named_child(root, i), content, out, max, &count);

   ts_tree_delete(tree);
   ts_parser_delete(parser);
   return count;
}

/* ---- call edges (caller -> callee) ------------------------------------------------- */

/* A node carrying the type arguments of a generic/template call (`<IFoo>`, `<T>`, the
 * Rust turbofish `::<u32>`). The call NAME precedes it, so the callee-name search must
 * not descend into it — otherwise a generic call records the type argument as the callee
 * (`GetService<IFoo>()` -> IFoo, `obj.run<T>()` -> T). */
static int is_type_arguments(const char *t)
{
   return strcmp(t, "type_argument_list") == 0 || strcmp(t, "type_arguments") == 0 ||
          strcmp(t, "template_argument_list") == 0 || strcmp(t, "type_parameters") == 0;
}

/* Reverse pre-order DFS for the LAST identifier-like descendant — the called name in a
 * callee expression (`obj.m` -> m, `a::b::c` -> c, `g` -> g), skipping type-argument
 * lists so a generic call resolves to the method name, not a type argument. */
static int last_identifier(TSNode node, TSNode *out)
{
   const char *t = ts_node_type(node);
   if (is_type_arguments(t))
      return 0;
   if (is_identifier_type(t))
   {
      *out = node;
      return 1;
   }
   uint32_t n = ts_node_named_child_count(node);
   for (uint32_t i = n; i-- > 0;)
      if (last_identifier(ts_node_named_child(node, i), out))
         return 1;
   return 0;
}

/* Call/invocation node types across the grammars. */
static int is_call_node(const char *t)
{
   return strcmp(t, "call_expression") == 0 || strcmp(t, "call") == 0 ||
          strcmp(t, "method_invocation") == 0 || strcmp(t, "invocation_expression") == 0 ||
          strcmp(t, "function_call") == 0 || strcmp(t, "juxt_function_call") == 0 ||
          strcmp(t, "message_expression") == 0 || strcmp(t, "command") == 0 ||
          strcmp(t, "function_call_expression") == 0 || strcmp(t, "member_call_expression") == 0 ||
          strcmp(t, "scoped_call_expression") == 0 ||
          strcmp(t, "nullsafe_member_call_expression") == 0 || strcmp(t, "macro_invocation") == 0 ||
          strcmp(t, "object_creation_expression") == 0;
}

/* CSS's reference is `var(--token)`, and the name it references is the argument rather
 * than the function: recording `var` would make every custom-property reference in a
 * stylesheet the same edge, which is no edge at all. Other CSS functions (rgb, calc,
 * url) name no symbol and are not references. Returns 0 if this is not one. */
static int css_call_callee_name(TSNode call, const char *content, char *out, int cap)
{
   TSNode fn;
   if (!child_of_type(call, "function_name", &fn))
      return 0;
   char name[32];
   node_text(fn, content, name, (int)sizeof(name));
   if (strcmp(name, "var") != 0)
      return 0;

   TSNode arguments;
   if (!child_of_type(call, "arguments", &arguments))
      return 0;
   uint32_t n = ts_node_named_child_count(arguments);
   for (uint32_t i = 0; i < n; i++)
   {
      TSNode argument = ts_node_named_child(arguments, i);
      node_text(argument, content, out, cap);
      /* The first argument is the property; a second is the fallback value. */
      if (out[0] == '-' && out[1] == '-')
         return 1;
      return 0;
   }
   return 0;
}

/* The called name: the callee expression (the `function`/`name`/`method` field, else the
 * first child) reduced to its last identifier. Returns 0 if none. */
static int call_callee_name(TSNode call, const char *content, char *out, int cap)
{
   TSNode fn = ts_node_child_by_field_name(call, "function", 8);
   if (ts_node_is_null(fn))
      fn = ts_node_child_by_field_name(call, "name", 4);
   if (ts_node_is_null(fn))
      fn = ts_node_child_by_field_name(call, "method", 6);
   if (ts_node_is_null(fn))
   {
      if (ts_node_named_child_count(call) == 0)
         return 0;
      fn = ts_node_named_child(call, 0); /* Swift/Ruby: callee is the first child */
   }
   TSNode id;
   if (is_identifier_type(ts_node_type(fn)))
      id = fn;
   else if (!last_identifier(fn, &id))
      return 0;
   node_text(id, content, out, cap);
   return out[0] != '\0';
}

/* Walk the whole tree (function bodies included, unlike definitions) tracking the enclosing
 * function as the caller; emit an edge at every call node. */
static void walk_calls(ts_lang_t lang, TSNode node, const char *content, const char *caller,
                       call_ref_t *out, int max, int *count)
{
   if (*count >= max)
      return;
   const char *kind = NULL;
   TSNode nm;
   const char *child_caller = caller;
   char buf[sizeof(out->caller)];
   int is_def = classify(lang, node, content, &kind, &nm);
   if (is_def && kind && strcmp(kind, "function") == 0 && !ts_node_is_null(nm))
   {
      node_text(nm, content, buf, (int)sizeof(buf));
      if (buf[0])
         child_caller = buf; /* calls in this subtree are attributed to this function */
   }
   /* !is_def: an Elixir `def foo` is itself a `call` node — never emit a definition as a
    * call (its signature's inner call is skipped for the same reason). */
   if (is_call_node(ts_node_type(node)) && !is_def)
   {
      char callee[sizeof(out->callee)];
      int named = lang == TSL_CSS ? css_call_callee_name(node, content, callee, (int)sizeof(callee))
                                  : call_callee_name(node, content, callee, (int)sizeof(callee));
      if (named && callee[0])
      {
         snprintf(out[*count].caller, sizeof(out[*count].caller), "%s", caller ? caller : "");
         snprintf(out[*count].callee, sizeof(out[*count].callee), "%s", callee);
         out[*count].line = (int)ts_node_start_point(node).row + 1;
         (*count)++;
         if (*count >= max)
            return;
      }
   }
   uint32_t n = ts_node_named_child_count(node);
   for (uint32_t i = 0; i < n && *count < max; i++)
      walk_calls(lang, ts_node_named_child(node, i), content, child_caller, out, max, count);
}

int code_treesitter_calls(const char *ext, const char *content, call_ref_t *out, int max)
{
   ts_lang_t which;
   const TSLanguage *lang = ts_language_for_ext(ext, &which);
   if (!lang || !content || !out || max <= 0)
      return -1;
   if (which == TSL_BASH)
      return -1; /* no useful call extraction — defer to the hand-rolled path */

   TSParser *parser = ts_parser_new();
   if (!parser)
      return -1;
   if (!ts_parser_set_language(parser, lang))
   {
      ts_parser_delete(parser);
      return -1;
   }
   TSTree *tree = ts_parser_parse_string(parser, NULL, content, (uint32_t)strlen(content));
   if (!tree)
   {
      ts_parser_delete(parser);
      return -1;
   }

   int count = 0;
   walk_calls(which, ts_tree_root_node(tree), content, "", out, max, &count);
   ts_tree_delete(tree);
   ts_parser_delete(parser);
   return count;
}

#else /* !AIMEE_TREESITTER */

int code_treesitter_available(const char *ext)
{
   (void)ext;
   return 0;
}

int code_treesitter_definitions(const char *ext, const char *content, definition_t *out, int max)
{
   (void)ext;
   (void)content;
   (void)out;
   (void)max;
   return -1;
}

int code_treesitter_calls(const char *ext, const char *content, call_ref_t *out, int max)
{
   (void)ext;
   (void)content;
   (void)out;
   (void)max;
   return -1;
}

#endif
