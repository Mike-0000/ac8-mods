-- Per-View FOV candidate 1.1.0. Offline single player only.
-- Qualified deployment must provide the pinned runtime and a single FOV writer.
local dir=assert(debug.getinfo(1,"S").source:match("^@?(.*)[\\/][Ss]cripts[\\/]"),"Cannot resolve mod directory")
local core=dofile(dir.."/Scripts/core.lua")
local fs={}
function fs.read(path)
    local f=io.open(path,"rb"); if not f then return nil end
    local s=f:read(65537); f:close()
    if not s or #s>65536 then return nil end
    return s
end
function fs.write(path,text)
    local f,err=io.open(path,"wb"); if not f then return nil,err end
    local ok,e=f:write(text); local closed,ce=f:close()
    if not ok or not closed then return nil,e or ce end
    return true
end
fs.remove=os.remove; fs.rename=os.rename
local logCount=0
local function log(message)
    if logCount>=160 then return end
    logCount=logCount+1
    message=tostring(message):sub(1,500)
    print("[FovMod] "..message.."\n")
    local f=io.open(dir.."/fovmod.log",logCount==1 and "w" or "a")
    if f then f:write(os.date("%H:%M:%S "),message,"\n"); f:close() end
end
for _,name in ipairs({"FindAllOf","NotifyOnNewObject","ExecuteInGameThread","LoopInGameThreadAfterFrames","RegisterKeyBind","StaticFindObject"}) do
    if type(_G[name])~="function" then log("Disabled: missing runtime API "..name); return end
end
if EngineTickAvailable~=true then log("Disabled: EngineTick is unavailable; use the documented tested runtime"); return end
if type(Key)~="table" then log("Disabled: key binding API unavailable"); return end
local gameplay=StaticFindObject("/Script/Engine.Default__GameplayStatics")
if not gameplay or not gameplay:IsValid() then log("Disabled: gameplay state API unavailable");return end
local cfg,warnings=core.parse(fs.read(dir.."/fov.ini") or fs.read(dir.."/defaults.ini") or "")
for _,w in ipairs(warnings) do log(w) end
local boundKeys=core.copy(cfg)
local journal=core.storage(dir,fs)
journal:load(cfg)
local enabled,stopped=true,false
local managers,views={},{}
local VIEWS={{"Cockpit","CachedCockpitCamera"},{"HUD","CachedFirstPersonCamera"},{"ThirdPerson","CachedThirdPersonCamera"}}
local function valid(o) return o~=nil and o:IsValid() end
-- This pinned runtime's UWorld wrapper is not classified as UObject by __eq.
-- Compare live world addresses explicitly; retain wrappers for validity checks.
local function sameWorld(a,b)
    return valid(a) and valid(b) and a:GetAddress()~=0 and a:GetAddress()==b:GetAddress()
end
local function track(list,o)
    if not valid(o) then return end
    for _,x in ipairs(list) do if x==o then return end end
    list[#list+1]=o
end
local function prune(list)
    for i=#list,1,-1 do if not valid(list[i]) then table.remove(list,i) end end
end
local function observe()
    prune(managers); prune(views)
    local selected=nil
    local rejected={}
    for _,m in ipairs(managers) do
        local c=m.PCOwner
        if valid(c) and c:IsLocalController() then
            local w=c:GetWorld()
            if sameWorld(m:GetWorld(),w) then
                if selected then return nil,"Multiple local camera managers" end -- Do not choose list position.
                local target=c:GetViewTarget()
                selected={controller=c,manager=m,world=w,target=target,valid=true,angle=m:GetFOVAngle(),flight=false,reason="No possessed flight camera"}
                local possessed=valid(target) and valid(c.Pawn) and target==c.Pawn
                local paused=gameplay:IsGamePaused(c)
                local ignored=c:IsMoveInputIgnored() or c:IsLookInputIgnored()
                if paused then selected.reason="Paused"
                elseif ignored then selected.reason="Controls ignored"
                elseif not possessed then selected.reason="View target is not the possessed pawn"
                else selected.reason="No unique active flight camera" end
                if possessed and not paused and not ignored then
                    for _,v in ipairs(views) do
                        local owner=v:GetOwner()
                        if owner==target and sameWorld(owner:GetWorld(),w) then
                            for _,spec in ipairs(VIEWS) do
                                local camera=v[spec[2]]
                                if valid(camera) and camera.bIsActive then
                                    if selected.view then selected.flight=false; return selected end
                                    selected.view=spec[1]; selected.flight=true;selected.reason=spec[1]
                                end
                            end
                        end
                    end
                end
            else
                rejected[#rejected+1]="invalid or mismatched world"
            end
        else
            rejected[#rejected+1]=valid(c) and "non-local controller" or "invalid controller"
        end
    end
    return selected,"No unique local camera (managers="..#managers..", views="..#views.."; "..table.concat(rejected,", ")..")"
end
local policy=core.policy(function(s,value) s.controller:FOV(value) end,log,sameWorld)
local lastState,confirmedValue=nil,nil
local function releaseOwned()
    local o=policy.owned
    if not o then return end
    if valid(o.controller) and valid(o.manager) and valid(o.world)
        and o.manager.PCOwner==o.controller and sameWorld(o.controller:GetWorld(),o.world) and sameWorld(o.manager:GetWorld(),o.world) then
        policy:release({controller=o.controller,manager=o.manager,world=o.world,valid=true,angle=o.manager:GetFOVAngle()})
    else policy.owned=nil end
end
local function stop(err)
    if stopped then return end
    stopped=true
    -- Never blanket-reset controllers after a failed reflective operation.
    pcall(releaseOwned)
    log("Disabled after API failure: "..tostring(err))
end
local function guarded(fn)
    if stopped then return end
    local ok,err=pcall(fn); if not ok then stop(err) end
end
local function step()
    guarded(function()
        local s,reason=observe()
        if not s then
            releaseOwned()
            if lastState~=reason then lastState=reason;log(lastState) end
            return
        end
        if lastState~=s.reason then lastState=s.reason;confirmedValue=nil;log("Camera: "..s.reason) end
        local wanted=enabled and s.flight and cfg[s.view] or 0
        policy:step(s,wanted)
        if policy.owned and wanted>0 and math.abs(s.angle-wanted)<=0.05 and confirmedValue~=wanted then
            confirmedValue=wanted;log(string.format("Observed %s FOV %.2f",s.view,s.angle))
        elseif wanted==0 then confirmedValue=nil end
    end)
end
local storageWarning=false
local function save()
    local ok,err=journal:save(cfg)
    if not ok and not storageWarning then storageWarning=true;log("Settings remain in memory; save failed: "..tostring(err)) end
end
local function action(fn)
    return function() if not stopped then ExecuteInGameThread(function() guarded(fn) end) end end
end
local lastIgnored=nil
local function nudge(delta)
    local s,reason=observe()
    if not enabled or policy.blocked or not s or not s.flight then
        local why=not enabled and "disabled" or policy.blocked and "ownership lost" or s and s.reason or reason
        if why~=lastIgnored then log("Adjustment ignored: "..why);lastIgnored=why end
        return
    end
    lastIgnored=nil
    local current=cfg[s.view]
    if current==0 then current=math.floor(s.angle/cfg.Step+0.5)*cfg.Step end
    cfg[s.view]=core.fov(current+delta*cfg.Step) or cfg[s.view]
    save(); step(); log(s.view.."="..cfg[s.view])
end
local actions={IncreaseKey=function() nudge(1) end,DecreaseKey=function() nudge(-1) end,
    ToggleKey=function() enabled=not enabled;step();log(enabled and "Enabled" or "Disabled") end,
    ReloadKey=function()
        local text=fs.read(dir.."/fov.ini")
        if not text then log("Reload skipped: fov.ini missing or unreadable");return end
        local nextCfg,w=core.parse(text,core.defaults)
        for _,message in ipairs(w) do log(message) end
        for _,key in ipairs({"IncreaseKey","DecreaseKey","ToggleKey","ReloadKey"}) do
            if nextCfg[key]~=boundKeys[key] then log("Key changes require a game restart");break end
        end
        cfg=nextCfg; save(); step(); log("Reloaded fov.ini")
    end}
local keys={}
for _,name in ipairs({"IncreaseKey","DecreaseKey","ToggleKey","ReloadKey"}) do
    local value=Key[cfg[name]]
    if type(value)~="number" or keys[value] then log("Disabled: invalid or duplicate key "..cfg[name]);return end
    keys[value]=true
end
for name,fn in pairs(actions) do RegisterKeyBind(Key[cfg[name]],action(fn)) end
for _,pair in ipairs({{"LiveCameraViewComponent",views},{"LivePlayerCameraManager",managers}}) do
    local class,list=pair[1],pair[2]
    NotifyOnNewObject("/Script/Live."..class,function(o)
        ExecuteInGameThread(function() guarded(function() track(list,o) end) end)
    end)
    ExecuteInGameThread(function() guarded(function()
        for _,o in ipairs(FindAllOf(class) or {}) do track(list,o) end
    end) end)
end
LoopInGameThreadAfterFrames(1,step)
log("Candidate loaded; single-player flight views only. No override outside a possessed flight camera.")
