import fs from 'node:fs';
import {execFileSync} from 'node:child_process';

// Runs in the existing deployment job with its existing Cloudflare credential.
// No credentials are printed, written to source, or sent to another service.
const command=process.platform==='win32'?'npx.cmd':'npx';
function wrangler(args){return execFileSync(command,['--yes','wrangler@latest',...args],{encoding:'utf8',stdio:['ignore','pipe','pipe'],env:{...process.env,NO_COLOR:'1'}});}
function namespaces(){return JSON.parse(wrangler(['kv','namespace','list']));}
const title='corevideo-review-feedback';
let namespace=namespaces().find(item=>item.title===title);
if(!namespace){wrangler(['kv','namespace','create',title,'--binding','REVIEW_FEEDBACK','--update-config=false']);namespace=namespaces().find(item=>item.title===title);}
if(!namespace||!/^[a-f0-9]{32}$/.test(namespace.id))throw new Error('Review feedback namespace could not be configured.');
const config=fs.readFileSync('wrangler.jsonc','utf8');
if(!config.includes('"assets": {'))throw new Error('Worker asset configuration missing.');
const binding=JSON.stringify([{binding:'REVIEW_FEEDBACK',id:namespace.id}]);
fs.writeFileSync('wrangler.review.generated.jsonc',config.replace('"assets": {',`"kv_namespaces": ${binding},\n  "assets": {`));
console.log('Review feedback storage binding configured.');
