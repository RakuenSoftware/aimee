package memory

import (
	"context"
	"fmt"

	store "github.com/JBailes/aimee/server-go/db"
)

// Only refresh a relation this producer owns. A coincident authored or indexed
// relation keeps its existing ownership and dependency contract. The caller
// holds the source row lock through extraction and this transaction.
func (s *postgresDataStore) writeCognifyRelation(ctx context.Context, parent int64, r cognifyRelation) error {
	var relation int64
	err := s.db.QueryRow(ctx, `INSERT INTO memory_relations(memory_id,src_entity,relation,dst_entity,fact_text)
 SELECT $1,$2,$3,$4,$5 WHERE NOT EXISTS(SELECT 1 FROM memory_relations
 WHERE memory_id=$1 AND src_entity=$2 AND relation=$3 AND dst_entity=$4 AND fact_text=$5 AND invalid_at='')
 RETURNING id`, parent, r.Subject, r.Relation, r.Object, r.FactText).Scan(&relation)
	if store.IsNoRows(err) {
		err = s.db.QueryRow(ctx, `SELECT r.id FROM memory_relations r WHERE r.memory_id=$1
 AND r.src_entity=$2 AND r.relation=$3 AND r.dst_entity=$4 AND r.fact_text=$5 AND r.invalid_at=''
 AND EXISTS(SELECT 1 FROM memory_lineage own WHERE own.object_type='relation' AND own.object_id=r.id
 AND own.source_kind='memory-cognify-v1' AND own.source_ref=$1::bigint::text)
 ORDER BY r.id LIMIT 1 FOR UPDATE`, parent, r.Subject, r.Relation, r.Object, r.FactText).Scan(&relation)
		if store.IsNoRows(err) {
			return nil
		}
	}
	if err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'relation',$1,'memory-cognify-v1',$2 WHERE NOT EXISTS(SELECT 1 FROM memory_lineage
 WHERE object_type='relation' AND object_id=$1 AND source_kind='memory-cognify-v1' AND source_ref=$2)`, relation, fmt.Sprint(parent)); err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `DELETE FROM memory_lineage WHERE object_type='relation' AND object_id=$1
 AND source_kind='memory-relation-input-v2'`, relation); err != nil {
		return err
	}
	tag, err := s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'relation',r.id,'memory-relation-input-v2',jsonb_build_object(
 'record_id',m.id::text,'record_revision',m.record_revision::text,
 'schema_version',1,'owner_id',o.owner_id::text,'derived_revision',r.record_revision::text)::text
 FROM memory_relations r JOIN memories m ON m.id=r.memory_id CROSS JOIN memory_collection_owner o
 WHERE r.id=$1 AND m.id=$2 AND o.id=1`, relation, parent)
	if err == nil && tag.RowsAffected() != 1 {
		return errDerivedSource
	}
	return err
}
