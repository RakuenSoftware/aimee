#!/usr/bin/env python3
# gen_config_surface.py: regenerate tests/test_config_surface.c from config.c.
# Parses config_load's aimee.yaml field reads (cJSON_GetObjectItemCaseSensitive
# chains) into JSON paths + types + range guards, and emits two YAML fixtures
# (A/B) with distinct in-range values plus per-field "cfgA != cfgB" assertions.
# Run from src/:  python3 tests/gen_config_surface.py   (add --diag to report
# all collisions instead of asserting). Re-run when config_load gains fields.
import re, sys, os
DIAG = "--diag" in sys.argv
lines=open("src/modules/config/config.c").read().split("\n")
import re as _re
_s=next(i for i,l in enumerate(lines) if l.startswith("int config_load(config_t *cfg)"))
_e=next(i for i in range(_s+1,len(lines)) if lines[i]=="}")
body="\n".join(lines[_s:_e+1])
assign_re=re.compile(r'(?:cJSON \*)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*cJSON_GetObjectItemCaseSensitive\(([A-Za-z_][A-Za-z0-9_]*),\s*"([^"]+)"\)')
fieldset_res=[
 ("bool", re.compile(r'cfg->([A-Za-z0-9_]+)\s*=\s*cJSON_IsTrue\(([A-Za-z_][A-Za-z0-9_]*)\)')),
 ("int",  re.compile(r'cfg->([A-Za-z0-9_]+)\s*=\s*\(int\)([A-Za-z_][A-Za-z0-9_]*)->valuedouble')),
 ("dbl",  re.compile(r'cfg->([A-Za-z0-9_]+)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)->valuedouble')),
 ("str",  re.compile(r'snprintf\(cfg->([A-Za-z0-9_]+),\s*sizeof[^,]*,\s*"%s",\s*([A-Za-z_][A-Za-z0-9_]*)->valuestring')),
]
b_both=re.compile(r'valuedouble\s*>=\s*([0-9.]+).*?valuedouble\s*<=\s*([0-9.]+)')
b_min =re.compile(r'valuedouble\s*>\s*([0-9.]+)')
vardef={}; tuples=[]
def resolve(var):
    parts=[];seen=set()
    while var in vardef and var not in seen:
        seen.add(var);parent,key=vardef[var];parts.append(key)
        if parent=="root": return list(reversed(parts))
        var=parent
    return None
last_bounds=None
for line in body.split("\n"):
    mb=b_both.search(line)
    if mb: last_bounds=(mb.group(1),mb.group(2))
    else:
        mn=b_min.search(line)
        if mn: last_bounds=(mn.group(1),None)
    m=assign_re.search(line)
    if m: vardef[m.group(1)]=(m.group(2),m.group(3)); continue
    hit=False
    for typ,rx in fieldset_res:
        fm=rx.search(line)
        if fm:
            path=resolve(fm.group(2))
            if path:
                tuples.append((tuple(path),fm.group(1),typ, last_bounds if typ in("int","dbl") else None))
            hit=True; break
    if hit: last_bounds=None
seen=set();uniq=[]
for t in tuples:
    if t[1] in seen: continue
    seen.add(t[1]);uniq.append(t)
def val(typ,which,b):
    if typ=="bool": return "true" if which=="A" else "false"
    if typ in ("int","dbl"):
        if b and b[1] is not None: return b[0] if which=="A" else b[1]
        if b and b[0] is not None:
            return (str(float(b[0])+1) if typ=="dbl" else str(int(float(b[0]))+1)) if which=="A" else ("4096" if typ=="int" else "0.99")
        if typ=="int": return "3" if which=="A" else "4096"
        return "0.01" if which=="A" else "0.99"
    return "ZZA_val" if which=="A" else "ZZB_val"
def build_tree():
    root={}
    for path,field,typ,b in uniq:
        d=root
        for p in path[:-1]: d=d.setdefault(p,{})
        d[path[-1]]=(field,typ,b)
    return root
tree=build_tree()
def emit(node,which,ind=0):
    o=[]
    for k,v in node.items():
        pad="  "*ind
        if isinstance(v,dict): o.append(f"{pad}{k}:"); o+=emit(v,which,ind+1)
        else:
            field,typ,b=v; o.append(f"{pad}{k}: {val(typ,which,b)}")
    return o
ya="\n".join(emit(tree,"A")); yb="\n".join(emit(tree,"B"))
def cstr(s): return s.replace("\\","\\\\").replace('"','\\"').replace("\n","\\n")
ca,cb=cstr(ya),cstr(yb)
A=[]
for path,field,typ,b in uniq:
    dotted=".".join(path)
    if typ=="bool": chk=f'cfgA.{field} == 1 && cfgB.{field} == 0'
    elif typ=="str": chk=f'strcmp(cfgA.{field}, cfgB.{field}) != 0'
    else: chk=f'cfgA.{field} != cfgB.{field}'
    if DIAG: A.append(f'   if (!({chk})) {{ printf("MISMATCH {field} ({dotted})\\n"); fails++; }}')
    else: A.append(f'   assert({chk});')
asserts="\n".join(A); N=len(uniq)
faildecl="   int fails = 0;\n" if DIAG else ""
failend=('   printf("diag: %d mismatches\\n", fails);\n   return 0;\n' if DIAG
         else f'   printf("all tests passed ({N} parsed fields)\\n");\n   return 0;\n')
hdr=('/* test_config_surface.c: characterization net for config_load\'s parse surface.\n'
     ' * AUTO-DERIVED from config.c. Two YAML fixtures (A/B) set every config_load-parsed\n'
     ' * field to distinct, in-range values; asserting cfgA != cfgB proves each field is\n'
     ' * read from aimee.yaml, pinning the surface so config_load can be split into\n'
     ' * config_parse_* section helpers without silently dropping a field. */\n')
inc=('#include <assert.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
     '#include <unistd.h>\n#include <sys/stat.h>\n#include "aimee.h"\n#include "platform_path.h"\n'
     '#include "platform_test_util.h"\n')
helper=('\nstatic void write_cfg(const char *home, const char *yaml)\n{\n'
        '   char p1[600], dir[600], path[700];\n'
        '   snprintf(p1, sizeof(p1), "%s/.config", home); mkdir(p1, 0755);\n'
        '   snprintf(dir, sizeof(dir), "%s/.config/aimee", home); mkdir(dir, 0755);\n'
        '   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);\n'
        '   FILE *f = fopen(path, "w"); assert(f); fputs(yaml, f); fclose(f);\n}\n')
main=('\nstatic const char *FIXTURE_A = "'+ca+'";\n'
      'static const char *FIXTURE_B = "'+cb+'";\n\n'
      'int main(void)\n{\n   printf("config_surface: ");\n'
      '   char tmpdir[512];\n'
      '   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-cfgsurf-XXXXXX", platform_tmpdir());\n'
      '   assert(platform_mkdtemp(tmpdir) != NULL);\n'
      '   platform_setenv("HOME", tmpdir);\n   platform_unsetenv("AIMEE_HOME");\n'
      '   platform_setenv("AIMEE_NO_CACHE", "1");\n'+faildecl+
      '\n   static config_t cfgA, cfgB;\n'
      '   write_cfg(tmpdir, FIXTURE_A); memset(&cfgA, 0, sizeof(cfgA)); config_load(&cfgA);\n'
      '   write_cfg(tmpdir, FIXTURE_B); memset(&cfgB, 0, sizeof(cfgB)); config_load(&cfgB);\n\n'
      +asserts+'\n\n'+failend+'}\n')
open("src/tests/test_config_surface.c","w").write(hdr+inc+helper+main)
print(f"generated ({'DIAG' if DIAG else 'ASSERT'}): {N} fields")
