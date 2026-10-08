--[[
AcePilotsMod for ACE COMBAT 8: tougher enemy pilots (offline single-player only).

Settings live in acepilots.ini next to this mod's Scripts folder. acepilots_hook.dll (source in hook\)
owns that file: it draws the in-game menu (F6), steps through the presets (F8) and applies everything
that works on raw game data: manoeuvre odds, agility, missiles in the air, blackout tolerance.

This script loads the DLL and does the part that needs the engine's reflection, following the same ini:
  ForceRank           every enemy pilot's PilotRank, through the game's own SetPilotRank. The game sets
                      rank when a unit is set up and missions can change it later, so it is checked on a
                      timer rather than once at spawn.
  IgnoreAttackQueue, ForceFlares, ForceEvade, ForceTaunts
                      behaviour flags on each enemy unit. Permission to engage and to fire are left to
                      the mission.
Whatever it changes it puts back when the setting is turned off.

Log: acepilots.log in the same folder.
]]

local MOD_DIR = (debug.getinfo(1, "S").source:match("^@?(.*)[\\/][Ss]cripts[\\/]") or ".")
local INI_PATH = MOD_DIR .. "\\acepilots.ini"
local LOG_PATH = MOD_DIR .. "\\acepilots.log"
local HOOK_PATH = MOD_DIR .. "\\acepilots_hook.dll"

local UNIT_CLASS = "LiveAIGameObject"
local CHECK_EVERY_FRAMES = 30
local SPREAD_EVERY_SECONDS = 10
local ALLY_WORDS = { "ally", "wingman", "friend" }
-- ini key -> the unit flags it switches on
local FLAGS = {
    IgnoreAttackQueue = { "bIgnoreAttackQuota" },
    ForceFlares = { "bPermissionToUseFlares", "bCanActivateFlares" },
    ForceEvade = { "bPermissionToEvade" },
    ForceTaunts = { "bPermissionToProvoke" },
}

-- Only the keys this script acts on; the DLL reads the rest. Nothing is changed until the ini has been read.
local cfg = { ForceRank = 0, IgnoreAttackQueue = 0, ForceFlares = 0, ForceEvade = 0, ForceTaunts = 0, AirOnly = 1, IncludeAllies = 0 }

local units = {}
local stopped = false
local described = {}
local changed, flagged, flags_logged = 0, 0, 0
local hook_loaded = false
local ini_text = nil
local last_spread, next_spread = nil, 0
-- Faction values seen on units whose name marks them as allied; any unit of such a faction is an ally.
local ally_factions = {}

local function log(fmt, ...)
    local line = string.format(fmt, ...)
    print("[AcePilotsMod] " .. line .. "\n")
    local f = io.open(LOG_PATH, "a")
    if f then
        f:write(os.date("%H:%M:%S "), line, "\n")
        f:close()
    end
end

local function describe_cfg()
    return string.format("rank %s, attack queue %s, flares %s, evade %s, taunts %s, air only %d, allies %d",
        cfg.ForceRank > 0 and tostring(cfg.ForceRank) or "left to the game", cfg.IgnoreAttackQueue == 1 and "ignored" or "stock",
        cfg.ForceFlares == 1 and "forced" or "stock", cfg.ForceEvade == 1 and "forced" or "stock",
        cfg.ForceTaunts == 1 and "forced" or "stock", cfg.AirOnly, cfg.IncludeAllies)
end

-- Re-reads the ini when its text has changed. A key missing from the file means off.
local function load_cfg()
    local f = io.open(INI_PATH, "r")
    if not f then return end
    local text = f:read("a")
    f:close()
    if text == ini_text or text == "" then return end
    ini_text = text
    local seen = {}
    for key, value in ("\n" .. text):gmatch("\n[ \t]*(%w+)[ \t]*=[ \t]*([%d%.]+)") do seen[key] = tonumber(value) end
    if seen.ForceRank == nil and seen.Mode ~= nil then return end  -- old layout; the DLL converts it and writes it back
    for key in pairs(cfg) do cfg[key] = math.floor(seen[key] or (key == "AirOnly" and 1 or 0)) end
    cfg.ForceRank = math.max(0, math.min(5, cfg.ForceRank))
    last_spread = nil
    log("settings: %s", describe_cfg())
end

-- The DLL installs itself when loaded and owns the ini from then on, so there is nothing to call in it.
local function load_hook()
    if hook_loaded then return end
    -- An update copied in while the game was running waits next to the DLL; swap it in before loading.
    local staged = io.open(HOOK_PATH .. ".new", "rb")
    if staged then
        staged:close()
        os.remove(HOOK_PATH)
        local renamed, why = os.rename(HOOK_PATH .. ".new", HOOK_PATH)
        log(renamed and "hook DLL updated from staged copy" or "could not swap in the staged hook DLL: " .. tostring(why))
    end
    local ok, result, err = pcall(function() return package.loadlib(HOOK_PATH, "*") end)
    if ok and result then
        hook_loaded = true
        log("hook DLL loaded")
    else
        log("hook DLL NOT loaded: no menu, no presets, no manoeuvre or agility changes (%s). F8 switches rank 5 on and off.",
            tostring(ok and err or result))
    end
end

local function has_word(text, words)
    text = text:lower()
    for _, word in ipairs(words) do
        if text:find(word, 1, true) then return true end
    end
    return false
end

-- Once per class: log its parent chain, so the log shows what kinds of unit a mission has.
local function describe(unit, class_name)
    if described[class_name] then return end
    described[class_name] = true
    pcall(function()
        local chain, class = {}, unit:GetClass()
        while class and class:IsValid() and #chain < 12 do
            chain[#chain + 1] = class:GetFName():ToString()
            class = class:GetSuperStruct()
        end
        log("class %s", table.concat(chain, " > "))
    end)
end

local function track(object)
    if not object:IsValid() then return end
    for _, entry in ipairs(units) do
        if entry.object == object then return end
    end
    units[#units + 1] = { object = object }
end

-- Fills in what does not change for a unit.
local function identify(entry)
    local unit = entry.object
    entry.name = unit:GetFullName()
    entry.class = unit:GetClass():GetFName():ToString()
    entry.air = unit:IsAir()
    entry.dummy = unit:IsDummy()
    entry.named_ally = has_word(entry.name, ALLY_WORDS)
    local ok, faction = pcall(function() return unit.Faction end)
    entry.faction = (ok and type(faction) == "number") and faction or nil
    if entry.named_ally and entry.faction and not ally_factions[entry.faction] then
        ally_factions[entry.faction] = true
        log("faction %d is allied (from %s)", entry.faction, entry.class)
    end
    entry.known = true
    describe(unit, entry.class)
end

local function is_ally(entry)
    return entry.named_ally or (entry.faction ~= nil and ally_factions[entry.faction] == true)
end

-- Whether the settings reach this unit at all.
local function in_scope(entry)
    if entry.dummy then return false end
    if cfg.AirOnly == 1 and not entry.air then return false end
    if cfg.IncludeAllies ~= 1 and is_ally(entry) then return false end
    return true
end

-- Switches the flags the settings ask for on, and puts back any it switched on that are no longer asked for.
local function sync_flags(entry, scoped)
    local unit = entry.object
    local raised = nil
    for key, names in pairs(FLAGS) do
        local want = scoped and cfg[key] == 1
        for _, name in ipairs(names) do
            if want then
                local ok, value = pcall(function() return unit[name] end)
                if ok and type(value) == "boolean" then
                    entry.flags = entry.flags or {}
                    if entry.flags[name] == nil then entry.flags[name] = value end
                    if not value then
                        unit[name] = true
                        raised = raised or {}
                        raised[#raised + 1] = name
                    end
                end
            elseif entry.flags and entry.flags[name] ~= nil then
                unit[name] = entry.flags[name]
                entry.flags[name] = nil
            end
        end
    end
    if raised and not entry.flag_counted then
        entry.flag_counted = true
        flagged = flagged + 1
        if flags_logged < 8 then
            flags_logged = flags_logged + 1
            log("flags on %s: switched on %s", entry.class, table.concat(raised, ", "))
        end
    end
end

-- How many enemy aircraft sit at each rank right now; logged when it changes.
local function log_spread()
    if os.time() < next_spread then return end
    next_spread = os.time() + SPREAD_EVERY_SECONDS
    local counts, total = { 0, 0, 0, 0, 0 }, 0
    for _, entry in ipairs(units) do
        if entry.known and entry.air and not is_ally(entry) and not entry.dummy and entry.rank then
            local rank = math.max(1, math.min(5, entry.rank))
            counts[rank] = counts[rank] + 1
            total = total + 1
        end
    end
    local text = string.format("rank1=%d rank2=%d rank3=%d rank4=%d rank5=%d", table.unpack(counts))
    if total > 0 and text ~= last_spread then
        last_spread = text
        log("enemy air units by rank: %s (ranks set on %d units, flags on %d units so far)", text, changed, flagged)
    end
end

local function step()
    load_cfg()
    for i = #units, 1, -1 do
        local entry = units[i]
        local unit = entry.object
        if not unit:IsValid() then
            table.remove(units, i)
        else
            local rank = unit:GetPilotRank()
            -- 0 means the game has not set the unit up yet
            if rank > 0 then
                if not entry.known then identify(entry) end
                -- while this mod has not set the rank, whatever the game has is the original
                if not entry.applied then entry.original = rank end
                local scoped = in_scope(entry)
                local wanted = (scoped and cfg.ForceRank > 0) and cfg.ForceRank or nil
                if wanted and rank ~= wanted then
                    -- a rank that differs from the one this mod set means the game changed it; remember it
                    if entry.applied and rank ~= entry.applied and rank ~= entry.original then
                        entry.original = rank
                        log("rank reset by game to %d, set to %d again: %s", rank, wanted, entry.name)
                    end
                    unit:SetPilotRank(wanted)
                    if not entry.applied then
                        changed = changed + 1
                        if changed <= 12 then
                            log("rank %d -> %d (reads %d) %sfaction %s %s", rank, wanted, unit:GetPilotRank(),
                                entry.air and "air " or "ground ", tostring(entry.faction), entry.class)
                        end
                    end
                    entry.applied = wanted
                    rank = wanted
                elseif not wanted and entry.applied then
                    unit:SetPilotRank(entry.original)
                    entry.applied = nil
                    rank = entry.original
                end
                sync_flags(entry, scoped)
                entry.rank = rank
            end
        end
    end
    log_spread()
end

local function safe_step()
    if stopped then return end
    local ok, err = pcall(step)
    if not ok then
        stopped = true
        log("stopped after error: %s", tostring(err))
    end
end

-- The DLL handles F8 itself. Without it, F8 still switches the one thing this script can do alone.
RegisterKeyBind(Key.F8, function()
    if hook_loaded then return end
    ExecuteInGameThread(function()
        cfg.ForceRank = cfg.ForceRank > 0 and 0 or 5
        last_spread = nil
        log("settings: %s", describe_cfg())
    end)
end)

io.open(LOG_PATH, "w"):close()
load_hook()
load_cfg()

NotifyOnNewObject("/Script/Live." .. UNIT_CLASS, function(object)
    ExecuteInGameThread(function() track(object) end)
end)
ExecuteInGameThread(function()
    for _, object in ipairs(FindAllOf(UNIT_CLASS) or {}) do track(object) end
end)
LoopInGameThreadAfterFrames(CHECK_EVERY_FRAMES, safe_step)
log("loaded")
