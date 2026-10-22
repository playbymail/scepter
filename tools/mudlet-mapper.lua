-- Scepter mapper for Mudlet: draws the map from GMCP Room.Info (#190).
--
-- See "Mudlet's mapper" in docs/running.md.  Remove Mudlet's generic_mapper
-- package first (Toolbox -> Package Manager): it maps from the room text,
-- which this game doesn't mark up for it.  Then paste this file into a new
-- script (Scripts -> Add Item) and save.  Each Room.Info
-- adds or updates the room, links its exits, and centres the map on it.
-- Rooms that the current room's n/s/e/w/u/d exits lead to are added as
-- placeholders one step away; "out" and "go <thing>" exits are linked once
-- both ends have been visited.

scepterMap = scepterMap or {}

local offsets = {
  n = { 0, 1, 0 }, s = { 0, -1, 0 }, e = { 1, 0, 0 }, w = { -1, 0, 0 },
  u = { 0, 0, 1 }, d = { 0, 0, -1 },
}

-- environment -> custom env id and colour
local envs = {
  plain        = { 200, 150, 150, 150 },
  safe         = { 201,  80, 200,  80 },
  ["anti-magic"] = { 202, 170,  90, 220 },
  dark         = { 203,  70,  70,  70 },
  bar          = { 204, 210, 150,  50 },
  store        = { 205,  70, 150, 230 },
  pawnshop     = { 206,  60, 190, 190 },
  smith        = { 207, 210,  80,  60 },
  newsstand    = { 208, 230, 230, 100 },
}

local function areaId(name)
  if name == nil or name == "" then name = "Scepter" end
  return getAreaTable()[name] or addAreaName(name)
end

local function addAt(id, area, x, y, z)
  if roomExists(id) then return end
  addRoom(id)
  setRoomArea(id, area)
  setRoomCoordinates(id, x, y, z)
end

function scepterMap.onRoomInfo()
  local info = gmcp.Room and gmcp.Room.Info
  if not info or not info.num then return end
  local id, area = info.num, areaId(info.area)

  if not roomExists(id) then
    -- a room reached by "out", "go" or a teleport: put it beside the last one
    local x, y, z = 0, 0, 0
    if scepterMap.current and roomExists(scepterMap.current) then
      x, y, z = getRoomCoordinates(scepterMap.current)
      x, y = x + 2, y + 2
    end
    addAt(id, area, x, y, z)
  end

  setRoomName(id, info.name or "")
  local env = envs[info.environment] or envs.plain
  setCustomEnvColor(env[1], env[2], env[3], env[4], 255)
  setRoomEnv(id, env[1])

  local x, y, z = getRoomCoordinates(id)
  for dir, to in pairs(info.exits or {}) do
    local off = offsets[dir]
    if off then
      addAt(to, area, x + off[1], y + off[2], z + off[3])
    end
    if roomExists(to) then
      if off or dir == "out" then
        setExit(id, to, dir)
      else
        addSpecialExit(id, to, dir)
      end
    end
  end

  scepterMap.current = id
  centerview(id)
end

if scepterMap.handler then killAnonymousEventHandler(scepterMap.handler) end
scepterMap.handler = registerAnonymousEventHandler("gmcp.Room.Info", "scepterMap.onRoomInfo")

-- draw the room we are in now, if Room.Info has already arrived
if gmcp.Room and gmcp.Room.Info then scepterMap.onRoomInfo() end
