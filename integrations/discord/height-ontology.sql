-- Out-of-band migration for an existing 1.0.0 KB deployment using the updated
-- memory module. New source builds include this row in the generated schema.
-- Preserve operator definitions and stable relation IDs.
INSERT INTO rel_types(rel_type,head_kinds,tail_kinds,is_symmetric,inverse_rel_type,correction_behavior,category,sensitivity,is_hierarchy_rel,status)
VALUES('has_height','other','scalar',0,'','supersede','measurement','normal',0,'active')
ON CONFLICT(rel_type) DO NOTHING;
