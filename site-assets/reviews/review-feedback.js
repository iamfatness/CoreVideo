const field=id=>document.getElementById(id);
const form=field('batch-feedback');
const review=document.querySelector('[data-review]').dataset.review;
const key='corevideo-feedback-'+review;
const note=()=>({area:field('feedback-area').value,verdict:field('feedback-verdict').value,comment:field('feedback-text').value});
function saveDraft(){try{localStorage.setItem(key,JSON.stringify(note()));}catch{}}
try{const value=JSON.parse(localStorage.getItem(key)||'null');if(value){field('feedback-area').value=value.area;field('feedback-verdict').value=value.verdict;field('feedback-text').value=value.comment;}}catch{}
for(const id of ['feedback-area','feedback-verdict','feedback-text'])field(id).addEventListener('input',saveDraft);
document.querySelectorAll('[data-feedback-area]').forEach(link=>link.addEventListener('click',()=>{field('feedback-area').value=link.dataset.feedbackArea;saveDraft();field('feedback-text').focus({preventScroll:true});}));
let pending=null;
form.addEventListener('submit',async event=>{
  event.preventDefault();if(!form.reportValidity())return;
  const draft=note();if(draft.comment.trim().length<3){field('feedback-status').textContent='Please add at least three characters of feedback.';return;}
  const signature=JSON.stringify(draft);if(!pending||pending.signature!==signature)pending={signature,id:crypto.randomUUID()};
  field('feedback-submit').disabled=true;field('feedback-status').textContent='Saving feedback…';
  try{
    const response=await fetch('/reviews/api/feedback',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({...draft,review,submissionId:pending.id,scenario:'populated',view:'wide',website:field('feedback-website').value}),signal:AbortSignal.timeout(15000)});
    const result=await response.json();if(!response.ok||!result.saved)throw new Error(result.error||'Feedback was not saved.');
    field('feedback-status').textContent='Feedback saved. Thank you.';field('feedback-receipt').textContent=`Receipt: ${result.id} · ${new Date(result.savedAt).toLocaleString()}`;
    // Preserve any new text typed while the previous note was saving.
    if(JSON.stringify(note())===signature){field('feedback-text').value='';saveDraft();}pending=null;
  }catch(error){field('feedback-status').textContent=`${error.message} Your draft is still here. Please retry or download a copy.`;}
  finally{field('feedback-submit').disabled=false;}
});
field('feedback-download').addEventListener('click',()=>{const url=URL.createObjectURL(new Blob([JSON.stringify({review,...note()},null,2)],{type:'application/json'}));const link=document.createElement('a');link.href=url;link.download='corevideo-operator-feedback.json';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);});
