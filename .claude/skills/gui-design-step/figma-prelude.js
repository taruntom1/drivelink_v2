// Helper prelude for use_figma calls that build with the DriveLink G2 design system.
// Paste at the top of a use_figma script. Change PAGE_ID for other pages
// (G2 Design system 2:4, Screens – Local 2:5, Screens – Remote 2:6, Custom panels 2:7).
const PAGE_ID = "2:4";
const page = await figma.getNodeByIdAsync(PAGE_ID); await figma.setCurrentPageAsync(page);
const g2 = await figma.getNodeByIdAsync("2:4"); // components and icons live here; instances work from any page
// Variables by name (local collections Primitives, Color, Size, Type).
const VM={}; for (const v of await figma.variables.getLocalVariablesAsync()) VM[v.name]=v;
const V=n=>{const v=VM[n]; if(!v) throw new Error("no var "+n); return v;};
const val=n=>{const v=V(n); return v.valuesByMode[Object.keys(v.valuesByMode)[0]];};
// Text and effect styles by name (Body/Default, Title/Section, Mono/Small, Elevation/Popup ...).
const TS={}; for (const s of await figma.getLocalTextStylesAsync()) TS[s.name]=s.id;
const ES={}; for (const s of await figma.getLocalEffectStylesAsync()) ES[s.name]=s.id;
for (const st of ["Regular","Medium","Semi Bold"]) await figma.loadFontAsync({family:"Inter",style:st});
await figma.loadFontAsync({family:"JetBrains Mono",style:"Regular"});
// Paints bound to colour variables, e.g. fill(node, "color/bg/surface").
const paint=(n,op)=>{const p=figma.variables.setBoundVariableForPaint({type:"SOLID",color:{r:.5,g:.5,b:.5}},"color",V(n)); return op==null?p:{...p,opacity:op};};
const fill=(nd,n,op)=>{nd.fills=n?[paint(n,op)]:[];return nd;};
const stroke=(nd,n,w="border/width",align="INSIDE",op)=>{ if(!n){nd.strokes=[];return nd;} nd.strokes=[paint(n,op)]; nd.strokeAlign=align; try{nd.setBoundVariable("strokeWeight",V(w));}catch(e){nd.strokeWeight=val(w);} return nd;};
// Spacing, radius and size bound to variables: pad(frame, 12, 0) uses space/12 and space/0.
const sp=(nd,k,n)=>nd.setBoundVariable(k,V(n));
const pad=(nd,h,v)=>{sp(nd,"paddingLeft","space/"+h);sp(nd,"paddingRight","space/"+h);sp(nd,"paddingTop","space/"+v);sp(nd,"paddingBottom","space/"+v);};
const gap=(nd,g)=>sp(nd,"itemSpacing","space/"+g);
const rad=(nd,r)=>{for(const k of ["topLeftRadius","topRightRadius","bottomLeftRadius","bottomRightRadius"]) sp(nd,k,"radius/"+r);};
const txt=(ch,st,col)=>{const t=figma.createText(); t.textStyleId=TS[st]; t.characters=ch; fill(t,col); t.name=ch.length<28?ch:"text"; return t;};
const trunc=t=>{t.textAutoResize="TRUNCATE"; t.textTruncation="ENDING"; t.maxLines=1;};
// Auto-layout frame with no fill. An empty one is 100x100: resize spacers to 1 px on the cross axis.
const AL=(dir,name)=>{const f=figma.createAutoLayout(dir); f.name=name; f.fills=[]; return f;};
const spacer=row=>{const s=AL("HORIZONTAL","spacer"); row.appendChild(s); s.layoutGrow=1; if(row.layoutMode==="HORIZONTAL"){s.layoutSizingVertical="FIXED"; s.resize(s.width,1);} return s;};
// Icons: icon("power", "color/state/armed", 20).
const ICON={}; for (const c of g2.findAll(n=>n.type==="COMPONENT"&&n.name.startsWith("Icon/"))) ICON[c.name.slice(5)]=c;
const icon=(nm,col,size=16)=>{const i=ICON[nm].createInstance(); i.name="icon"; if(col) i.children[0].strokes=[paint(col)]; if(size!==16) i.rescale(size/16); return i;};
// Component instances: inst("Button","Style=Primary, State=Default"); setP(i,{Label:"Save"}).
const SET=n=>{const s=g2.findOne(x=>x.type==="COMPONENT_SET"&&x.name===n); if(!s) throw new Error("no set "+n); return s;};
const inst=(set,variant)=>{const c=SET(set).children.find(c=>c.name===variant); if(!c) throw new Error(set+": no "+variant); return c.createInstance();};
const comp=n=>g2.findOne(x=>x.type==="COMPONENT"&&x.name===n); // single components: Menu, NavRail, Tooltip
const setP=(i,o)=>{const m={}; for(const [n,v] of Object.entries(o)){const k=Object.keys(i.componentProperties).find(k=>k.split("#")[0]===n); if(k) m[k]=v;} i.setProperties(m);};
const add=(p,c,fh,fv)=>{p.appendChild(c); if(fh) c.layoutSizingHorizontal="FILL"; if(fv) c.layoutSizingVertical="FILL"; return c;};
const fixH=(nd,n)=>{nd.counterAxisSizingMode="FIXED"; nd.resize(nd.width,val(n)); sp(nd,"height",n);};
// Building components: mkC(frame) turns a frame into a component; grid() lays out a set's variants in rows by one property.
const mkC=f=>figma.createComponentFromNode(f);
const parse=c=>Object.fromEntries(c.name.split(", ").map(p=>p.split("=")));
const grid=(set,rowProp,gx=24,gy=24,pd=32)=>{const kids=[...set.children]; const rows=[],rm={}; for(const c of kids){const r=rowProp?parse(c)[rowProp]:"all"; if(!(r in rm)){rm[r]=rows.length;rows.push([]);} rows[rm[r]].push(c);} const cw=Math.max(...kids.map(c=>c.width)); let y=pd; for(const row of rows){const rh=Math.max(...row.map(c=>c.height)); row.forEach((c,i)=>{c.x=pd+i*(cw+gx); c.y=y;}); y+=rh+gy;} set.resizeWithoutConstraints(pd*2+Math.max(...rows.map(r=>r.length))*(cw+gx)-gx, y-gy+pd); fill(set,"color/bg/app"); rad(set,"lg"); stroke(set,"color/border/default");};
// Component properties shared by all variants (only when the default fits every variant).
const prop=(set,name,type,def,sel,field)=>{const key=set.addComponentProperty(name,type,def); for(const v of set.children){ for(const n of v.findAll(sel)) n.componentPropertyReferences={...(n.componentPropertyReferences||{}),[field]:key}; } return key;};
// Sections on the G2 page: a title, then blocks of (title, description, node).
const section=(name,x,y)=>{let s=page.findOne(n=>n.type==="SECTION"&&n.name===name); if(!s){s=figma.createSection(); s.name=name; s.x=x; s.y=y; fill(s,"color/bg/surface"); const t=txt(name,"Title/Page","color/text/primary"); s.appendChild(t); t.x=40; t.y=40;} return s;};
const fitSection=s=>{let r=0,b=0; for(const c of s.children){r=Math.max(r,c.x+c.width); b=Math.max(b,c.y+c.height);} s.resizeWithoutConstraints(r+48,b+48);};
const block=(s,title,desc,node)=>{let b=0; for(const c of s.children) if(c!==node) b=Math.max(b,c.y+c.height); const t=txt(title,"Title/Section","color/text/primary"); s.appendChild(t); t.x=40; t.y=b+48; const d=txt(desc,"Caption","color/text/secondary"); s.appendChild(d); d.textAutoResize="HEIGHT"; d.resize(1100,d.height); d.x=40; d.y=t.y+32; if(node.parent!==s) s.appendChild(node); node.x=40; node.y=d.y+d.height+16; fitSection(s); return node;};
const TSn={}; for (const [n,id] of Object.entries(TS)) TSn[id]=n;
const restack=s=>{ const kids=[...s.children].sort((a,b)=>a.y-b.y); let bottom=0, lastTitle=null, lastDesc=null;
  for (const k of kids){ const st = k.type==="TEXT" ? TSn[k.textStyleId] : null;
    if (st==="Title/Page"){ k.y=40; bottom=k.y+k.height; }
    else if (st==="Title/Section"){ k.y=bottom+48; lastTitle=k; bottom=k.y+k.height; }
    else if (st==="Caption" && lastTitle && !lastDesc){ k.y=lastTitle.y+32; lastDesc=k; bottom=k.y+k.height; }
    else { k.y=bottom+16; bottom=k.y+k.height; lastTitle=null; lastDesc=null; } }
  fitSection(s); };
// Modes: frame.setExplicitVariableModeForCollection(await figma.variables.getVariableCollectionByIdAsync("VariableCollectionId:24:64"), "24:2") for Light.
