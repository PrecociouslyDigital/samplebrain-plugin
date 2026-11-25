-- Builds a project with one clip running Samplebrain as ARA take FX,
-- renders it, saves it and quits. Settings come from the environment.
local dir = os.getenv("SB_DIR")
local clip = os.getenv("SB_CLIP")
local rate = tonumber(os.getenv("SB_RATE"))
local name = os.getenv("SB_NAME")

local log = io.open(dir .. "/" .. name .. ".log", "w")
local function say(s) log:write(s .. "\n"); log:flush() end

reaper.GetSetProjectInfo(0, "PROJECT_SRATE", rate, true)
reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 1, true)
reaper.InsertTrackAtIndex(0, true)
local track = reaper.GetTrack(0, 0)
reaper.SetOnlyTrackSelected(track)
reaper.SetEditCurPos(0, false, false)
reaper.InsertMedia(clip, 0)
local item = reaper.GetTrackMediaItem(track, 0)
local take = reaper.GetActiveTake(item)
local fx = reaper.TakeFX_AddByName(take, "VST3:Samplebrain", 1)
-- SB_SECOND names a second clip, put on another track 4 s in.
if os.getenv("SB_SECOND") then
  reaper.InsertTrackAtIndex(1, true)
  local second = reaper.GetTrack(0, 1)
  reaper.SetOnlyTrackSelected(second)
  reaper.SetEditCurPos(4, false, false)
  reaper.InsertMedia(os.getenv("SB_SECOND"), 0)
  local take2 = reaper.GetActiveTake(reaper.GetTrackMediaItem(second, 0))
  reaper.TakeFX_AddByName(take2, "VST3:Samplebrain", 1)
end
local _, fxname = reaper.TakeFX_GetFXName(take, fx, "")
say("fx " .. fx .. " " .. fxname)
local ok, ara = reaper.TakeFX_GetNamedConfigParm(take, fx, "ARA")
say("ara " .. tostring(ok) .. " " .. tostring(ara))

reaper.GetSetProjectInfo_String(0, "RENDER_FILE", dir, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", name, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", rate, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 1, true)

local start = reaper.time_precise()
-- SB_DELAY seconds pass before rendering, to test both waiting and not.
local function render()
  if reaper.time_precise() - start < tonumber(os.getenv("SB_DELAY") or "0") then return reaper.defer(render) end
  local t = reaper.time_precise()
  reaper.Main_OnCommand(42230, 0) -- render with the most recent settings, auto-close
  say(string.format("render took %.1f s", reaper.time_precise() - t))
  local saved = dir .. "/" .. name .. ".rpp"
  reaper.Main_SaveProjectEx(0, saved, 0)
  say("saved")
  log:close()
  -- Reopen the saved copy so the project isn't dirty and quitting doesn't ask.
  reaper.Main_openProject("noprompt:" .. saved)
  reaper.Main_OnCommand(40004, 0) -- quit
end
render()
