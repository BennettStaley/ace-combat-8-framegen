--[[
On-screen text for AC8 Tweaks, drawn by the game's own UI layer (a UMG user widget holding one
text block), so it works with frame generation, HDR and screenshots. Shown for a few seconds
whenever a setting changes, hidden otherwise. Every engine call is guarded: if the widget cannot
be built the rest of the mod carries on without it.
]]

local H = {}
local TAG = "[AC8Tweaks] "
local HIDDEN, HIT_TEST_INVISIBLE = 2, 3 -- ESlateVisibility

local UEHelpers
local widget, text
local generation = 0
local disabled = false

local function log(fmt, ...)
    print(TAG .. string.format(fmt, ...) .. "\n")
end

local function build()
    local pc = UEHelpers.GetPlayerController()
    if not pc:IsValid() then return false end
    local lib = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary")
    local widget_class = StaticFindObject("/Script/UMG.UserWidget")
    local text_class = StaticFindObject("/Script/UMG.TextBlock")
    if not (lib:IsValid() and widget_class:IsValid() and text_class:IsValid()) then
        error("UMG classes not found")
    end
    local w = lib:Create(pc, widget_class, pc)
    if not w:IsValid() then error("widget not created") end
    local tree = w.WidgetTree
    if not tree:IsValid() then error("widget has no tree") end
    local t = StaticConstructObject(text_class, tree)
    if not t:IsValid() then error("text block not created") end
    tree.RootWidget = t
    pcall(function()
        t:SetShadowOffset({ X = 2, Y = 2 })
        t:SetShadowColorAndOpacity({ R = 0, G = 0, B = 0, A = 0.9 })
    end)
    w:AddToViewport(1000)
    w:SetPositionInViewport({ X = 80, Y = 160 }, true)
    widget, text = w, t
    return true
end

local function ready()
    if disabled then return false end
    if widget and widget:IsValid() and text and text:IsValid() then return true end
    local ok, built = pcall(build)
    if not ok then
        disabled = true
        log("on-screen text unavailable: %s", tostring(built))
        return false
    end
    return built
end

-- Shows message for `seconds`, replacing whatever is up. Safe to call from any hotkey or tick.
function H.show(message, seconds)
    if not ready() then return false end
    generation = generation + 1
    local mine = generation
    local ok, err = pcall(function()
        text:SetText(FText(message))
        widget:SetVisibility(HIT_TEST_INVISIBLE)
    end)
    if not ok then
        log("on-screen text failed: %s", tostring(err))
        return false
    end
    ExecuteWithDelay(math.floor((seconds or 3) * 1000), function()
        ExecuteInGameThread(function()
            if mine == generation then H.hide() end
        end)
    end)
    return true
end

function H.hide()
    if widget and widget:IsValid() then pcall(function() widget:SetVisibility(HIDDEN) end) end
end

function H.start(helpers)
    UEHelpers = helpers
end

return H
