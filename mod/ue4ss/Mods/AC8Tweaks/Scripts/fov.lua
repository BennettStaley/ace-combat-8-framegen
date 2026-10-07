--[[
Field of view per camera view: cockpit, HUD (first person without cockpit) and third person.

Mechanism found by the AC8 "FOV Change" mod (4598llj, Nexus mod 2), code written fresh here:
the game's LiveCameraViewComponent keeps one cached camera per view and marks the active one,
LivePlayerCameraManager reports the rendered angle, and PlayerController:FOV locks the final
value. FOV(0) releases the lock and gives the game's own dynamic field of view back.
Runs once per frame because the game re-derives its value every frame.

The game's zoom on a focused target (holding the target view button) is kept: every frame the
game eases the active camera's FieldOfView from its resting angle to a narrower one and back, and
the locked angle is narrowed by the same proportion.
]]

local F = {}
local TAG = "[AC8Tweaks] "
local VIEWS = {
    { name = "Cockpit", camera = "CachedCockpitCamera" },
    { name = "HUD", camera = "CachedFirstPersonCamera" },
    { name = "ThirdPerson", camera = "CachedThirdPersonCamera" },
}

local views, managers = {}, {}
local get_cfg -- returns { Cockpit = n, HUD = n, ThirdPerson = n }, 0 meaning the game's own value
local stopped = false

local function log(fmt, ...)
    print(TAG .. string.format(fmt, ...) .. "\n")
end

local function track(list, object)
    if not object:IsValid() then return end
    for _, entry in ipairs(list) do
        if entry.object == object then return end
    end
    list[#list + 1] = { object = object }
end

local function prune(list)
    for i = #list, 1, -1 do
        if not list[i].object:IsValid() then table.remove(list, i) end
    end
end

-- Name of the view the controller looks through, or nil when it is none of the three.
-- Also returns that view's camera and the entry of the plane's view component.
function F.view_of(manager)
    local controller = manager.PCOwner
    if not controller:IsValid() then return nil end
    local target = controller:GetViewTarget()
    for _, entry in ipairs(views) do
        local view = entry.object
        if view:IsValid() and view:GetOwner() == target then
            for _, v in ipairs(VIEWS) do
                local camera = view[v.camera]
                if camera:IsValid() and camera.bIsActive then return v.name, camera, entry end
            end
        end
    end
    return nil
end

-- The wanted angle, narrowed by as much as the game has its own camera zoomed in on a target.
-- The camera's resting angle is not exposed, so the widest angle seen on it stands in: the game
-- only ever eases between the resting angle and a narrower one.
function F.zoomed(wanted, view, camera, plane)
    local own = camera.FieldOfView
    if type(own) ~= "number" or own <= 0 then return wanted end
    plane.rest = plane.rest or {}
    plane.rest[view] = math.max(plane.rest[view] or 0, own)
    return wanted * own / plane.rest[view]
end

local function release(entry)
    if (entry.applied or 0) > 0 and entry.object:IsValid() and entry.object.PCOwner:IsValid() then
        entry.object.PCOwner:FOV(0)
    end
    entry.applied, entry.view = 0, nil
end

function F.step()
    local cfg = get_cfg()
    local wanted_any = (cfg.Cockpit or 0) > 0 or (cfg.HUD or 0) > 0 or (cfg.ThirdPerson or 0) > 0
    prune(managers)
    if not wanted_any then -- nothing to do beyond handing back anything we still hold
        for _, entry in ipairs(managers) do
            if (entry.applied or 0) > 0 then release(entry) end
        end
        return
    end
    prune(views)
    for _, entry in ipairs(managers) do
        local manager = entry.object
        local controller = manager.PCOwner
        if controller:IsValid() then
            local view, camera, plane = F.view_of(manager)
            local wanted = view and cfg[view] or 0
            local angle = wanted > 0 and F.zoomed(wanted, view, camera, plane) or 0
            local drifted = angle > 0 and math.abs(manager:GetFOVAngle() - angle) > 0.01
            if wanted ~= entry.applied or drifted then
                controller:FOV(angle)
            end
            if view ~= entry.view or wanted ~= entry.applied then
                log("fov %s: %s", view or "other", wanted == 0 and "game default" or tostring(wanted))
            end
            entry.view, entry.applied = view, wanted
        end
    end
end

-- What the player looks through right now: { view = "Cockpit" or nil, angle = degrees }.
function F.current()
    prune(managers)
    prune(views)
    local entry = managers[1]
    if not entry or stopped then return nil end
    local ok, result = pcall(function()
        return { view = F.view_of(entry.object), angle = entry.object:GetFOVAngle() }
    end)
    return ok and result or nil
end

-- { view = "Cockpit", applied = 100 } for the first camera manager, for state.ini
function F.status()
    local entry = managers[1]
    if not entry or stopped then return nil end
    return { view = entry.view, applied = entry.applied or 0 }
end

local function safe_step()
    if stopped then return end
    local ok, err = pcall(F.step)
    if not ok then
        stopped = true
        for _, entry in ipairs(managers) do pcall(release, entry) end
        log("fov stopped: %s", tostring(err))
    end
end

function F.start(cfg_getter)
    get_cfg = cfg_getter
    for _, pair in ipairs({ { "LiveCameraViewComponent", views }, { "LivePlayerCameraManager", managers } }) do
        local class, list = pair[1], pair[2]
        NotifyOnNewObject("/Script/Live." .. class, function(object)
            ExecuteInGameThread(function() track(list, object) end)
        end)
        ExecuteInGameThread(function()
            for _, object in ipairs(FindAllOf(class) or {}) do track(list, object) end
        end)
    end
    LoopInGameThreadAfterFrames(1, safe_step)
end

return F
