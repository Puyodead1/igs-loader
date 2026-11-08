/*
 * Speed Driver 4: chunk 0 of the two Lua chunks its IGS036 serves (cmd 9),
 * ours: a newer lib.machine than speed_lua.h's (Speed Driver 3, Power Truck)
 * and the globals only the chip's chunk defined.  Chunk 1 is
 * SPEED_LUA_BOOT("SD3Main") (the main component kept SD3's name).
 *
 * lib.machine as SD4's scripts use it:
 *   Machine(...)          global: the module is a machine; its env resolves
 *                         State, Group, Include, setState, getStateName and
 *                         `_` (the current run) without local aliases
 *   State "n" {enter(arg), update(ft), exit, on = {Ev = f}, timer = {[s] = f}}
 *   Group(names...) {...}
 *   Include "mod"         run another module's file in this machine's env
 *   Finalize = f, Initialize = f   assignments: at turnOff / turnOn
 *   setState(name [, run [, arg]])  arg goes to the state's enter
 *   getStateName([run])   "" when none (the C side reads it as a string)
 *   get(name)             the machine module's table
 *   setPath(script, speed) (kept; modules load through require's loaders)
 * plus SD3's create, turnOn, turnOff([run]), turnOffComponent, update,
 * getRuns, setComponentState, getComponentState, setDebug.
 */
#ifndef SD4_LUA_H
#define SD4_LUA_H

#define SD4_LUA_MACHINE \
"igs_call=function(f,...)\n" \
" local a={...}\n" \
" local ok,err=xpcall(function() return f(unpack(a)) end,debug.traceback)\n" \
" if not ok then io.stderr:write('[lua] '..tostring(err)..'\\n') end\n" \
"end\n" \
"package.preload['lib.machine']=function(...)\n" \
"local y=coroutine.yield\n" \
"coroutine.yield=function(...) if coroutine.running() then return y(...) end end\n" \
"local _G,lib,pairs,ipairs,type,rawget,rawset,setmetatable,setfenv,getfenv,require,table,string,package=\n" \
" _G,lib,pairs,ipairs,type,rawget,rawset,setmetatable,setfenv,getfenv,require,table,string,package\n" \
"module(...)\n" \
"local M=_M\n" \
"local runs,cur,dbg,def={},nil,nil,nil\n" \
"local api={State=true,Group=true,Include=true,setState=true,getStateName=true}\n" \
"local function call(r,f,...)\n" \
" if f then local p=cur cur=r f(...) cur=p end\n" \
"end\n" \
"local function slot(m,t)\n" \
" if t.on then for e in pairs(t.on) do lib.event.createRunSlot(m.name_,e) end end\n" \
"end\n" \
"local function merge(s,g)\n" \
" if not g.on then return end\n" \
" s.on=s.on or {}\n" \
" for e,h in pairs(g.on) do if s.on[e]==nil then s.on[e]=h end end\n" \
"end\n" \
"local function mach(lv)\n" \
" local e=getfenv(lv)\n" \
" if type(e)=='table' and rawget(e,'states_') then return e end\n" \
" return def\n" \
"end\n" \
"Machine=function(name)\n" \
" local m=package.loaded[name]\n" \
" local t,parts=_G,{}\n" \
" for p in string.gmatch(name,'[^%.]+') do parts[#parts+1]=p end\n" \
" for i=1,#parts-1 do\n" \
"  local v=rawget(t,parts[i])\n" \
"  if type(v)~='table' then v={} t[parts[i]]=v end\n" \
"  t=v\n" \
" end\n" \
" if type(m)~='table' then m=rawget(t,parts[#parts]) end\n" \
" if type(m)~='table' then m={} end\n" \
" t[parts[#parts]]=m\n" \
" package.loaded[name]=m\n" \
" rawset(m,'name_',name) rawset(m,'_NAME',name) rawset(m,'_M',m)\n" \
" rawset(m,'states_',{}) rawset(m,'groups_',{})\n" \
" setmetatable(m,{\n" \
"  __index=function(_,k) if k=='_' then return cur end if api[k] then return M[k] end return _G[k] end,\n" \
"  __newindex=function(t,k,v) if k=='_' then cur=v else rawset(t,k,v) end end})\n" \
" def=m\n" \
" setfenv(2,m)\n" \
"end\n" \
"_G.Machine=Machine\n" \
"State=function(name)\n" \
" local m=mach(3)\n" \
" return function(t)\n" \
"  t.name_=name\n" \
"  m.states_[name]=t\n" \
"  if not m.first_ then m.first_=t end\n" \
"  for _,g in ipairs(m.groups_) do if g.members_[name] then merge(t,g) end end\n" \
"  slot(m,t)\n" \
" end\n" \
"end\n" \
"Group=function(...)\n" \
" local names={...}\n" \
" local m=mach(3)\n" \
" return function(t)\n" \
"  t.members_={}\n" \
"  for _,n in ipairs(names) do t.members_[n]=true end\n" \
"  table.insert(m.groups_,t)\n" \
"  for n in pairs(t.members_) do local s=m.states_[n] if s then merge(s,t) end end\n" \
"  slot(m,t)\n" \
" end\n" \
"end\n" \
"Include=function(name)\n" \
" local m=mach(3)\n" \
" for _,ld in ipairs(package.loaders) do\n" \
"  local f=ld(name)\n" \
"  if type(f)=='function' then\n" \
"   local p=def def=m setfenv(f,m) f(name) def=p\n" \
"   return\n" \
"  end\n" \
" end\n" \
" _G.warn('lib.machine.Include: no module',name)\n" \
"end\n" \
"local function go(r,name,arg)\n" \
" local m=r.m_\n" \
" local s=m.states_[name]\n" \
" if not s then _G.warn('lib.machine: no state',name,'in',m.name_) return end\n" \
" if r.busy_ then r.next_=name r.arg_=arg return end\n" \
" r.busy_=true\n" \
" local o=r.state_\n" \
" if o then\n" \
"  call(r,o.exit)\n" \
"  for _,g in ipairs(m.groups_) do if g.members_[o.name_] and not g.members_[name] then call(r,g.exit) end end\n" \
" end\n" \
" r.state_=s r.time_=0 r.fired_={}\n" \
" for _,g in ipairs(m.groups_) do if g.members_[name] and not (o and g.members_[o.name_]) then call(r,g.enter) end end\n" \
" call(r,s.enter,arg)\n" \
" r.busy_=nil\n" \
" if dbg and dbg.setState then local p=cur cur=r dbg.setState(m) cur=p end\n" \
" local n,a=r.next_,r.arg_\n" \
" if n and not r.off_ then r.next_=nil r.arg_=nil go(r,n,a) end\n" \
"end\n" \
"create=function(class)\n" \
" local ok=_G.pcall(require,class)\n" \
" local m=package.loaded[class]\n" \
" if type(m)~='table' or not rawget(m,'states_') then _G.warn('lib.machine: not a machine',class) return end\n" \
" return m\n" \
"end\n" \
"get=function(name)\n" \
" local m=package.loaded[name]\n" \
" if type(m)~='table' then _G.pcall(require,name) m=package.loaded[name] end\n" \
" return m\n" \
"end\n" \
"setPath=function(s,p) M.scriptPath_=s M.speedPath_=p end\n" \
"turnOn=function(m,c)\n" \
" if not m or not c then return end\n" \
" if c.m_ and not c.off_ then turnOff(c) end\n" \
" c.m_=m c.off_=nil c.state_=nil c.next_=nil c.busy_=nil\n" \
" if not c.listed_ then c.listed_=true table.insert(runs,c) end\n" \
" lib.event.registerRun(c)\n" \
" call(c,rawget(m,'Initialize'))\n" \
" if m.first_ then go(c,m.first_.name_) end\n" \
"end\n" \
"turnOff=function(c)\n" \
" c=c or cur\n" \
" if not c or not c.m_ or c.off_ then return end\n" \
" local m,s=c.m_,c.state_\n" \
" if s then\n" \
"  call(c,s.exit)\n" \
"  for _,g in ipairs(m.groups_) do if g.members_[s.name_] then call(c,g.exit) end end\n" \
" end\n" \
" local f=rawget(m,'Finalize')\n" \
" if type(f)=='table' then f=f.exit end\n" \
" call(c,f)\n" \
" c.off_=true\n" \
" lib.event.unregisterRun(c)\n" \
"end\n" \
"local function comp(e,sn)\n" \
" local cs=lib.getComponents(sn)\n" \
" return cs and cs[e]\n" \
"end\n" \
"turnOffComponent=function(e,sn) local c=comp(e,sn) if c then turnOff(c) end end\n" \
"update=function(ft)\n" \
" local i=1\n" \
" while i<=#runs do\n" \
"  local r=runs[i]\n" \
"  if r.off_ then r.listed_=nil table.remove(runs,i)\n" \
"  else\n" \
"   local s=r.state_\n" \
"   if s then\n" \
"    call(r,s.update,ft)\n" \
"    for _,g in ipairs(r.m_.groups_) do if g.update and g.members_[s.name_] then call(r,g.update,ft) end end\n" \
"    if s.timer and r.state_==s then\n" \
"     r.time_=(r.time_ or 0)+(ft or 0)\n" \
"     for t,f in pairs(s.timer) do\n" \
"      if r.state_~=s then break end\n" \
"      if r.time_>=t and not r.fired_[t] then r.fired_[t]=true call(r,f) end\n" \
"     end\n" \
"    end\n" \
"   end\n" \
"   i=i+1\n" \
"  end\n" \
" end\n" \
" if dbg and dbg.update then dbg.update() end\n" \
"end\n" \
"getRuns=function() return runs end\n" \
"setState=function(name,r,arg)\n" \
" r=r or cur\n" \
" if r and r.m_ then go(r,name,arg) else _G.warn('lib.machine.setState: no machine',name) end\n" \
"end\n" \
"setComponentState=function(name,e,sn)\n" \
" local c=comp(e,sn)\n" \
" if c and c.m_ then go(c,name) else _G.warn('lib.machine.setComponentState: no machine',e,sn) end\n" \
"end\n" \
"getStateName=function(c)\n" \
" c=c or cur\n" \
" if c and c.state_ then return c.state_.name_ end\n" \
" return ''\n" \
"end\n" \
"getComponentState=function(e,sn)\n" \
" local c=comp(e,sn)\n" \
" if c and c.state_ then return c.state_.name_ end\n" \
"end\n" \
"setDebug=function(d) dbg=d end\n" \
"end\n"

/* globals the scripts call that only the chip's chunk defined (wiki Speed Driver 4) */
#define SD4_LUA_GLOBALS \
"trimLogDir=function() end\n" \
"getHardwareClock=function() return os.date('*t') end\n" \
"getMemoryInfo=function() return '' end\n" \
"set321Go=function() end\n" \
"setIsSoloGame=function() end\n" \
"enableNetGame=function() end\n" \
"logNet=function(...) if say then say(...) end end\n" \
"checkCardID=function() end\n"

#endif
