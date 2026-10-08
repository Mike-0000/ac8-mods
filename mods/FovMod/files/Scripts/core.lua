-- Pure policy and storage code; no Unreal or process access.
local M = {}
M.views = {"Cockpit", "HUD", "ThirdPerson"}
M.defaults = {Cockpit=0, HUD=0, ThirdPerson=0, Step=5,
    IncreaseKey="OEM_PLUS", DecreaseKey="OEM_MINUS", ToggleKey="F10", ReloadKey="F9"}
local function copy(t) local n={} for k,v in pairs(t) do n[k]=v end return n end
M.copy=copy
function M.fov(value)
    local n=tonumber(value)
    if not n or n~=n or n==math.huge or n==-math.huge or n<0 then return nil end
    if n==0 then return 0 end
    return math.max(40,math.min(150,n))
end
function M.parse(text, base)
    local cfg=copy(base or M.defaults)
    local warnings={}
    for line in (text.."\n"):gmatch("(.-)\r?\n") do
        local clean=line:gsub("[;#].*$","")
        local k,v=clean:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")
        if k and cfg[k]~=nil then
            if k=="Cockpit" or k=="HUD" or k=="ThirdPerson" then
                local n=M.fov(v); if n then cfg[k]=n else warnings[#warnings+1]="Invalid "..k end
            elseif k=="Step" then
                local n=tonumber(v)
                if n and n>=1 and n<=20 and n==math.floor(n) then cfg[k]=n else warnings[#warnings+1]="Invalid Step" end
            elseif v:match("^[A-Z][A-Z0-9_]*$") then cfg[k]=v
            else warnings[#warnings+1]="Invalid "..k end
        elseif k=="Other" and tonumber(v)~=0 then
            warnings[#warnings+1]="Other ignored: only the three flight views are supported"
        end
    end
    return cfg,warnings
end

-- Two-slot journal: the currently committed slot survives every failed write.
-- Commit promotes a complete temporary file to the unused slot via one rename.
-- This avoids Windows os.rename's inability to replace an existing file.
local function encode(g,c)
    return string.format("Generation=%d\nCockpit=%.6f\nHUD=%.6f\nThirdPerson=%.6f\nComplete=%d\n",g,c.Cockpit,c.HUD,c.ThirdPerson,g)
end
local function decode(s)
    if not s then return nil end
    local g,a,b,c,done=s:match("^Generation=(%d+)\nCockpit=([%d.]+)\nHUD=([%d.]+)\nThirdPerson=([%d.]+)\nComplete=(%d+)\n$")
    g=tonumber(g)
    if not g or g~=tonumber(done) or g<1 or g>9007199254740000 then return nil end
    local result={Cockpit=M.fov(a),HUD=M.fov(b),ThirdPerson=M.fov(c)}
    for _,k in ipairs(M.views) do if result[k]==nil or result[k]~=tonumber(({Cockpit=a,HUD=b,ThirdPerson=c})[k]) then return nil end end
    return {generation=g,values=result}
end
function M.storage(dir, fs)
    local function path(suffix) return dir.."/fov.state-"..suffix end
    local store={generation=0,slot=nil}
    function store:load(cfg)
        local a,b=decode(fs.read(path("a"))),decode(fs.read(path("b")))
        local chosen,slot
        if a then chosen,slot=a,"a" end
        if b and (not chosen or b.generation>chosen.generation) then chosen,slot=b,"b" end
        if chosen then
            self.generation,self.slot=chosen.generation,slot
            for _,k in ipairs(M.views) do cfg[k]=chosen.values[k] end
        end
        return cfg
    end
    function store:save(cfg)
        local slot=self.slot=="a" and "b" or "a"
        local temporary=path(slot..".tmp")
        local g=self.generation+1
        if g>9007199254740000 then return nil,"Settings generation limit" end
        local text=encode(g,cfg)
        local ok,err=fs.write(temporary,text)
        if not ok then return nil,err end
        if fs.read(temporary)~=text then return nil,"Temporary settings verification failed" end
        -- Removal affects only the inactive slot. The committed slot is retained.
        if fs.read(path(slot))~=nil then
            ok,err=fs.remove(path(slot)); if not ok then return nil,err end
        end
        ok,err=fs.rename(temporary,path(slot)); if not ok then return nil,err end
        self.generation,self.slot=g,slot
        return true
    end
    return store
end

function M.policy(write,log,sameWorld)
    sameWorld=sameWorld or function(a,b)return a==b end
    local p={owned=nil,blocked=false}
    local function matches(o,s)
        return o and s and o.controller==s.controller and o.manager==s.manager and sameWorld(o.world,s.world)
    end
    function p:release(s)
        local o=self.owned; self.owned=nil
        if matches(o,s) and s.valid and math.abs(s.angle-o.value)<=0.05 then write(s,0) end
    end
    function p:step(s,wanted)
        if self.blocked then return end
        local o=self.owned
        if not s or not s.valid then self.owned=nil; return end
        if o and (not matches(o,s) or o.target~=s.target or o.view~=s.view) then
            self:release(s); o=nil
        end
        if o and math.abs(s.angle-o.value)>0.05 then
            self.owned=nil; self.blocked=true
            log("FOV ownership lost; stopped writing for this session")
            return
        end
        if not s.flight or wanted==0 then self:release(s); return end
        if not o or o.value~=wanted then
            write(s,wanted)
            self.owned={controller=s.controller,manager=s.manager,world=s.world,target=s.target,view=s.view,value=wanted}
        end
    end
    return p
end
return M
