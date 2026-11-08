/*
 * Power Truck (E3000) game plugin: Speed Driver 3's engine and chip side
 * (games/speeddriver3/speed_chip.h) with this game's flow table and Lua
 * chunks.  wiki Power Truck.
 */
#include "../speeddriver3/speed_chip.h"
#include "../speeddriver3/speed_lua.h"

/*
 * The main flow (game.state.map: 1 OperatorSetting .. 33 WaitInterruptOver),
 * rebuilt from game/pt_main.lua and the test scripts.  The attract loop goes
 * by name; the chip decides the play, results and service paths.
 */
static const uint8_t flow[34][5] = {
    [1] = { 32 },                   /* OperatorSetting: exit */
    [10] = { 11 },                  /* InputName (card only) */
    [11] = { 14 },                  /* PlayerInfo (card only) */
    [12] = { 30 },                  /* SelectGear: done */
    [13] = { 20 },                  /* SelectNewTruck (card only) */
    [15] = { 16 },                  /* SelectVersus: solo */
    [17] = { 18 },                  /* SoloChallenge: mode picked */
    [18] = { 12 },                  /* SelectStage: stage picked */
    [19] = { 22, 13 },              /* SignUp: staff, new truck */
    [20] = { 16, 18, 12, 21 },      /* Continue: menu, next stage, retry, end */
    [21] = { 25, 24 },              /* GameOver: no rank, rank */
    [23] = { 20 },                  /* StaffEnd */
    [24] = { 25 },                  /* OverRanking: done */
    [25] = { 3 },                   /* OverReplay: no replay */
    [26] = { 19 },                  /* Ability: done */
    [27] = { 28 },                  /* MultiAbility: done */
    [28] = { 16, 21 },              /* Continue_NetTest: continue, decline */
    [29] = { 12 },                  /* NetSelectStage: voted */
    [30] = { 33, 26, 19, 27, 28 },  /* Game: interrupt, ability, solo end, versus defeat, net end */
    [31] = { 23, 3 },               /* Replay: staff end, replay end */
    [32] = { 2 },                   /* OperatorSettingCleanUp */
};

/*
 * The globals Power Truck's scripts call that only its boot chunk defined
 * (from how they are used):
 *   prepareRepFile()         as the main machine starts: the stage replays
 *                            (rep_file/<name>.rep and enc_script/replay/
 *                            <name>.lua.enc) restored from default/ if missing
 *   initReplay()             entering Game: record the race into rp_temp.rep
 *                            (the replay machine starts on ReplayStart)
 *   saveReplay()             the recording done: a run that earns the stage's
 *                            replay (misc.isReplayAvalible) becomes it, the
 *                            run's .rep and a plain <name>_user.lua script
 *   playReplay(name)         Staff, OverReplay: the stage's replay (the
 *                            player's if saved), then the Replay state
 *   getSize(t)               the number of entries in t (0 for none)
 *   searchIdx(t, v)          the index of v in t
 *   searchKeyMin(t)          t's smallest key
 *   searchKeyL(t, x)         t's largest key not above x (drift ring bonus)
 *   dbConnector              the network database: offline ("0" = no connection)
 */
#define PT_GLOBALS \
"local function cp(a,b,keep)\n" \
" if keep and lib.utility.isFileExist(b) then return end\n" \
" local f=io.open(a,'rb') if not f then return end\n" \
" local d=f:read('*a') f:close()\n" \
" local o=io.open(b,'wb') if o then o:write(d) o:close() end\n" \
"end\n" \
"prepareRepFile=function()\n" \
" local r,s=config.repFilePath,config.repScriptPath\n" \
" for _,st in ipairs({'airport','arctic','factory','highway','peiyi','street'}) do\n" \
"  for _,v in ipairs({'axx','bxx','rp_1'}) do\n" \
"   local n='pt_'..st..'_'..v\n" \
"   cp(r..'default/'..n..'.rep',r..n..'.rep',true)\n" \
"   cp(s..'default/'..n..'.lua.enc',s..n..'.lua.enc',true)\n" \
"  end\n" \
" end\n" \
" for _,n in ipairs({'rp_last','rp_temp'}) do\n" \
"  cp(r..'default/'..n..'.rep',r..n..'.rep',true)\n" \
"  cp(s..'default/'..n..'.lua.enc',s..n..'.lua.enc',true)\n" \
" end\n" \
"end\n" \
"local function main() return lib.getComponents'PTMain'.Game end\n" \
"initReplay=function()\n" \
" if lib.machine.getComponentState('Game','PTMain')=='Game' then main().replayFile='rp_temp.rep' end\n" \
"end\n" \
"saveReplay=function()\n" \
" local g=main()\n" \
" local name=game.data_table.replay.ReplayTable[g.stage]\n" \
" if not name or g.replayFile~='rp_temp.rep' then return end\n" \
" if not (config.enableAllStageReplay or game.main_helper.misc.isReplayAvalible()) then return end\n" \
" cp(config.repFilePath..'rp_temp.rep',config.repFilePath..name..'_user.rep')\n" \
" local t={}\n" \
" for k,v in pairs(speed.my.profile) do\n" \
"  local ty=type(v)\n" \
"  if type(k)=='string' and (ty=='number' or ty=='boolean' or ty=='string') then\n" \
"   t[#t+1]=string.format('\\t%s = %s,',k,ty=='string' and string.format('%q',v) or tostring(v))\n" \
"  end\n" \
" end\n" \
" local o=io.open(config.repScriptPath..name..'_user.lua','w')\n" \
" if o then\n" \
"  o:write('require \"speed.replay_lib\"\\nReplay\\n{\\n\\tstage = ',string.format('%q',g.stage),\n" \
"   ',\\n\\tfile = ',string.format('%q',name..'_user.rep'),',\\nProfile =\\n{\\n',table.concat(t,'\\n'),'\\n},\\n}\\n')\n" \
"  o:close()\n" \
" end\n" \
"end\n" \
"playReplay=function(name)\n" \
" if not name then\n" \
"  game.setMainState(lib.machine.getComponentState('Game','PTMain')=='Staff' and 'StaffEnd' or 0)\n" \
"  return\n" \
" end\n" \
" local u=config.repScriptPath..name..'_user.lua'\n" \
" local f=io.open(u,'r')\n" \
" if f then f:close() dofile(u) else dofile(config.repScriptPath..name..'.lua') end\n" \
" game.setMainState('Replay')\n" \
"end\n" \
"getSize=function(t)\n" \
" local n=0 if t then for _ in pairs(t) do n=n+1 end end return n\n" \
"end\n" \
"searchIdx=function(t,v)\n" \
" if t then for i,x in pairs(t) do if x==v then return i end end end\n" \
"end\n" \
"searchKeyMin=function(t)\n" \
" local m for k in pairs(t) do if m==nil or k<m then m=k end end return m\n" \
"end\n" \
"searchKeyL=function(t,x)\n" \
" local b for k in pairs(t) do if k<=x and (b==nil or k>b) then b=k end end return b\n" \
"end\n" \
"dbConnector={\n" \
" execute=function() return '0' end,\n" \
" dbConnectionTest=function() return false end,\n" \
"}\n"

static const char chunk0[] = SPEED_LUA_MACHINE SPEED_LUA_DIALOG SPEED_LUA_END;
static const char chunk1[] = PT_GLOBALS SPEED_LUA_BOOT("PTMain");
/* the sizes the game asks for (its arguments 25020 7056 10080), less the trailer */
_Static_assert(sizeof(chunk0) - 1 + 16 <= 7056, "lib.machine chunk too large");
_Static_assert(sizeof(chunk1) - 1 + 16 <= 10080, "boot chunk too large");

static const struct speed_title title = {
    .name = "Power Truck",
    .log = "powertruck",
    .fram = "/PT/readwrite/a36_fram.bin",
    .flow = flow,
    .flow_states = 34,
    .chunk = { chunk0, chunk1 },
    .handshake = E2100_HS_SESSION,
};
