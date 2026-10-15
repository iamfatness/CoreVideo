const AREAS = new Set(['Overall workflow','Source list and identity','Layout and compact view','Picture controls and live edits','Audio controls','Lower-third text','Live versus draft behavior','Health and status clarity']);
const VERDICTS = new Set(['changes','approve','question']);
const SCENARIOS = new Set(['populated','offline','returns','unassigned']);
const response = (value,status=200)=>new Response(JSON.stringify(value),{status,headers:{'content-type':'application/json; charset=utf-8','cache-control':'no-store','x-content-type-options':'nosniff','x-robots-tag':'noindex, nofollow'}});
async function boundedJson(request){
  const reader=request.body?.getReader();if(!reader)throw new Error('Missing feedback.');
  const chunks=[];let bytes=0;
  try{while(true){const {done,value}=await reader.read();if(done)break;bytes+=value.byteLength;if(bytes>24576){await reader.cancel();throw new RangeError('Feedback request is too large.');}chunks.push(value);}}
  finally{reader.releaseLock();}
  const payload=new Uint8Array(bytes);let at=0;for(const chunk of chunks){payload.set(chunk,at);at+=chunk.byteLength;}
  return JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(payload));
}

// Private KV records only. There is deliberately no public read/list route.
export async function handleReviewFeedback(request,env){
  if(request.method!=='POST')return response({error:'Use POST.'},405);
  if(request.headers.get('origin')!==new URL(request.url).origin)return response({error:'Submit feedback from this review page.'},403);
  if(!request.headers.get('content-type')?.toLowerCase().startsWith('application/json'))return response({error:'Use JSON feedback.'},415);
  if(!env.REVIEW_FEEDBACK)return response({error:'Feedback storage is unavailable. Please keep your draft and try again.'},503);
  let body;try{body=await boundedJson(request);}catch(error){return response({error:error instanceof RangeError?'Feedback request is too large.':'Feedback could not be read.'},error instanceof RangeError?413:400);}
  if(!body || typeof body!=='object'||Array.isArray(body)||body.review!=='sources-workspace-v1'||
      typeof body.submissionId!=='string'||!/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i.test(body.submissionId)||
      !AREAS.has(body.area)||!VERDICTS.has(body.verdict)||!SCENARIOS.has(body.scenario)||!['wide','compact'].includes(body.view)||
      typeof body.comment!=='string'||body.comment.trim().length<3||body.comment.length>4000||body.website)
    return response({error:'Check the review fields and enter 3–4000 characters of feedback.'},400);
  const note={review:body.review,area:body.area,verdict:body.verdict,comment:body.comment.trim(),scenario:body.scenario,view:body.view};
  const id=body.submissionId.toLowerCase(),key=`feedback:sources-workspace-v1:${id}`;
  try{
    const existing=await env.REVIEW_FEEDBACK.get(key,'json');
    if(existing){if(JSON.stringify(existing.note)!==JSON.stringify(note))return response({error:'This submission has changed. Please reload and submit your saved draft again.'},409);return response({saved:true,id,savedAt:existing.savedAt},201);}
    // Best-effort abuse protection; KV is eventually consistent. IP addresses
    // are not retained with feedback. Rate keys are hashed and expire quickly.
    const address=request.headers.get('cf-connecting-ip')||'unknown';
    const digest=await crypto.subtle.digest('SHA-256',new TextEncoder().encode(address));
    const fingerprint=Array.from(new Uint8Array(digest),b=>b.toString(16).padStart(2,'0')).join('');
    const rateKey=`rate:${Math.floor(Date.now()/60000)}:${fingerprint}`;
    const count=Number(await env.REVIEW_FEEDBACK.get(rateKey)||0);
    if(count>=8)return response({error:'Please wait a minute before submitting another note.'},429);
    await env.REVIEW_FEEDBACK.put(rateKey,String(count+1),{expirationTtl:120});
    const savedAt=new Date().toISOString();
    await env.REVIEW_FEEDBACK.put(key,JSON.stringify({id,savedAt,note}),{expirationTtl:180*86400});
    return response({saved:true,id,savedAt},201);
  }catch{return response({error:'Feedback was not confirmed saved. Your draft can be retried or downloaded.'},503);}
}
