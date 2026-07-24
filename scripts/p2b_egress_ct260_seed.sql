\set ON_ERROR_STOP on
BEGIN;
SELECT set_config('aimee.principal','owner',true);
INSERT INTO kb_team(id,name) VALUES (982260,'p2b_ct260_live');
INSERT INTO kb_team_membership(identity_key,team,is_default)
  VALUES ('cert:issuer-p2b-live:01AB',982260,1),('owner',982260,0);
INSERT INTO kb_enrollments(scope,fingerprint,serial,state,cert_issuer,cert_serial_norm,authority_id)
  VALUES ('p2b-live','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
          '01AB','active','issuer-p2b-live','01AB','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb');
SELECT org_catalog_bedrock_upsert('p2b-live-model','P2b Live Model','converse','anthropic',
  'foundation','aws','us-east-1',NULL,ARRAY['us-east-1'],NULL,'',true);
SELECT org_model_entitle('p2b-live-model',982260);
SELECT org_pricing_add_version('bedrock','p2b-live-billable',1.0,2.0,0.5,0.75);
SELECT org_vault_put('team:982260:bedrock',982260,'bedrock','iam',1,
  decode(repeat('01',40),'hex'),decode(repeat('02',12),'hex'),decode('03','hex'),
  decode(repeat('04',16),'hex'));
SELECT org_budget_set(982260,NULL,'day',100.0,NULL);
COMMIT;
