(()=>{
const $=id=>document.getElementById(id);
const escape=value=>String(value).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const sources=[
 {id:'host',name:'Alex Morgan',type:'Zoom',format:'1920 × 1080',fps:'59.94',audio:'Source audio',level:-18,online:true},
 {id:'alice',name:'Alice Chen',type:'Zoom',format:'1280 × 720',fps:'30',audio:'Source audio',level:-22,online:true},
 {id:'share',name:'Presentation',type:'Screen',format:'2560 × 1440',fps:'15',audio:'No paired audio',online:true},
 {id:'camera',name:'Studio camera',type:'Camera',format:'1920 × 1080',fps:'60',audio:'Studio microphone',level:-16,online:true},
 {id:'clip',name:'Opening film',type:'Media',format:'1920 × 1080',fps:'29.97',audio:'File audio',level:-20,online:true},
 {id:'bob',name:'Bob Rivera',type:'Zoom',format:'1280 × 720',fps:'30',audio:'Source audio',level:-24,online:false},
 {id:'srt',name:'Remote SRT',type:'SRT',format:'1920 × 1080',fps:'50',audio:'Embedded audio',level:-21,online:true},
 {id:'rtmp',name:'RTMP guest',type:'RTMP',format:'1280 × 720',fps:'30',audio:'Embedded audio',level:-20,online:true},
 {id:'browser',name:'Web graphic',type:'Browser',format:'1920 × 1080',fps:'30',audio:'No paired audio',online:true}
];
const inputs=Array.from({length:10},(_,i)=>({id:i+1,source:sources[i]?.id&&i<7?sources[i].id:null,mv:i<4,iso:i<2,writing:i===0}));
const looks=Object.fromEntries(sources.map(s=>[s.id,{name:s.name,opacity:100,ltName:s.name,ltSecondary:s.type==='Zoom'?'Guest':'',draftName:s.name,draftSecondary:s.type==='Zoom'?'Guest':''}]));
looks.host.ltSecondary=looks.host.draftSecondary='Host';
let selected=2,opened=false,activeTab='source',pending=null;
const expanded=new Set();
const input=()=>inputs.find(i=>i.id===selected);
const source=i=>sources.find(s=>s.id===(i||input()).source);
const name=i=>i.source?looks[i.source].name:'Unassigned';
function status(text){$('status').textContent=text;}
function saveColumns(){const hidden=[...document.querySelectorAll('[data-column]')].filter(c=>!c.checked).map(c=>c.dataset.column);for(const c of document.querySelectorAll('[data-column]'))$('workspace').classList.toggle('hide-'+c.dataset.column,!c.checked);try{localStorage.setItem('corevideo-table-columns-v1',JSON.stringify(hidden));}catch{}}
try{const hidden=JSON.parse(localStorage.getItem('corevideo-table-columns-v1')||'[]');for(const c of document.querySelectorAll('[data-column]'))c.checked=!hidden.includes(c.dataset.column);}catch{}
document.querySelectorAll('[data-column]').forEach(c=>c.addEventListener('change',saveColumns));saveColumns();
function renderRows(){
 const query=$('search').value.toLowerCase(),category=$('category').value;
 const visible=inputs.filter(i=>{const s=source(i);return (name(i)+' '+(s?.type||'')+' '+String(i.id).padStart(2,'0')).toLowerCase().includes(query)&&(category==='all'||(category==='network'?['SRT','RTMP'].includes(s?.type):s?.type===category));});
 $('rows').innerHTML=visible.map(i=>{const s=source(i),offline=s&&!s.online,state=!s?'Empty':offline?'Offline':'Advancing';return `<tr data-row="${i.id}" class="${selected===i.id?'selected':''}" aria-selected="${selected===i.id}"><td>${String(i.id).padStart(2,'0')}</td><td><div class="identity"><span class="thumb ${!s?'empty':offline?'offline':''}" aria-hidden="true">${!s?'＋':offline?'—':s.type==='Media'?'▶':'●'}</span><span><span class="source-name" title="${escape(name(i))}">${escape(name(i))}</span><span class="source-note">${escape(s?s.type+' · '+(offline?'Offline':i.mv?'In MV':'Not in MV'):'Choose a source')}</span></span></div></td><td class="optional type-col">${s?.type||'—'}</td><td class="observed-format">${s&&s.online?s.format:'—'}</td><td>${s&&s.online?s.fps:'—'}</td><td class="optional signal-col"><span class="signal ${offline?'offline':''}">${state}</span></td><td class="optional audio-col">${s&&s.level!=null&&s.online?`<div class="meter" aria-hidden="true"><span style="width:${Math.max(8,85+s.level)}%"></span></div><span class="audio-label">${s.level} dBFS</span>`:`<span class="audio-label">${offline?'Unavailable':s?'No paired audio':'—'}</span>`}</td><td><input type="checkbox" data-mv="${i.id}" aria-label="Input ${i.id} in Multiview" ${i.mv?'checked':''} ${!s?'disabled':''}></td><td><input type="checkbox" data-iso="${i.id}" aria-label="Arm ISO for input ${i.id}" ${i.iso?'checked':''} ${!s||i.writing?'disabled':''}><span class="iso-label ${i.writing?'writing':''}">${i.writing?'REC · demo':i.iso?'Armed':'Off'}</span></td><td><button data-inspect="${i.id}" class="inspect-button" aria-label="Inspect input ${i.id}">Inspect</button><button data-more="${i.id}" class="inspect-button" aria-label="More details for input ${i.id}" aria-expanded="${expanded.has(i.id)}" aria-controls="row-details-${i.id}">⋯</button></td></tr><tr class="row-more" id="row-details-${i.id}" ${expanded.has(i.id)?'':'hidden'}><td colspan="10"><div class="row-grid"><span>Source type<strong>${s?.type||'Unassigned'}</strong></span><span>Signal<strong>${state}</strong></span><span>Paired audio<strong>${escape(s?.audio||'None')}</strong></span><span>Audio level<strong>${s?.level!=null&&s.online?s.level+' dBFS · demo':'Unavailable'}</strong></span><span>ISO writer<strong>${i.writing?'Writing · simulated':i.iso?'Armed · writer idle':'Not armed'}</strong></span>${offline?`<span>Last known format, not advancing<strong>${s.format} · ${s.fps} fps</strong></span>`:''}</div></td></tr>`;}).join('');
 $('no-results').hidden=visible.length>0;$('counts').textContent=`${inputs.filter(i=>i.source).length}/10 assigned · ${inputs.filter(i=>i.mv&&i.source).length} in MV · ${inputs.filter(i=>i.iso).length} ISO armed`;
}
function tab(name){activeTab=name;for(const button of document.querySelectorAll('[data-tab]')){const on=button.dataset.tab===name;button.setAttribute('aria-selected',String(on));button.tabIndex=on?0:-1;$('pane-'+button.dataset.tab).hidden=!on;}}
function renderInspector(){
 const i=input(),s=source(),look=looks[i.source];pending=null;$('assignment-confirm').hidden=true;
 $('inspector').hidden=!opened;$('workspace').classList.toggle('inspector-open',opened);$('toggle-inspector').setAttribute('aria-expanded',String(opened));$('toggle-inspector').textContent=opened?'Close inspector':'Open inspector';
 $('inspector-slot').textContent=`INPUT ${String(selected).padStart(2,'0')} · ${s?.type||'UNASSIGNED'}`;$('inspector-name').textContent=name(i);$('preview-name').textContent=!s?'Unassigned':!s.online?'Offline · binding retained':name(i);
 $('detail-stats').textContent=!s?'No source selected.':s.online?`${s.format} · ${s.fps} fps · advancing (demo)`:`Offline · no advancing video. Last known ${s.format} / ${s.fps} fps.`;
 const types=['Zoom','Camera','Screen','Media','SRT','RTMP','Browser'];$('assignment').innerHTML='<option value="">Unassigned</option>'+types.map(type=>`<optgroup label="${type}">${sources.filter(s=>s.type===type).map(s=>`<option value="${s.id}">${escape(looks[s.id].name)}</option>`).join('')}</optgroup>`).join('');$('assignment').value=i.source||'';
 $('assignment').disabled=i.writing;$('display-name').value=look?.name||'';$('display-name').disabled=!s;
 $('inspector-mv').checked=i.mv;$('inspector-mv').disabled=!s;$('inspector-iso').checked=i.iso;$('inspector-iso').disabled=!s||i.writing;
 $('lt-name').value=look?.draftName||'';$('lt-secondary').value=look?.draftSecondary||'';for(const id of ['lt-name','lt-secondary','apply-text','cancel-text'])$(id).disabled=!s;
 $('text-status').textContent='';$('recovery').textContent=i.writing?'Simulated writer active. Reassignment and disarming are disabled here; recording stop belongs in recording controls.':!s?'Choose a source. Other numbered inputs will not move.':!s.online?'Reconnect this same source or deliberately replace it. No automatic replacement and no false live FPS.':'Source is advancing in this simulation. Source connection settings remain separate from Multiview inclusion.';
 $('audio-detail').textContent=s?`${s.audio} · ${s.online&&s.level!=null?s.level+' dBFS (simulated)':'No advancing audio measurement'}`:'No source assigned.';
 $('opacity').value=$('opacity-exact').value=look?.opacity??100;$('opacity-readout').textContent=(look?.opacity??100)+'%';$('opacity-error').textContent='';$('opacity').disabled=$('opacity-exact').disabled=$('opacity-reset').disabled=!s;
 document.querySelector('.portrait').style.opacity=look?look.opacity/100:0;tab(activeTab);
}
function render(){renderRows();renderInspector();}
function inspect(id){selected=id;opened=true;render();status(`Inspecting Input ${String(id).padStart(2,'0')} · ${name(input())}. Assignments, MV and scene routes are unchanged.`);}
$('rows').addEventListener('click',event=>{if(event.target.closest('input'))return;const more=event.target.closest('[data-more]');if(more){const id=Number(more.dataset.more);expanded.has(id)?expanded.delete(id):expanded.add(id);renderRows();return;}const row=event.target.closest('[data-row]');if(row)inspect(Number(row.dataset.row));});
$('rows').addEventListener('change',event=>{const mv=event.target.dataset.mv,iso=event.target.dataset.iso;if(!mv&&!iso)return;const i=inputs.find(i=>i.id===Number(mv||iso));if(mv)i.mv=event.target.checked;else if(!i.writing)i.iso=event.target.checked;render();status(mv?`Input ${i.id} ${i.mv?'included in':'removed from'} Multiview. Binding and scene routes unchanged.`:`Input ${i.id} ISO ${i.iso?'armed; writer remains idle':'disarmed'}. No recording starts or stops.`);});
$('search').addEventListener('input',renderRows);$('category').addEventListener('change',renderRows);
$('overview').addEventListener('click',()=>{opened=false;$('search').value='';$('category').value='all';render();status('Full table. Same show, assignments and selected input.');});
$('with-inspector').addEventListener('click',()=>{inspect(selected);});
$('toggle-inspector').addEventListener('click',()=>{opened=!opened;renderInspector();status(opened?'Inspector open for the selected input. Table values retained.':'Inspector closed. Full table width restored.');});$('close-inspector').addEventListener('click',()=>{opened=false;renderInspector();$('toggle-inspector').focus();});
for(const id of ['disconnect','reconnect'])$(id).addEventListener('click',()=>{sources.find(s=>s.id==='alice').online=id==='reconnect';render();status(id==='disconnect'?'Alice offline. Input 02, name, MV inclusion and ISO arm state retained; FPS no longer reports live video.':'Alice returned to Input 02. Other assignments and settings retained.');});
document.querySelectorAll('[data-tab]').forEach(button=>{button.addEventListener('click',()=>tab(button.dataset.tab));button.addEventListener('keydown',event=>{if(!['ArrowLeft','ArrowRight','Home','End'].includes(event.key))return;event.preventDefault();const tabs=[...document.querySelectorAll('[data-tab]')],index=event.key==='Home'?0:event.key==='End'?2:(tabs.indexOf(button)+(event.key==='ArrowRight'?1:2))%3;tab(tabs[index].dataset.tab);tabs[index].focus();});});
function assign(value){input().source=value||null;if(!value){input().mv=false;input().iso=false;}render();status('Input binding explicitly changed. Other inputs did not shift. Existing scene routes still reference this input.');}
$('assignment').addEventListener('change',()=>{if(selected===1||selected===2){pending=$('assignment').value;$('assignment-confirm').hidden=false;}else assign($('assignment').value);});$('confirm-assignment').addEventListener('click',()=>{if(pending!==null)assign(pending);});$('cancel-assignment').addEventListener('click',()=>{pending=null;$('assignment-confirm').hidden=true;$('assignment').value=input().source||'';});
$('display-name').addEventListener('change',()=>{if(!source())return;looks[input().source].name=$('display-name').value.trim()||source().name;render();status('Display name updated in the mockup. Source binding and lower-third text unchanged.');});
for(const id of ['inspector-mv','inspector-iso'])$(id).addEventListener('change',()=>{input()[id==='inspector-mv'?'mv':'iso']=$(id).checked;render();status('Source inclusion/arm state changed in the mockup. No scene Take or recording start.');});
for(const id of ['lt-name','lt-secondary'])$(id).addEventListener('input',()=>{if(source())looks[input().source][id==='lt-name'?'draftName':'draftSecondary']=$(id).value;});
$('apply-text').addEventListener('click',()=>{const look=looks[input().source];if(!look)return;look.ltName=look.draftName;look.ltSecondary=look.draftSecondary;$('text-status').textContent='Text applied in simulation. Source assignment and shared style unchanged.';});$('cancel-text').addEventListener('click',()=>{const look=looks[input().source];if(!look)return;look.draftName=look.ltName;look.draftSecondary=look.ltSecondary;renderInspector();$('text-status').textContent='Draft discarded; applied text retained.';});
function opacity(value){if(!source())return;if(!Number.isFinite(value)||value<0||value>100){$('opacity-error').textContent='Use 0–100%. The previous opacity is retained.';return;}looks[input().source].opacity=value;$('opacity').value=$('opacity-exact').value=value;$('opacity-readout').textContent=value+'%';$('opacity-error').textContent='';document.querySelector('.portrait').style.opacity=value/100;}
$('opacity').addEventListener('input',()=>opacity(Number($('opacity').value)));$('opacity-exact').addEventListener('change',()=>opacity($('opacity-exact').value.trim()===''?NaN:Number($('opacity-exact').value)));$('opacity-reset').addEventListener('click',()=>opacity(100));
$('catalog').addEventListener('click',()=>{$('catalog-panel').hidden=!$('catalog-panel').hidden;if(!$('catalog-panel').hidden)$('catalog-panel').scrollIntoView({behavior:'auto'});});$('close-catalog').addEventListener('click',()=>{$('catalog-panel').hidden=true;$('catalog').focus();});
for(const id of ['quality-resolution','quality-fps'])$(id).addEventListener('change',()=>status('Requested Zoom quality changed in the mockup. Observed source format/FPS cells remain unchanged.'));
document.addEventListener('click',event=>{for(const detail of document.querySelectorAll('.tools details[open]'))if(!detail.contains(event.target))detail.open=false;});
render();

})();
