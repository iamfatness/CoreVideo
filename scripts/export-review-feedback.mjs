import fs from 'node:fs';
import {execFileSync} from 'node:child_process';
import {createPublicKey,randomBytes,createCipheriv,publicEncrypt,constants} from 'node:crypto';

// Maintainer-only workflow. Plaintext exists in memory, never logs/artifacts.
// The recipient's private key stays on their machine. No web reader is added.
function wrangler(args){return execFileSync('npx',['--yes','wrangler@latest',...args],{encoding:'utf8',stdio:['ignore','pipe','pipe'],env:{...process.env,NO_COLOR:'1'}});}
try{
  const recipient=createPublicKey(Buffer.from(process.env.REVIEW_PUBLIC_KEY||'','base64'));
  if(recipient.asymmetricKeyType!=='rsa'||recipient.asymmetricKeyDetails.modulusLength<3072)throw Error('Invalid recipient');
  const namespace=JSON.parse(wrangler(['kv','namespace','list'])).find(n=>n.title==='corevideo-review-feedback');
  if(!namespace||!/^[a-f0-9]{32}$/.test(namespace.id))throw Error('Missing storage');
  const keys=JSON.parse(wrangler(['kv','key','list','--namespace-id',namespace.id,'--prefix','feedback:','--remote']));
  const notes=[];
  for(const key of keys){if(!/^feedback:(sources-workspace-v1|sources-table-v1|operator-uiux-v1|workflow-review-v1):[a-f0-9-]{36}$/.test(key.name))continue;notes.push(JSON.parse(wrangler(['kv','key','get',key.name,'--namespace-id',namespace.id,'--remote','--text'])));}
  const secret=randomBytes(32),iv=randomBytes(12),cipher=createCipheriv('aes-256-gcm',secret,iv);
  const encrypted=Buffer.concat([cipher.update(JSON.stringify({exportedAt:new Date().toISOString(),notes}),'utf8'),cipher.final()]);
  const wrapped=publicEncrypt({key:recipient,oaepHash:'sha256',padding:constants.RSA_PKCS1_OAEP_PADDING},secret);
  fs.writeFileSync('review-feedback.encrypted.json',JSON.stringify({version:1,algorithm:'RSA-OAEP-SHA256/AES-256-GCM',key:wrapped.toString('base64'),iv:iv.toString('base64'),tag:cipher.getAuthTag().toString('base64'),data:encrypted.toString('base64')}));
  console.log('Review notes encrypted for the supplied recipient. No plaintext exported.');
}catch{console.error('Private review export failed; no plaintext was logged or exported.');process.exitCode=1;}
