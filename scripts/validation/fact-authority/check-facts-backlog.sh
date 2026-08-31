#!/bin/bash
# What the now-unconditional memory_facts drain is working through.
#
# Retiring config.typed_facts_enabled means kb_memory_facts_drain() actually
# runs. On a container that has been up since before the change, the jobs
# memory.store enqueued while the gate was OFF are still queued -- so the first
# run after the fix drains a backlog and writes typed facts from old memories.
# That is the fix working, but it moves rows under any probe that seeds
# `user`/`email` and asserts on it.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
P=/root/psql.sh

echo "memory_facts jobs by status:"
$P "select '  ' || status || ' = ' || count(*) from kb_async_jobs
      where kind='memory_facts' group by status order by 1"

echo "live 'user email' semantic edges:"
$P "select '  id=' || id || ' class=' || confidence_class || ' rank=' || authority_rank
         || ' src=' || source || ' state=' ||
           case when superseded_at='' and invalidated_at='' and suppressed=0
                then 'current' else 'gone' end
      from entity_edges
      where source='user' and relation='email' order by id"

echo "recently asserted semantic edges (last 10):"
$P "select '  ' || source || ' ' || relation || ' ' || target
         || '  class=' || confidence_class || ' asserted=' || asserted_at
      from entity_edges where edge_class='semantic' order by id desc limit 10"
