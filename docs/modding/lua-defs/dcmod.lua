---@meta
-- Type definitions of the Lua environment of DriveClub 1.28 and of the dcmod library, for editors that read
-- EmmyLua annotations (Lua language server in VS Code, IntelliJ with EmmyLua). Put this folder in the workspace
-- library list of your editor. The game's own tables are generated from docs/modding/lua-api.md (what exists, not what
-- the arguments are: every function takes `...` and returns `any`); the dcmod part is written by hand.

---@class dcmod.CarWheel
---@field load number Vertical load in N.
---@field lateral number Lateral force in N.
---@field longitudinal number Longitudinal force in N.
---@field slip_ratio number
---@field slip_angle number

---@class dcmod.Car
---@field speed number m/s
---@field kmh number
---@field rpm number
---@field max_rpm number
---@field limiter number Rev limiter in rpm.
---@field gear integer 1 and up forward, -1 reverse.
---@field throttle number 0 to 1, from the controller.
---@field brake number 0 to 1, from the controller.
---@field steer number -1 to 1, from the controller.
---@field wheels dcmod.CarWheel[] Right front, left front, right rear, left rear.
---@field position {x: number, y: number, z: number} World position in metres, the middle of the four wheels.
---@field heading number Radians, atan2(x, z) of the way the car points.

---@alias dcmod.Color number[] {r, g, b, a} with values from 0 to 1

---@class dcmod.ui
---Text on the screen. Positions are fractions of the screen, size is the height in pixels, ttl seconds (1).
---@param id string
---@param x number
---@param y number
---@param text string
---@param size? integer
---@param color? dcmod.Color
---@param ttl? number
function dcmod.ui.text(id, x, y, text, size, color, ttl) end
---A filled rectangle.
---@param id string
---@param x number
---@param y number
---@param w number
---@param h number
---@param color? dcmod.Color
---@param ttl? number
function dcmod.ui.rect(id, x, y, w, h, color, ttl) end
---A picture of the mod (images/<name>.png, 8 bit RGB or RGBA PNG, at most 2048 by 2048) drawn over the game.
---@param name string
---@param tint table|nil {r, g, b, a} multiplied with the picture
function dcmod.ui.image(id, name, x, y, w, h, tint, ttl) end
---Removes an item.
---@param id string
function dcmod.ui.clear(id) end

---@class dcmod.postfx
---Switches the filter postfx/<name>.spv of a mod on; "off" or nil switches filters off.
---@param name string|nil
---@param slot integer|nil 0 to 2 on the finished picture, 3 to 5 on the HDR picture before the game's own post (0 by default); the slots run in order
function dcmod.postfx.use(name, slot) end
---The size a filter slot draws at, 0.05 to 1 of the picture (1 by default), for chains that downsample and blur.
---@param fraction number
---@param slot integer
function dcmod.postfx.scale(fraction, slot) end
---Sets one of the 16 numbers the filter reads (push constants values[i / 4][i % 4]).
---@param index integer 0 to 15
---@param value number
---@param slot integer|nil the slot of the filter (0 by default)
function dcmod.postfx.param(index, value, slot) end

---@class dcmod
---@field postfx dcmod.postfx
---@field version integer
---@field ui dcmod.ui
---@field ready_delay number Seconds after the start before dcmod.on_ready and dcmod.on callbacks run (20).
---@field settings table<string, table<string, any>> The settings of every mod.
dcmod = {}
---Writes a line to the game log ("Mod: ...").
---@param ... any
function dcmod.log(...) end
---Runs fn once, in real time.
---@param seconds number
---@param fn function
function dcmod.after(seconds, fn) end
---Runs fn again and again.
---@param seconds number
---@param fn function
function dcmod.every(seconds, fn) end
---Runs fn once the game is up.
---@param fn function
function dcmod.on_ready(fn) end
---Calls fn(...) whenever the game sends the event (NewLap, GUIPushPage, GUIPopPage ...).
---@param event string
---@param fn function
function dcmod.on(event, fn) end
---The value of a setting the mod offers.
---@param mod string
---@param name string
---@param fallback any
---@return any
function dcmod.setting(mod, name, fallback) end
---The live state of the car the player drives, or nil when no car is driven.
---@return dcmod.Car|nil
function dcmod.car() end
---@class dcmod.Camera
---@field x number
---@field y number
---@field z number
---@field right number[] unit vector, x y z
---@field up number[] unit vector, x y z
---@field forward number[] unit vector, x y z
---The game camera of the last frame (world metres, y up), or nil when no frame was drawn for a second.
---@return dcmod.Camera|nil
function dcmod.camera() end
---The hour of the day of the scene (0 to 24) and the speed of the clock, or nil when no race is loaded.
---@return number|nil hour
---@return number|nil compression
function dcmod.time() end
---Moves the clock of the scene to an hour (0 to 24).
---@param hour number
function dcmod.set_time(hour) end
---@class dcmod.Input
---@field mx number 0 to 1 from the left of the game window
---@field my number 0 to 1 from the top of the game window
---@field left boolean
---@field right boolean
---@field middle boolean
---@field keys table<integer, boolean> Windows virtual key codes of the keys held
---Keys and mouse while the game window has the focus, nil otherwise.
---@return dcmod.Input|nil
function dcmod.input() end
---True while a key is held ("F7", "A", "SPACE", "UP", ... or a virtual key number).
---@param name string|integer
---@return boolean
function dcmod.key(name) end
---True hands the player's car to the AI driver, false gives it back, nil asks who drives (returns true when the AI does).
---@param on boolean|nil
---@return boolean|nil
function dcmod.ai_drive(on) end
---@class dcmod.store
dcmod.store = {}
---A value kept between runs: the type of default (number, boolean, text) decides the type that comes back.
---@param name string
---@param default any
---@return any
function dcmod.store.get(name, default) end
---Keeps a number, boolean or text (up to 255 characters).
---@param name string
---@param value any
function dcmod.store.set(name, value) end
---Forgets a value.
---@param name string
function dcmod.store.remove(name) end
---@class dcmod.RaceCar
---@field name string vehicle name
---@field progress number 0 to 1 round the lap
---@field player boolean
---@field actor userdata the game's actor
---@field laps integer laps completed
---@field distance number distance along the race, a fraction of a lap
---@field finish integer finish position, 0 until finished
---@field car_collisions integer
---@field world_collisions integer
---@field wheelie_score number
---The cars of the race (empty when no race is loaded).
---@return dcmod.RaceCar[]
function dcmod.cars() end
---@class dcmod.Gui
---@field text fun(self: dcmod.Gui, text: string)
---@field button fun(self: dcmod.Gui, label: string): boolean
---@field checkbox fun(self: dcmod.Gui, label: string, value: boolean): boolean
---@field slider fun(self: dcmod.Gui, label: string, value: number, min: number, max: number): number
---@class dcmod.GuiOptions
---@field key string|nil key that shows and hides the panel (F8)
---@field x number|nil 0 to 1, where the panel starts
---@field y number|nil
---@field width number|nil 0 to 1 of the screen width
---@field shown boolean|nil true: visible from the start
---@class dcmod.gui
dcmod.gui = {}
---A panel over the game that fn(g) describes about 15 times a second while it is shown.
---@param title string
---@param fn fun(g: dcmod.Gui)
---@param options dcmod.GuiOptions|nil
function dcmod.gui.app(title, fn, options) end
---@class dcmod.RaceWeather
---@field rain string|nil Heavy, Mid, Light, VeryLight, Wet, Damp, DryToWet, WetToDry or a full preset name
---@field weather string|nil Clear, Cloudy, Overcast or Stormy
---The weather of the races that load from now on (an empty table gives the choice back to the game).
---@param settings dcmod.RaceWeather
function dcmod.race_weather(settings) end

-- Functions of the game that are not part of Lua 5.1
---@type fun(...): any
AbstractClass = nil
---@type fun(...): any
Class = nil
---@type fun(...): any
ClassClearCaches = nil
---@type fun(...): any
ClassMock = nil
---@type fun(...): any
ClearTimeout = nil
---@type fun(...): any
GetTimeout = nil
---@type fun(...): any
IsModalShown = nil
---@type fun(...): any
IsOSActive = nil
---@type fun(...): any
LogLuaMemory = nil
---@type fun(...): any
ReloadClass = nil
---@type fun(...): any
ReloadTemplate = nil
---@type fun(...): any
SetRealTimeout = nil
---@type fun(...): any
SetTimeout = nil
---@type fun(...): any
errortostring = nil
---@type fun(...): any
gcinfo = nil
---@type fun(...): any
kernelTime = nil
---@type fun(...): any
loadfileraw = nil
---@type fun(...): any
numbertostring = nil
---@type fun(...): any
toboolean = nil

---@class ActivityStream
---@field Dump fun(...): any
---@field Entry fun(...): any
---@field FormatAsJsArray fun(...): any
---@field FormatAsJsMap fun(...): any
---@field GetDumpEnabled fun(...): any
---@field GetUrl fun(...): any
---@field New fun(...): any
---@field PostProcessResults fun(...): any
---@field SetDumpEnabled fun(...): any
---@field __index table
---@field _dependencies table
---@field dumpEnabled boolean
---@field name string
---@field stream_entry_action table
ActivityStream = {}

---@class Actor
---@field AllActors fun(...): any
---@field AllActorsWithProperty fun(...): any
---@field CollectionSize fun(...): any
---@field CreateProperty fun(...): any
---@field GetActorById fun(...): any
---@field GetActorByName fun(...): any
---@field GetActorFromColIdx fun(...): any
---@field GetActorId fun(...): any
---@field GetAnimationName fun(...): any
---@field GetLocalPlayer fun(...): any
---@field GetPlayer fun(...): any
---@field GetSqDistanceBetweenActors fun(...): any
---@field GetVehicleName fun(...): any
---@field GetViewedPlayer fun(...): any
---@field HasProperty fun(...): any
---@field PropertyEqualTo fun(...): any
---@field PropertyLessThan fun(...): any
---@field PropertyMoreThan fun(...): any
---@field PropertyNotEqualTo fun(...): any
---@field RemoveActor fun(...): any
---@field SpawnActor fun(...): any
Actor = {}

---@class AiLiveries
---@field Liveries table
---@field LiveryForAiId fun(...): any
---@field New fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
AiLiveries = {}

---@class Async
---@field Graph fun(...): any
---@field New fun(...): any
---@field Parallel fun(...): any
---@field Series fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
Async = {}

---@class AsyncLoad
---@field IsLoadingInProgress fun(...): any
AsyncLoad = {}

---@class AutoTestApi
---@field BroadcastVehicleChoice fun(...): any
---@field GetCompleteSeasonData fun(...): any
---@field GetMatchSlotsData fun(...): any
---@field GetPermittedVehiclesList fun(...): any
---@field GoToLobbyGUI fun(...): any
---@field RegisterForEvent fun(...): any
---@field RequestMatchSlotsData fun(...): any
AutoTestApi = {}

---@class Automation
---@field Go fun(...): any
---@field GoReplay fun(...): any
Automation = {}

---@class BestScore
---@field UpdateScoresAndGhosts fun(...): any
BestScore = {}

---@class BootOptions
---@field Get fun(...): any
---@field Set fun(...): any
BootOptions = {}

---@class BuildOptions
---@field Set fun(...): any
BuildOptions = {}

---@class CameraSequence
---@field AdvanceToNext fun(...): any
---@field IsRunning fun(...): any
---@field New fun(...): any
---@field ResetTimeoutOnCurrentCamera fun(...): any
---@field Start fun(...): any
---@field StartAutoSequence fun(...): any
---@field Stop fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
CameraSequence = {}

---@class ChallengeInfo
---@field GetChallengeTargetEgsId fun(...): any
---@field GetChallengeTargetName fun(...): any
---@field GetChallengeTargetScore fun(...): any
---@field GetMyBestChallengeScore fun(...): any
ChallengeInfo = {}

---@class ChallengeResponder
---@field AcceptChallenge fun(...): any
---@field New fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
ChallengeResponder = {}

---@class CinematicsOptions
---@field Set fun(...): any
CinematicsOptions = {}

---@class Config
---@field GetDriverCount fun(...): any
---@field GetHumanDriverCount fun(...): any
---@field GetTotalHumanCount fun(...): any
Config = {}

---@class CountdownHelper
---@field Init fun(...): any
---@field ManualUpdate fun(...): any
---@field New fun(...): any
---@field ResetCountdown fun(...): any
---@field Shutdown fun(...): any
---@field Update fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
CountdownHelper = {}

---@class CrazySpeedConstants
---@field New fun(...): any
---@field SetUp fun(...): any
---@field SetupParams table
---@field __index table
---@field _dependencies table
---@field name string
CrazySpeedConstants = {}

---@class DebugOptions
---@field Set fun(...): any
DebugOptions = {}

---@class Diorama
---@field GetRecentVehicle fun(...): any
---@field SetRecentVehicle fun(...): any
Diorama = {}

---@class Drift
---@field New fun(...): any
---@field SetUpDrift fun(...): any
---@field SetupParams table
---@field __index table
---@field _dependencies table
---@field name string
Drift = {}

---@class DriverActions
---@field AddDriverAction fun(...): any
---@field DeferredActionCreation fun(...): any
---@field FinaliseDriverActionRewards fun(...): any
---@field GiveFinishPositionRewards fun(...): any
---@field New fun(...): any
---@field OnSplit fun(...): any
---@field Setup fun(...): any
---@field Stop fun(...): any
---@field Update fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
DriverActions = {}

---@class E3Options
---@field Get fun(...): any
---@field Set fun(...): any
E3Options = {}

---@class EGSConstants
---@field AccountSessionStatus table
---@field ActivityStreamSubjectType table
---@field ActivityStreamVerb table
---@field AggregateIncludes table
---@field AggregatedProfileOption table
---@field AssetAvailability table
---@field AssetType table
---@field AwardType table
---@field BonusValueType table
---@field ChallengeOrdering table
---@field ChallengePrivacy table
---@field ChallengeStatus table
---@field ChallengeTypes table
---@field Club table
---@field ClubAttributeType table
---@field ClubInviteStatus table
---@field ClubMemberStatus table
---@field ClubMemberType table
---@field ClubOrderBy table
---@field ClubStatus table
---@field ClubType table
---@field ConfigDataType table
---@field Country table
---@field DriverActionType table
---@field EntitlementType table
---@field ErrorCodes table
---@field EventListName table
---@field EventListType table
---@field FinishPositionType table
---@field FriendType table
---@field GamePlayerFinishType table
---@field GamePlayerStatus table
---@field GameStatus table
---@field LobbyPlayerStatus table
---@field LobbyStatus table
---@field LobbyType table
---@field MissionLamsID table
---@field New fun(...): any
---@field Objective table
---@field ObjectiveType table
---@field ProfileOwnerType table
---@field Region table
---@field RewardDisplayValue table
---@field RewardType table
---@field SeasonType table
---@field SocialGoalType table
---@field StreamEntryActions table
---@field __index table
---@field _dependencies table
---@field name string
EGSConstants = {}

---@class ErrorDialog
---@field Close fun(...): any
---@field DevModal fun(...): any
---@field Show fun(...): any
ErrorDialog = {}

---@class Event
---@field Close fun(...): any
---@field Listen fun(...): any
---@field SendEvent fun(...): any
Event = {}

---@class FacebookDialog
---@field Close fun(...): any
---@field Show fun(...): any
FacebookDialog = {}

---@class Fame
---@field AddFame fun(...): any
---@field ApplyBonusAwards fun(...): any
---@field ClearPot fun(...): any
---@field CommitPot fun(...): any
---@field MergePots fun(...): any
Fame = {}

---@class FluidOptions
---@field Set fun(...): any
FluidOptions = {}

---@class GUIConstants
---@field Club table
---@field Leaderboard table
---@field Location table
---@field New fun(...): any
---@field User table
---@field __index table
---@field _dependencies table
---@field gameTips table
---@field name string
GUIConstants = {}

---@class GUIOptions
---@field Get fun(...): any
---@field Set fun(...): any
GUIOptions = {}

---@class GameComponents
---@field GatherComponents fun(...): any
---@field New fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
GameComponents = {}

---@class GameConfig
---@field AddRecordingComponent fun(...): any
---@field AddResource fun(...): any
---@field Clear fun(...): any
---@field ClearUsedTrackPassContext fun(...): any
---@field Deprecated_SetMissionStm fun(...): any
---@field Deprecated_SetNrLaps fun(...): any
---@field Deprecated_SetTimeOfDay fun(...): any
---@field GetAllowRetry fun(...): any
---@field GetCanChallenge fun(...): any
---@field GetChallengeId fun(...): any
---@field GetChallengeOriginalEventListID fun(...): any
---@field GetChallengePlayerGhostId fun(...): any
---@field GetChallengePlayerScore fun(...): any
---@field GetChallengeTargetAccountId fun(...): any
---@field GetChallengeTargetAccountName fun(...): any
---@field GetChallengeTargetDisplayName fun(...): any
---@field GetChallengeTargetGhostId fun(...): any
---@field GetChallengeTargetScore fun(...): any
---@field GetChallengeType fun(...): any
---@field GetCustomRetryUrl fun(...): any
---@field GetDisplayProgressScreens fun(...): any
---@field GetEgsGameID fun(...): any
---@field GetEgsGameSeriesID fun(...): any
---@field GetEnvSeed fun(...): any
---@field GetEnvironment fun(...): any
---@field GetEventID fun(...): any
---@field GetEventLams fun(...): any
---@field GetEventListID fun(...): any
---@field GetEventListName fun(...): any
---@field GetEventListType fun(...): any
---@field GetEventSeriesRound fun(...): any
---@field GetEventSeriesRoundTotal fun(...): any
---@field GetEventType fun(...): any
---@field GetFaceOffs fun(...): any
---@field GetFinishFameForPosition fun(...): any
---@field GetGameArea fun(...): any
---@field GetGameComponents fun(...): any
---@field GetGarageVehicle fun(...): any
---@field GetGarageWorld fun(...): any
---@field GetGlobalAIDifficulty fun(...): any
---@field GetHasObjectives fun(...): any
---@field GetIsDriveClubLiveEvent fun(...): any
---@field GetIsEventSeries fun(...): any
---@field GetIsFreeplayEvent fun(...): any
---@field GetIsLastEventInSeries fun(...): any
---@field GetIsTeamMode fun(...): any
---@field GetLiveTimers fun(...): any
---@field GetMatchingTimers fun(...): any
---@field GetMission fun(...): any
---@field GetMissionParams fun(...): any
---@field GetMissionStm fun(...): any
---@field GetNextSlotID fun(...): any
---@field GetNrCompetitors fun(...): any
---@field GetNrLaps fun(...): any
---@field GetPostRacePage fun(...): any
---@field GetPrecipitationType fun(...): any
---@field GetPresetTargetScore fun(...): any
---@field GetReplayFile fun(...): any
---@field GetResources fun(...): any
---@field GetSeasonID fun(...): any
---@field GetSingleplayerType fun(...): any
---@field GetSprintVariant fun(...): any
---@field GetTarget fun(...): any
---@field GetTargetAvatar fun(...): any
---@field GetTargetName fun(...): any
---@field GetTemplateID fun(...): any
---@field GetTimeMultiplier fun(...): any
---@field GetTimeOfDay fun(...): any
---@field GetTimeTrialDuration fun(...): any
---@field GetTrack fun(...): any
---@field GetTrackID fun(...): any
---@field GetTrackInfrastructureLevel fun(...): any
---@field GetUsedTrackPassContext fun(...): any
---@field GetVehicle fun(...): any
---@field GetWeatherType fun(...): any
---@field IsConfigDataInCPP fun(...): any
---@field IsInRace fun(...): any
---@field IsSingleplayer fun(...): any
---@field LoadedFromReplayFile fun(...): any
---@field New fun(...): any
---@field SetConfigDataInCPP fun(...): any
---@field SetResources fun(...): any
---@field __index table
---@field _dependencies table
---@field data table
---@field dataInCPP boolean
---@field kAllFaceOffs number
---@field kNoFaceOffs number
---@field kTourFaceOffsOnly number
---@field name string
GameConfig = {}

---@class GameProduct
---@field GetSelectedGameProduct fun(...): any
GameProduct = {}

---@class GameSession
---@field Create fun(...): any
---@field FinishSession fun(...): any
---@field GameJoined fun(...): any
---@field PostDriftScore fun(...): any
---@field PostFame fun(...): any
---@field PostLapTime fun(...): any
---@field PostResults fun(...): any
---@field UnblockAllSessions fun(...): any
GameSession = {}

---@class GameState
---@field CurrentFrame fun(...): any
---@field CurrentGameTime fun(...): any
---@field EnableComponents fun(...): any
---@field EndGame fun(...): any
---@field GetCurrent fun(...): any
---@field GetFrameRateStats fun(...): any
---@field GetGameConfig fun(...): any
---@field GetGameProduct fun(...): any
---@field GetTracks fun(...): any
---@field GetVehicles fun(...): any
---@field LoadMode fun(...): any
---@field Quit fun(...): any
---@field ResetFrameRateStats fun(...): any
---@field RestartGame fun(...): any
---@field SetNoDataLayers fun(...): any
---@field SetProcessExitCode fun(...): any
---@field StartGame fun(...): any
GameState = {}

---@class GamescomOptions
---@field Get fun(...): any
---@field Set fun(...): any
GamescomOptions = {}

---@class Ghosting
---@field SetAllVehicleGhosting fun(...): any
---@field SetAllVehicleNoGhosting fun(...): any
Ghosting = {}

---@class Ghosts
---@field FinishRecording fun(...): any
---@field FinishRecordingAndStartNew fun(...): any
---@field StartRecording fun(...): any
Ghosts = {}

---@class GlobalObjective
---@field HaveDisplayableObjectives fun(...): any
GlobalObjective = {}

---@class HUD
---@field SetCountdownTimer fun(...): any
HUD = {}

---@class Json
---@field ToLua fun(...): any
Json = {}

---@class JsonMarshaller
---@field New fun(...): any
---@field ToJson fun(...): any
---@field ToLua fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
JsonMarshaller = {}

---@class LAMS
---@field Translate fun(...): any
---@field TranslateArray fun(...): any
LAMS = {}

---@class Livery
---@field CompactLivery fun(...): any
---@field ExpandLivery fun(...): any
---@field GetLivery fun(...): any
Livery = {}

---@class LiveryCompExpand
---@field CompactLiveryDescription fun(...): any
---@field ExpandLiveryDescription fun(...): any
---@field New fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
LiveryCompExpand = {}

---@class LiveryManager
---@field LiveriesReady fun(...): any
LiveryManager = {}

---@class LuaDelegate
---@field GetErrorHandler fun(...): any
---@field SetErrorHandler fun(...): any
LuaDelegate = {}

---@class LuaGameEditor
---@field LoadGameEditor fun(...): any
---@field New fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
LuaGameEditor = {}

---@class MainMenu
---@field SetupGameConfig fun(...): any
---@field StartFirstGameExperience fun(...): any
MainMenu = {}

---@class MessageOptions
---@field Get fun(...): any
---@field Set fun(...): any
MessageOptions = {}

---@class MessageQueue
---@field Connect fun(...): any
---@field Post fun(...): any
---@field SendGameStartTelemetry fun(...): any
---@field SetMessageDisabled fun(...): any
---@field SetMessageEnabled fun(...): any
---@field Update fun(...): any
MessageQueue = {}

---@class OSDController
---@field Common_AddOverDrive fun(...): any
---@field Common_ClampedTimeToString fun(...): any
---@field Common_ClearTimersOnExit fun(...): any
---@field Common_ConfigureForCamera fun(...): any
---@field Common_ConfigureKERSAndDRS fun(...): any
---@field Common_ConfigureSpeedo fun(...): any
---@field Common_Depause fun(...): any
---@field Common_DisplayDiffTimeToActorAhead fun(...): any
---@field Common_DisplayDiffTimeToActorBehind fun(...): any
---@field Common_DisplayDiffTimeToTargetAhead fun(...): any
---@field Common_DisplayDiffTimeToTargetBehind fun(...): any
---@field Common_Exit fun(...): any
---@field Common_GetSplitColour fun(...): any
---@field Common_HideCatastrophicCrash fun(...): any
---@field Common_Pause fun(...): any
---@field Common_PercentageComplete fun(...): any
---@field Common_Reset fun(...): any
---@field Common_SelectTeam fun(...): any
---@field Common_SetBestInfo fun(...): any
---@field Common_SetBestInfoScore fun(...): any
---@field Common_SetOSDUnits fun(...): any
---@field Common_SetSpeedoVisibility fun(...): any
---@field Common_SetTargetInfo fun(...): any
---@field Common_SetTargetInfoScore fun(...): any
---@field Common_SetupTargetPanel fun(...): any
---@field Common_ShowCatastrophicCrash fun(...): any
---@field Common_ShowOnlyOverdrives fun(...): any
---@field Common_ShowResetToTrack fun(...): any
---@field Common_ShowWrongWay fun(...): any
---@field Common_StartPhotoMode fun(...): any
---@field Common_StopPhotoMode fun(...): any
---@field Common_TrackType fun(...): any
---@field Common_UpdateSpeedometer fun(...): any
---@field Common_UpdateTickerAction fun(...): any
---@field Drift_DisableScore fun(...): any
---@field Drift_DisplayDiffScore fun(...): any
---@field Drift_DriftZone fun(...): any
---@field Drift_ExitState fun(...): any
---@field Drift_Init fun(...): any
---@field Drift_Load fun(...): any
---@field Drift_ModeShowInfo fun(...): any
---@field Drift_SetScore fun(...): any
---@field Drift_SpeedZone fun(...): any
---@field Drift_UpdateDrift fun(...): any
---@field EnqueueMessage fun(...): any
---@field EnqueueTranslatedMessage fun(...): any
---@field Initialise fun(...): any
---@field MultiTimeTrial_Depause fun(...): any
---@field MultiTimeTrial_Pause fun(...): any
---@field New fun(...): any
---@field Race_Init fun(...): any
---@field Race_Load fun(...): any
---@field Race_SetLapInfo fun(...): any
---@field Race_SetPositionInfo fun(...): any
---@field Race_UpdateRaceTime fun(...): any
---@field Race_UpdateTracker fun(...): any
---@field ResetTimer fun(...): any
---@field TernaryOperator fun(...): any
---@field TimeTrial_ApplySplitColour fun(...): any
---@field TimeTrial_ClearHoldSplitTimers fun(...): any
---@field TimeTrial_HoldSplitTimers fun(...): any
---@field TimeTrial_Init fun(...): any
---@field TimeTrial_Load fun(...): any
---@field TimeTrial_ResetSplitTimers fun(...): any
---@field TimeTrial_SetCurrentLapTime fun(...): any
---@field TimeTrial_ShowEndSessionPrompt fun(...): any
---@field TimeTrial_SplitTime fun(...): any
---@field TimeTrial_SplitTimeNoAnim fun(...): any
---@field TimeTrial_UpdateDiffTime fun(...): any
---@field TimeTrial_UpdateLapDiffColour fun(...): any
---@field TimeTrial_UpdateSplitColour fun(...): any
---@field TopSpeed_Init fun(...): any
---@field TopSpeed_Load fun(...): any
---@field TopSpeed_ResetTotalScore fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
OSDController = {}

---@class OSDPage
---@field Hide fun(...): any
---@field Send fun(...): any
---@field SetLayout fun(...): any
---@field Show fun(...): any
OSDPage = {}

---@class OSDTime
---@field GetOSDTimer fun(...): any
OSDTime = {}

---@class OverDrive
---@field Add fun(...): any
---@field Finalise fun(...): any
OverDrive = {}

---@class PartyInfo
---@field GetNumberInParty fun(...): any
---@field JoinByName fun(...): any
PartyInfo = {}

---@class PauseMultiTimeTrialPage
---@field SetStartSession fun(...): any
PauseMultiTimeTrialPage = {}

---@class PausePage
---@field AllowEndSession fun(...): any
---@field AllowRestartSeries fun(...): any
PausePage = {}

---@class Platform
---@field CheckChatRestriction fun(...): any
---@field EgsDisconnect fun(...): any
---@field EgsReconnect fun(...): any
---@field GetE3Name fun(...): any
---@field GetEgsConnected fun(...): any
---@field GetEgsConnectionStatus fun(...): any
---@field GetIsAdapterUp fun(...): any
---@field GetIsPsnCheckInProgress fun(...): any
---@field GetPlatformInitialised fun(...): any
---@field GetPrimaryEgsAccountId fun(...): any
---@field GetPrimaryOnlineName fun(...): any
---@field GetPrimaryUserName fun(...): any
---@field GetPsnAvailable fun(...): any
---@field GetPsnPlusAvailable fun(...): any
---@field GetTimeInMs fun(...): any
---@field GetUserAvailabilityError fun(...): any
---@field SetEgsConnected fun(...): any
---@field UpdateServiceChecks fun(...): any
---@field UsingPlusFeature fun(...): any
Platform = {}

---@class PlayerProfile
---@field GetAccountId fun(...): any
---@field GetDisplayName fun(...): any
---@field GetDriverAppearence fun(...): any
---@field GetValue fun(...): any
---@field IsFGEComplete fun(...): any
---@field SetFGEComplete fun(...): any
---@field SetValue fun(...): any
PlayerProfile = {}

---@class Plugins
---@field LoadPlugin fun(...): any
Plugins = {}

---@class PrecipitationDriver
---@field SetInitialPrecipitationState fun(...): any
---@field SetPrecipitationFromGameConfig fun(...): any
---@field StartDrivingPrecipitation fun(...): any
PrecipitationDriver = {}

---@class ProductRegion
---@field GetRegion fun(...): any
ProductRegion = {}

---@class Profiling
---@field AddMemoryWarningCallback fun(...): any
---@field AddTimingWarningCallback fun(...): any
---@field ClearWarningCallbacks fun(...): any
---@field New fun(...): any
---@field Off fun(...): any
---@field On fun(...): any
---@field Profiler fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
Profiling = {}

---@class RenderOptions
---@field Set fun(...): any
RenderOptions = {}

---@class Rewards
---@field AwardDriverActionXP fun(...): any
---@field AwardFinishPositionXPandCash fun(...): any
---@field AwardObjectiveXp fun(...): any
---@field AwardSocialGoalXP fun(...): any
---@field Clear fun(...): any
---@field FinaliseQueuedRewards fun(...): any
---@field IsQueueFinished fun(...): any
---@field New fun(...): any
---@field Setup fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
Rewards = {}

---@class RoadShearOptions
---@field Set fun(...): any
RoadShearOptions = {}

---@class Roads
---@field GetPoslenRelativeInMetres fun(...): any
---@field IsPointToPoint fun(...): any
---@field IsPointToPointRoadLayout fun(...): any
---@field RegionToPoslen fun(...): any
Roads = {}

---@class Rooms
---@field GetLocation fun(...): any
---@field GetNetName fun(...): any
---@field GetRoomEnabled fun(...): any
Rooms = {}

---@class STMScript
---@field LoadScript fun(...): any
---@field TriggerScript fun(...): any
STMScript = {}

---@class Schema
---@field Boolean fun(...): any
---@field Check fun(...): any
---@field Coerce fun(...): any
---@field In fun(...): any
---@field Integer fun(...): any
---@field New fun(...): any
---@field Number fun(...): any
---@field Opt fun(...): any
---@field OptBoolean fun(...): any
---@field OptIn fun(...): any
---@field OptInteger fun(...): any
---@field OptNumber fun(...): any
---@field OptOr fun(...): any
---@field OptRange fun(...): any
---@field OptString fun(...): any
---@field OptStringContains fun(...): any
---@field OptStringPattern fun(...): any
---@field Or fun(...): any
---@field Range fun(...): any
---@field String fun(...): any
---@field StringContains fun(...): any
---@field StringPattern fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
Schema = {}

---@class Selector
---@field ClearAndPushPage fun(...): any
---@field ClearStack fun(...): any
---@field GetLoadingPage fun(...): any
---@field HandleGameCustomData fun(...): any
---@field InsertPage fun(...): any
---@field PopPage fun(...): any
---@field PopToPage fun(...): any
---@field PushPage fun(...): any
---@field SetActive fun(...): any
---@field ShowTutorial fun(...): any
Selector = {}

---@class Settings
---@field AreEGSFaceOffsVisible fun(...): any
---@field Audio fun(...): any
---@field Display fun(...): any
---@field DisplaySet fun(...): any
Settings = {}

---@class SocialGoalSourceEGS
---@field AllEGSGoalsAdded fun(...): any
---@field AllEGSGoalsUpdated fun(...): any
---@field CreateEGSSocialGoal fun(...): any
---@field DebugLogStatus fun(...): any
---@field EGSDisconnected fun(...): any
---@field EGSGoalUpdateFinished fun(...): any
---@field GetGoalsFromEGS fun(...): any
---@field GiveDevScores fun(...): any
---@field GiveFirstInWorldTarget fun(...): any
---@field GiveGoalDevTarget fun(...): any
---@field GiveGoalTargetFromEGS fun(...): any
---@field GiveInvalidScores fun(...): any
---@field HideEGSGoals fun(...): any
---@field Initialise fun(...): any
---@field IsNextUpdateDelayed fun(...): any
---@field IsTargetSane fun(...): any
---@field New fun(...): any
---@field NewEGSGoalTargetsFailed fun(...): any
---@field NoGoalUpdates fun(...): any
---@field OnGoalAttempted fun(...): any
---@field RemoveAllEGSGoals fun(...): any
---@field SetGoalUpdated fun(...): any
---@field SetGoalUpdating fun(...): any
---@field ShowEGSGoals fun(...): any
---@field Update fun(...): any
---@field UpdateGoalScore fun(...): any
---@field UpdateGoalScores fun(...): any
---@field UpdateGoalTargetFromEGS fun(...): any
---@field WipeSocialGoals fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
SocialGoalSourceEGS = {}

---@class SocialGoalSourceTour
---@field CreateGoalInternal fun(...): any
---@field CreateTourGoal fun(...): any
---@field Initialise fun(...): any
---@field New fun(...): any
---@field OnPlayerChangedLap fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
SocialGoalSourceTour = {}

---@class SocialGoals
---@field AddActor fun(...): any
---@field AddSocialGoal fun(...): any
---@field AwardAverageSpeedXP fun(...): any
---@field AwardCorneringXP fun(...): any
---@field AwardMaxSpeedXP fun(...): any
---@field AwardNothing fun(...): any
---@field AwardSingleDriftXP fun(...): any
---@field CheckOverlap fun(...): any
---@field CheckOverlappingGoals fun(...): any
---@field DeactivateGoal fun(...): any
---@field DebugPrintGoal fun(...): any
---@field EvaluateGoal fun(...): any
---@field FindAndSetNextSocialGoal fun(...): any
---@field FindNextGoalFromPoslen fun(...): any
---@field GetActorData fun(...): any
---@field GetBestAttemptScore fun(...): any
---@field GetCurrentSocialGoal fun(...): any
---@field GetFailMessage fun(...): any
---@field GetInfoPanelDataForGoal fun(...): any
---@field GetPreviousGoalScore fun(...): any
---@field GetRunningTotalScore fun(...): any
---@field GetShortestWrappedDistance fun(...): any
---@field GetSuccessMessage fun(...): any
---@field GiveGoalInvalidTarget fun(...): any
---@field GiveGoalsToActor fun(...): any
---@field HideGoal fun(...): any
---@field HigherThanTarget fun(...): any
---@field Initialise fun(...): any
---@field InvalidGoalAttempt fun(...): any
---@field LeaveGoal fun(...): any
---@field LiveRestart fun(...): any
---@field New fun(...): any
---@field OnStatsUpdated fun(...): any
---@field SendSocialGoalTelemetry fun(...): any
---@field SetCurrentSocialGoalProgress fun(...): any
---@field SetGUIUnits fun(...): any
---@field SetGoalOverlapped fun(...): any
---@field SetGoalRacingLineVisibility fun(...): any
---@field SetNextSocialGoal fun(...): any
---@field SetRacingLinesShown fun(...): any
---@field ShowGoal fun(...): any
---@field Shutdown fun(...): any
---@field SortGoalsByZone fun(...): any
---@field StartGoal fun(...): any
---@field TellGameAboutGoal fun(...): any
---@field TellGameAboutGoals fun(...): any
---@field TranslateGoalTypes fun(...): any
---@field Update fun(...): any
---@field UpdateActorPositionInGoals fun(...): any
---@field UpdateDeferredShowingGoals fun(...): any
---@field WipeSocialGoals fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
SocialGoals = {}

---@class StatCollector
---@field GetStats fun(...): any
---@field PrintStats fun(...): any
---@field SetEnabled fun(...): any
---@field SetStatFloat fun(...): any
---@field SetStatInt fun(...): any
StatCollector = {}

---@class Stats
---@field TrackStats fun(...): any
Stats = {}

---@class StringUtils
---@field IsEmptyString fun(...): any
---@field IsInvalidString fun(...): any
---@field IsNotEmptyString fun(...): any
---@field IsValidString fun(...): any
---@field New fun(...): any
---@field SplitParagraph fun(...): any
---@field StartsWith fun(...): any
---@field __index table
---@field _dependencies table
---@field join fun(...): any
---@field name string
---@field split fun(...): any
---@field trim fun(...): any
StringUtils = {}

---@class SystemDialog
---@field Close fun(...): any
---@field Show fun(...): any
SystemDialog = {}

---@class TableUtils
---@field DeepCopyTable fun(...): any
---@field GetValueOrAddEmptyTable fun(...): any
---@field IndexOf fun(...): any
---@field Invert fun(...): any
---@field IsEmpty fun(...): any
---@field Keys fun(...): any
---@field MergeArrays fun(...): any
---@field MergeTables fun(...): any
---@field New fun(...): any
---@field NumEntries fun(...): any
---@field Print fun(...): any
---@field ProcessTableInternal fun(...): any
---@field ShallowCopyTable fun(...): any
---@field TableToArrayInternal fun(...): any
---@field ToString fun(...): any
---@field ToStringCompact fun(...): any
---@field Values fun(...): any
---@field __index table
---@field _dependencies table
---@field allowPrintTable boolean
---@field name string
TableUtils = {}

---@class TeamModeHelper
---@field GetTeamFromPlayerName fun(...): any
---@field New fun(...): any
---@field SetTeamsToActors fun(...): any
---@field SetUpTeamsFromResources fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
TeamModeHelper = {}

---@class Telemetry
---@field New fun(...): any
---@field PostStat fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
Telemetry = {}

---@class Time
---@field ExtractMilliseconds fun(...): any
---@field ExtractMinutes fun(...): any
---@field ExtractSeconds fun(...): any
---@field New fun(...): any
---@field SecondsToString fun(...): any
---@field TimeOfDayFloatTo24HTime fun(...): any
---@field ToString fun(...): any
---@field ToTimeOfDayString fun(...): any
---@field TranslateTimeStrings fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
Time = {}

---@class TimeManager
---@field SetGameSpeed fun(...): any
TimeManager = {}

---@class Timeline
---@field Load fun(...): any
---@field Start fun(...): any
---@field Stop fun(...): any
Timeline = {}

---@class Tour
---@field GetEventRoundTotal fun(...): any
---@field GetEventTotal fun(...): any
---@field GetNextEventWithNoStars fun(...): any
---@field GetNextEventWithoutMaxStars fun(...): any
---@field GetNumberOfStarsAvailable fun(...): any
---@field GetTotalStarsAvailable fun(...): any
---@field GetTotalStarsWon fun(...): any
---@field Go fun(...): any
---@field SetEventStarsCompleted fun(...): any
Tour = {}

---@class Tracks
---@field GetDetails fun(...): any
Tracks = {}

---@class Trophies
---@field UpdateDeferredTrophies fun(...): any
---@field UpdatePostRaceTrophies fun(...): any
Trophies = {}

---@class UIOptions
---@field Get fun(...): any
---@field Set fun(...): any
UIOptions = {}

---@class UriUtils
---@field BuildParams fun(...): any
---@field DecodeURI fun(...): any
---@field DecodeURIComponent fun(...): any
---@field EncodeURI fun(...): any
---@field EncodeURIComponent fun(...): any
---@field GetUriQuery fun(...): any
---@field New fun(...): any
---@field ParseQuery fun(...): any
---@field ParseUriRequest fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
UriUtils = {}

---@class VehicleOptions
---@field Set fun(...): any
VehicleOptions = {}

---@class VehicleSelectPage
---@field StartedVehicleLoading fun(...): any
---@field StoppedVehicleLoading fun(...): any
VehicleSelectPage = {}

---@class VehicleUnlock
---@field EvaluateUnlocks fun(...): any
VehicleUnlock = {}

---@class Vehicles
---@field GetDetails fun(...): any
---@field GetPlayerControlType fun(...): any
---@field GetVehicleSelectCamSequence fun(...): any
---@field PlayerHasDRSorKERS fun(...): any
Vehicles = {}

---@class Voice
---@field PlayRaceCraftDialog fun(...): any
---@field SetVoiceDictionary fun(...): any
Voice = {}

---@class WeatherPresetLookup
---@field GetWeatherPreset fun(...): any
---@field New fun(...): any
---@field __index table
---@field _dependencies table
---@field name string
WeatherPresetLookup = {}

---@class arg
arg = {}

---@class argset
argset = {}

---@class coroutine
---@field create fun(...): any
---@field resume fun(...): any
---@field running fun(...): any
---@field status fun(...): any
---@field wrap fun(...): any
---@field yield fun(...): any
coroutine = {}

---@class dcmod
---@field log fun(...): any
---@field version number
dcmod = {}

---@class debug
---@field debug fun(...): any
---@field getfenv fun(...): any
---@field gethook fun(...): any
---@field getinfo fun(...): any
---@field getlocal fun(...): any
---@field getmetatable fun(...): any
---@field getregistry fun(...): any
---@field getupvalue fun(...): any
---@field setfenv fun(...): any
---@field sethook fun(...): any
---@field setlocal fun(...): any
---@field setmetatable fun(...): any
---@field setupvalue fun(...): any
---@field traceback fun(...): any
debug = {}

---@class evoio
---@field open fun(...): any
evoio = {}

---@class gProfilerData
---@field funcData table
---@field memUsageFunc fun(...): any
---@field memWarningCallbacks table
---@field timeWarningCallbacks table
---@field timingFunc fun(...): any
---@field warnExcLevel number
---@field warnExcTime number
---@field warnIncLevel number
---@field warnIncTime number
gProfilerData = {}

---@class math
---@field abs fun(...): any
---@field acos fun(...): any
---@field asin fun(...): any
---@field atan2 fun(...): any
---@field atan fun(...): any
---@field ceil fun(...): any
---@field cos fun(...): any
---@field cosh fun(...): any
---@field deg fun(...): any
---@field exp fun(...): any
---@field floor fun(...): any
---@field fmod fun(...): any
---@field frexp fun(...): any
---@field huge number
---@field ldexp fun(...): any
---@field log10 fun(...): any
---@field log fun(...): any
---@field max fun(...): any
---@field min fun(...): any
---@field mod fun(...): any
---@field modf fun(...): any
---@field pi number
---@field pow fun(...): any
---@field rad fun(...): any
---@field random fun(...): any
---@field randomseed fun(...): any
---@field sin fun(...): any
---@field sinh fun(...): any
---@field sqrt fun(...): any
---@field tan fun(...): any
---@field tanh fun(...): any
math = {}

---@class os
---@field clock fun(...): any
---@field date fun(...): any
---@field difftime fun(...): any
---@field exit fun(...): any
---@field remove fun(...): any
---@field rename fun(...): any
---@field time fun(...): any
os = {}

---@class string
---@field byte fun(...): any
---@field char fun(...): any
---@field dump fun(...): any
---@field find fun(...): any
---@field format fun(...): any
---@field gfind fun(...): any
---@field gmatch fun(...): any
---@field gsub fun(...): any
---@field len fun(...): any
---@field lower fun(...): any
---@field match fun(...): any
---@field rep fun(...): any
---@field reverse fun(...): any
---@field sub fun(...): any
---@field upper fun(...): any
string = {}

---@class table
---@field concat fun(...): any
---@field foreach fun(...): any
---@field foreachi fun(...): any
---@field getn fun(...): any
---@field insert fun(...): any
---@field maxn fun(...): any
---@field remove fun(...): any
---@field setn fun(...): any
---@field sort fun(...): any
table = {}

