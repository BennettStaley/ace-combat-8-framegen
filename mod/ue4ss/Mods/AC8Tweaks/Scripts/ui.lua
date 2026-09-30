--[[
Writes the layout of every user widget currently on screen to a text file: class, name,
visibility, canvas anchors and offsets, render transform, nested as the game nests them.
Used to find out how the HUD is arranged before changing it. Every engine call is guarded, so
whatever the widgets do not support just shows up as "?".
]]

local U = {}

local function str(v)
    local ok, r = pcall(function()
        if type(v) == "userdata" and v.ToString then return v:ToString() end
        return tostring(v)
    end)
    return ok and r or "?"
end

local function vec(v)
    local ok, r = pcall(function() return string.format("%.2f,%.2f", v.X, v.Y) end)
    return ok and r or "?"
end

local function margin(m)
    local ok, r = pcall(function() return string.format("%.1f,%.1f,%.1f,%.1f", m.Left, m.Top, m.Right, m.Bottom) end)
    return ok and r or "?"
end

local function name_of(widget)
    local ok, r = pcall(function() return widget:GetClass():GetFName():ToString() .. " " .. widget:GetFName():ToString() end)
    return ok and r or "?"
end

local function slot_info(widget)
    local ok, r = pcall(function()
        local slot = widget.Slot
        if not slot:IsValid() then return "" end
        local parts = { "slot=" .. slot:GetClass():GetFName():ToString() }
        pcall(function()
            local a = slot:GetAnchors()
            parts[#parts + 1] = "anchors=" .. vec(a.Minimum) .. ">" .. vec(a.Maximum)
            parts[#parts + 1] = "offsets=" .. margin(slot:GetOffsets())
            parts[#parts + 1] = "align=" .. vec(slot:GetAlignment())
        end)
        return table.concat(parts, " ")
    end)
    return ok and r or ""
end

local function transform_info(widget)
    local ok, r = pcall(function()
        local t = widget.RenderTransform
        return "translate=" .. vec(t.Translation) .. " scale=" .. vec(t.Scale)
    end)
    return ok and r or ""
end

local function visibility_info(widget)
    local ok, r = pcall(function() return "vis=" .. tostring(widget:GetVisibility()) end)
    return ok and r or ""
end

local function walk(widget, depth, out, seen)
    if depth > 40 or not widget:IsValid() then return end
    local ok, addr = pcall(function() return widget:GetAddress() end)
    if ok and addr then
        if seen[addr] then return end
        seen[addr] = true
    end
    out[#out + 1] = string.rep("  ", depth) .. name_of(widget) .. " " .. visibility_info(widget) .. " " .. slot_info(widget) .. " " .. transform_info(widget)
    -- a user widget's content hangs off its tree
    pcall(function()
        local tree = widget.WidgetTree
        if tree:IsValid() and tree.RootWidget:IsValid() then walk(tree.RootWidget, depth + 1, out, seen) end
    end)
    -- panels expose their children
    local okc, n = pcall(function() return widget:GetChildrenCount() end)
    if okc and type(n) == "number" then
        for i = 0, n - 1 do
            local okk, child = pcall(function() return widget:GetChildAt(i) end)
            if okk and child then walk(child, depth + 1, out, seen) end
        end
    end
end

-- Writes the dump to path. Returns the number of top-level widgets found, or nil and an error.
function U.dump(path, helpers)
    local out, seen, count = {}, {}, 0
    local pc = helpers.GetPlayerController()
    pcall(function()
        local lib = StaticFindObject("/Script/UMG.Default__WidgetLayoutLibrary")
        out[#out + 1] = "viewport=" .. vec(lib:GetViewportSize(pc)) .. " scale=" .. tostring(lib:GetViewportScale(pc))
    end)
    for _, widget in ipairs(FindAllOf("UserWidget") or {}) do
        local ok, on_screen = pcall(function() return widget:IsValid() and widget:IsInViewport() end)
        if ok and on_screen then
            count = count + 1
            out[#out + 1] = ""
            out[#out + 1] = "== " .. str(widget:GetFullName())
            walk(widget, 0, out, seen)
        end
    end
    local f = io.open(path, "wb")
    if not f then return nil, "cannot write " .. path end
    f:write(table.concat(out, "\n"), "\n")
    f:close()
    return count
end

return U
