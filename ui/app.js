(() => {
'use strict';

const bridge = window.chrome && window.chrome.webview;
const MOCK = !bridge;
function send(o){
  if (bridge) bridge.postMessage(JSON.stringify(o));
  else mock(o);
}
function onMsg(m){ handle(m); }
if (bridge) bridge.addEventListener('message', e => onMsg(e.data));

const S = { file:null, reduction:90, codec:'hevc', speed:'balanced', av1:false, running:false };
const $ = s => document.querySelector(s);
const setState = s => document.body.dataset.state = s;

function human(b){
  const u=['B','KB','MB','GB','TB']; let v=b,i=0;
  while(v>=1024&&i<4){v/=1024;i++;}
  return (v<10?v.toFixed(2):v<100?v.toFixed(1):Math.round(v))+' '+u[i];
}
function mmss(s){ const m=Math.floor(s/60),x=Math.round(s%60); return m+'m '+(x<10?'0':'')+x+'s'; }

function handle(m){
  switch(m.type){
    case 'caps': {
      $('#gpuName').textContent = m.gpu || 'GPU';
      $('#encName').textContent = m.encoder || 'encoder';
      S.av1 = !!m.av1;
      const b = $('#btnAv1');
      if(!S.av1){ b.disabled=true; b.title='AV1 indisponivel nesta GPU'; }
      break;
    }
    case 'file': {
      S.file = m; setState('config');
      $('#fName').textContent = m.name;
      $('#fSize').textContent = m.sizeHuman;
      $('#fRes').textContent  = m.width+' x '+m.height;
      $('#fDur').textContent  = mmss(m.durationSec);
      recalc(); introPanel('#panel-config');
      break;
    }
    case 'started': S.running=true; setState('run'); energize(true); resetRing(); introPanel('#panel-run'); break;
    case 'progress': updateProgress(m); break;
    case 'done': showDone(m); break;
    case 'error': toast(m.message); S.running=false; energize(false);
                  if(document.body.dataset.state==='run') setState(S.file?'config':'idle'); break;
    case 'log':  break;
  }
}

function recalc(){
  if(!S.file) return;
  const out = S.file.sizeBytes*(1-S.reduction/100);
  $('#estSize').textContent = human(out);
  $('#estBar').style.width = (100-S.reduction)+'%';
}

const RC = 2*Math.PI*96;
function resetRing(){ $('#ringFg').style.strokeDashoffset = RC; $('#pct').textContent='0'; }
function updateProgress(m){
  const p = Math.max(0,Math.min(100,m.percent));
  $('#ringFg').style.strokeDashoffset = RC*(1-p/100);
  $('#pct').textContent = Math.round(p);
  $('#stSpeed').textContent = (m.speed||0).toFixed(1)+'x';
  $('#stEta').textContent   = m.etaSec>0 ? mmss(m.etaSec) : 'quase la';
  $('#stFps').textContent   = Math.round(m.fps||0);
  $('#stSize').textContent  = human(m.outBytes||0);
}
function showDone(m){
  S.running=false; energize(false); setState('done');
  $('#doneRatio').textContent = Math.round(m.ratio);
  $('#doneIn').textContent  = m.inHuman;
  $('#doneOut').textContent = m.outHuman;
  $('#doneTime').textContent = m.elapsedSec<60 ? m.elapsedSec.toFixed(1)+'s' : mmss(m.elapsedSec);
  introPanel('#panel-done');
  if(window.gsap){
    gsap.from('#doneRatio',{scale:.4,opacity:0,duration:.7,ease:'back.out(2)'});
    gsap.from('.done-badge',{scale:0,rotate:-40,duration:.6,ease:'back.out(2)'});
  }
}

let toastT;
function toast(msg){
  const t=$('#toast'); t.textContent=msg; t.classList.add('show');
  clearTimeout(toastT); toastT=setTimeout(()=>t.classList.remove('show'),4200);
}

$('#btnPick').onclick = ()=>send({type:'pick'});
$('#btnSwap').onclick = ()=>send({type:'pick'});
$('#btnGo').onclick   = ()=>{ if(S.file) send({type:'start',reduction:S.reduction,codec:S.codec,speed:S.speed}); };
$('#btnCancel').onclick = ()=>send({type:'cancel'});
$('#btnOpen').onclick   = ()=>send({type:'open'});
$('#btnReveal').onclick = ()=>send({type:'reveal'});
$('#btnAgain').onclick  = ()=>{ setState(S.file?'config':'idle'); };

function segSetup(sel, key, after){
  const seg=$(sel);
  seg.addEventListener('click',e=>{
    const b=e.target.closest('button'); if(!b||b.disabled) return;
    seg.querySelectorAll('button').forEach(x=>x.classList.remove('on'));
    b.classList.add('on');
    const v=b.dataset.v; S[key]= (key==='reduction')?parseInt(v,10):v;
    if(after) after(v); recalc();
  });
}
segSetup('#segReduce','reduction');
segSetup('#segCodec','codec',v=>{
  $('#codecHint').textContent = v==='av1' ? 'AV1 comprime ainda mais precisa de player recente'
                                          : 'HEVC abre em qualquer aparelho';
});
segSetup('#segSpeed','speed');

const drop=$('#drop');
['dragenter','dragover'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.add('over');}));
['dragleave','drop'].forEach(ev=>drop.addEventListener(ev,e=>{e.preventDefault();drop.classList.remove('over');}));

function introPanel(sel){
  if(!window.gsap) return;
  const el=$(sel);
  gsap.fromTo(el,{y:24,opacity:0},{y:0,opacity:1,duration:.6,ease:'power3.out'});
  gsap.fromTo(el.children,{y:18,opacity:0},{y:0,opacity:1,duration:.5,stagger:.05,ease:'power3.out',delay:.05});
}

let energized=false;
function energize(on){ energized=on; document.querySelectorAll('use').forEach(()=>{});
  document.body.classList.toggle('energized',on); }

(function scene(){
  if(!window.THREE) return;
  const canvas=$('#scene');
  const renderer=new THREE.WebGLRenderer({canvas,antialias:true,alpha:true});
  renderer.setPixelRatio(Math.min(devicePixelRatio,2));
  const scn=new THREE.Scene();
  const cam=new THREE.PerspectiveCamera(55,innerWidth/innerHeight,.1,100);
  cam.position.z=7;

  const PINK=0xff2d78, PINK2=0xff6aa0;
  const group=new THREE.Group(); scn.add(group);

  function poly(geo,color,op,scale,pos){
    const edges=new THREE.EdgesGeometry(geo);
    const mat=new THREE.LineBasicMaterial({color,transparent:true,opacity:op});
    const l=new THREE.LineSegments(edges,mat);
    l.scale.setScalar(scale); l.position.set(...pos);
    group.add(l); return l;
  }

  const a=poly(new THREE.DodecahedronGeometry(1,0),PINK2,.34,1.15,[-4.4, 2.3,-2.5]);
  const b=poly(new THREE.IcosahedronGeometry(1,0), PINK ,.42,1.35,[ 4.6, 2.1,-3.0]);
  const c=poly(new THREE.OctahedronGeometry(1,0),  PINK ,.38,1.05,[-4.2,-2.5,-2.2]);
  const d=poly(new THREE.IcosahedronGeometry(1,0), PINK2,.30,0.85,[ 4.2,-2.7,-2.0]);

  function miniQuasarTexture(){
    const s=128, cv=document.createElement('canvas'); cv.width=cv.height=s;
    const g=cv.getContext('2d'), c=s/2;
    const rad=g.createRadialGradient(c,c,0,c,c,c);
    rad.addColorStop(0,'rgba(255,224,238,0.95)');
    rad.addColorStop(0.16,'rgba(255,90,158,0.6)');
    rad.addColorStop(0.5,'rgba(255,20,95,0.14)');
    rad.addColorStop(1,'rgba(255,20,95,0)');
    g.fillStyle=rad; g.fillRect(0,0,s,s);

    g.strokeStyle='rgba(255,255,255,0.85)'; g.lineCap='round';
    g.lineWidth=2.2; g.beginPath(); g.moveTo(c,c-c*0.88); g.lineTo(c,c+c*0.88); g.stroke();
    g.lineWidth=1.6; g.beginPath(); g.moveTo(c-c*0.55,c); g.lineTo(c+c*0.55,c); g.stroke();
    g.fillStyle='#fff'; g.beginPath(); g.arc(c,c,2.6,0,7); g.fill();
    return new THREE.CanvasTexture(cv);
  }
  const mtex=miniQuasarTexture();
  const MG=new THREE.Group(); scn.add(MG);
  const minis=[];
  for(let i=0;i<13;i++){
    const mat=new THREE.SpriteMaterial({map:mtex,transparent:true,opacity:.4,
      blending:THREE.AdditiveBlending,depthWrite:false});
    const sp=new THREE.Sprite(mat);
    const x=(Math.random()-.5)*17, y=(Math.random()-.5)*10, z=-2-Math.random()*6;
    sp.position.set(x,y,z);
    const base=0.32+Math.random()*0.5;
    sp.userData={base, phase:Math.random()*Math.PI*2, drift:0.05+Math.random()*0.05,
                 x0:x, y0:y, amp:0.25+Math.random()*0.35};
    sp.scale.setScalar(base);
    MG.add(sp); minis.push(sp);
  }
  const W_RHYTHM = 2*Math.PI/3.6;

  let mx=0,my=0;
  addEventListener('mousemove',e=>{ mx=(e.clientX/innerWidth-.5); my=(e.clientY/innerHeight-.5); });
  addEventListener('resize',()=>{ cam.aspect=innerWidth/innerHeight; cam.updateProjectionMatrix(); renderer.setSize(innerWidth,innerHeight); });
  renderer.setSize(innerWidth,innerHeight);

  const clock=new THREE.Clock();
  (function loop(){
    requestAnimationFrame(loop);
    const t=clock.getElapsedTime(), k=energized?3.2:1;
    a.rotation.x=t*.12*k; a.rotation.y=t*.16*k;
    b.rotation.x=-t*.1*k; b.rotation.z=t*.13*k;
    c.rotation.y=t*.2*k;  c.rotation.x=t*.09*k;
    d.rotation.z=t*.22*k; d.rotation.y=-t*.15*k;

    for(const sp of minis){
      const u=sp.userData;
      const pulse=0.5+0.5*Math.sin(t*W_RHYTHM+u.phase);
      sp.material.opacity=(0.12+u.base*0.4)*(0.35+0.65*pulse);
      sp.scale.setScalar(u.base*(0.78+0.44*pulse));
      sp.position.x=u.x0+Math.sin(t*u.drift+u.phase)*u.amp;
      sp.position.y=u.y0+Math.cos(t*u.drift*0.8+u.phase)*u.amp*0.6;
    }
    group.rotation.y += ((mx*.4)-group.rotation.y)*.04;
    group.rotation.x += ((my*.3)-group.rotation.x)*.04;
    MG.rotation.y += ((mx*.15)-MG.rotation.y)*.04;
    renderer.render(scn,cam);
  })();
})();

function startIntro(){

  if(window.gsap){
    gsap.from('.splash-mark',{scale:.55,opacity:0,rotate:-25,duration:1,ease:'power3.out'});
    gsap.from('.splash-word',{y:26,opacity:0,duration:.7,delay:.4,ease:'power3.out'});
    gsap.from('.splash-tag',{y:14,opacity:0,duration:.6,delay:.6,ease:'power2.out'});
    gsap.to('.splash-mark',{scale:1.05,duration:1.4,delay:.6,ease:'power1.inOut'});
  }
  const hide=()=>{ const sp=$('#splash'); if(!sp||sp.dataset.gone) return;
    sp.dataset.gone=1; sp.style.opacity='0'; setTimeout(()=>sp.style.display='none',650); };
  setTimeout(hide, 2200);
  setTimeout(()=>send({type:'ready'}), 1600);
}
if(document.readyState==='complete') startIntro();
else window.addEventListener('load', startIntro);

function mock(o){
  if(o.type==='ready') setTimeout(()=>onMsg({type:'caps',gpu:'NVIDIA GeForce RTX 4060 Laptop GPU',encoder:'HEVC NVENC',av1:true,hw:true}),300);
  if(o.type==='pick') setTimeout(()=>onMsg({type:'file',name:'gravacao_final.mp4',sizeBytes:524288000,sizeHuman:'500 MB',durationSec:612,width:1920,height:1080,vcodec:'h264'}),200);
  if(o.type==='cancel'){ mock._c=true; }
  if(o.type==='open'||o.type==='reveal') toast('acao disponivel no aplicativo');
  if(o.type==='start'){
    onMsg({type:'started'}); mock._c=false;
    const inB=S.file.sizeBytes, outB=inB*(1-S.reduction/100); let p=0;
    const iv=setInterval(()=>{
      if(mock._c){clearInterval(iv);return;}
      p+=Math.random()*7+3;
      if(p>=100){ p=100; clearInterval(iv);
        onMsg({type:'progress',percent:100,speed:11.4,etaSec:0,fps:340,outBytes:outB});
        setTimeout(()=>onMsg({type:'done',outName:'gravacao_final_KernelP.mp4',outBytes:outB,outHuman:human(outB),inHuman:S.file.sizeHuman,ratio:S.reduction,elapsedSec:54.3}),400);
      } else {
        onMsg({type:'progress',percent:p,speed:9+Math.random()*4,etaSec:(100-p)*.6,fps:300+Math.random()*80,outBytes:outB*(p/100)});
      }
    },260);
  }
}

})();
