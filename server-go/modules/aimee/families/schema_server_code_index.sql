-- Repository snapshots belong to runtime state, independently of personal memory.
-- A completed scan publishes one immutable file set. Older, aborted and retired
-- snapshots remain retained; removing a source only changes the current head.
CREATE TABLE server_code_projects (
 name text PRIMARY KEY, root text NOT NULL,
 generation bigint NOT NULL DEFAULT 0,
 current_scan text NOT NULL DEFAULT '', scan_id text NOT NULL DEFAULT '',
 retired boolean NOT NULL DEFAULT false,
 scanned_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE server_code_scans (
 project text NOT NULL REFERENCES server_code_projects(name),
 scan_id text NOT NULL, state text NOT NULL CHECK(state IN ('building','ready','aborted','superseded')),
 created_at timestamptz NOT NULL DEFAULT now(), PRIMARY KEY(project,scan_id)
);
CREATE TABLE server_code_file_versions (
 project text NOT NULL, scan_id text NOT NULL, path text NOT NULL,
 data jsonb NOT NULL, content text NOT NULL, fingerprint text NOT NULL,
 embedding vector, embedding_serving text NOT NULL DEFAULT '',
 PRIMARY KEY(project,scan_id,path),
 FOREIGN KEY(project,scan_id) REFERENCES server_code_scans(project,scan_id)
);
CREATE INDEX server_code_file_versions_search ON server_code_file_versions
 USING gin(to_tsvector('simple',path||' '||content));
CREATE VIEW server_code_projects_current AS
 SELECT name,root,generation,scanned_at,current_scan FROM server_code_projects
 WHERE NOT retired AND current_scan<>'';
CREATE VIEW server_code_files_current AS
 SELECT v.project,v.path,v.content,v.fingerprint,v.embedding,v.embedding_serving,
 COALESCE(v.data->>'module_identity','') AS module_identity,
 COALESCE(v.data->>'embedding_policy','') AS embedding_policy,
 v.data->'definitions' AS definitions,v.data->'calls' AS calls,v.data->'imports' AS imports
 FROM server_code_file_versions v JOIN server_code_projects_current p
 ON p.name=v.project AND p.current_scan=v.scan_id;
-- Existing legacy repository indexes are copied, never rewritten or removed.
DO $upgrade$
BEGIN
 IF to_regclass('user_code_projects') IS NOT NULL AND to_regclass('user_code_files') IS NOT NULL THEN
  INSERT INTO server_code_projects(name,root,generation,current_scan,scanned_at)
   SELECT name,root,generation,CASE WHEN generation>0 THEN 'legacy-import-v1' ELSE '' END,scanned_at
   FROM user_code_projects;
  INSERT INTO server_code_scans(project,scan_id,state)
   SELECT name,'legacy-import-v1','ready' FROM user_code_projects;
  INSERT INTO server_code_file_versions(project,scan_id,path,data,content,fingerprint,embedding,embedding_serving)
   SELECT project,'legacy-import-v1',path,
    jsonb_build_object('rel_path',path,'content',content,'definitions',definitions,'calls',calls,
     'imports',imports,'module_identity',module_identity),content,
    encode(sha256(convert_to(content,'UTF8')),'hex'),embedding,embedding_serving
   FROM user_code_files;
 END IF;
END $upgrade$;
-- Runtime SQL capability cannot physically discard retained source history.
CREATE FUNCTION server_code_retained_history_guard() RETURNS trigger
LANGUAGE plpgsql AS $guard$
BEGIN
 IF TG_OP IN ('DELETE','TRUNCATE') OR TG_TABLE_NAME='server_code_file_versions' THEN
  RAISE EXCEPTION 'repository source history is retained; publish or retire a head instead'
   USING ERRCODE='55000';
 END IF;
 RETURN NEW;
END $guard$;
CREATE TRIGGER server_code_versions_immutable BEFORE UPDATE OR DELETE ON server_code_file_versions
 FOR EACH ROW EXECUTE FUNCTION server_code_retained_history_guard();
CREATE TRIGGER server_code_versions_no_truncate BEFORE TRUNCATE ON server_code_file_versions
 FOR EACH STATEMENT EXECUTE FUNCTION server_code_retained_history_guard();
CREATE TRIGGER server_code_scans_no_delete BEFORE DELETE ON server_code_scans
 FOR EACH ROW EXECUTE FUNCTION server_code_retained_history_guard();
CREATE TRIGGER server_code_scans_no_truncate BEFORE TRUNCATE ON server_code_scans
 FOR EACH STATEMENT EXECUTE FUNCTION server_code_retained_history_guard();
CREATE TRIGGER server_code_projects_no_delete BEFORE DELETE ON server_code_projects
 FOR EACH ROW EXECUTE FUNCTION server_code_retained_history_guard();
CREATE TRIGGER server_code_projects_no_truncate BEFORE TRUNCATE ON server_code_projects
 FOR EACH STATEMENT EXECUTE FUNCTION server_code_retained_history_guard();
