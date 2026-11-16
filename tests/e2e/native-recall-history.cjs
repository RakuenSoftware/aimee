const assert=require('node:assert/strict');
function validateAddedTurns(rows,turn,session,actor){
 assert(rows.every(r=>r.session_id===session&&r.actor===actor));
 const history=rows.filter(r=>r.event.kind==='history');
 const proofs=rows.filter(r=>r.event.kind==='replay_proof');
 for(const row of proofs){
  const event=row.event,proof=event.payload;assert.deepEqual(Object.keys(event).sort(),['kind','payload']);
  assert(/^[0-9a-f]{32}$/.test(row.turn_id));assert(new RegExp('^'+row.turn_id+':proof:[1-9][0-9]*$').test(row.event_id));
  assert(proof&&typeof proof==='object'&&!Array.isArray(proof));assert(Object.keys(proof).every(key=>['scope','block_digest','wire','thinking_binding'].includes(key)));
  for(const value of [proof.scope,proof.block_digest])assert(typeof value==='string'&&/^[0-9a-f]{64}$/.test(value));
  assert(proof.wire==='responses'?proof.thinking_binding===undefined:proof.wire==='anthropic'&&typeof proof.thinking_binding==='string'&&/^ap1:[0-9a-f]{64}:[0-9a-f]{64}:[0-9a-f]{64}$/.test(proof.thinking_binding));
 }
 assert(rows.filter(r=>!['history','replay_proof'].includes(r.event.kind)).every(r=>r.turn_id===turn),'Foreign execution event');
 if(!history.length)return;
 assert.equal(new Set(history.map(r=>r.turn_id)).size,1,'Multiple history observation turns');
 assert(history.every(r=>r.event.payload.scope==='primary'&&['message','commit'].includes(r.event.payload.phase)));
 const messages=history.filter(r=>r.event.payload.phase==='message'),commits=history.filter(r=>r.event.payload.phase==='commit');
 assert.equal(commits.length,1);assert.equal(history.at(-1),commits[0]);
 assert.equal(commits[0].event.payload.count,messages.length);
 messages.forEach((r,i)=>assert.equal(r.event.payload.index??0,i,'Noncontiguous primary history index'));
}
module.exports={validateAddedTurns};
