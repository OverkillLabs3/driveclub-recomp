# The Lua environment of the game

This list was read from the running game (version 1.28) by a mod script that walked the global table, so it shows what exists at the end of start-up, not what each function does. Member suffixes: `f` function, `t` table, `s` string, `n` number, `b` boolean.

Run your own survey with a script like the one in [lua.md](lua.md#finding-out-more); `dcmod lua survey` shows how the game's own scripts use these names.

## Global functions

`AbstractClass`, `Class`, `ClassClearCaches`, `ClassMock`, `ClearTimeout`, `GetTimeout`, `IsModalShown`, `IsOSActive`, `LogLuaMemory`, `ReloadClass`, `ReloadTemplate`, `SetRealTimeout`, `SetTimeout`, `assert`, `collectgarbage`, `dofile`, `error`, `errortostring`, `gcinfo`, `getfenv`, `getmetatable`, `ipairs`, `kernelTime`, `load`, `loadfile`, `loadfileraw`, `loadstring`, `module`, `newproxy`, `next`, `numbertostring`, `pairs`, `pcall`, `print`, `rawequal`, `rawget`, `rawset`, `require`, `select`, `setfenv`, `setmetatable`, `toboolean`, `tonumber`, `tostring`, `type`, `unpack`, `xpcall`

## Other globals

`BuildConfig` (string), `DevReload` (boolean), `_VERSION` (string), `global` (userdata), `missionstm` (userdata)

## Tables

- `ActivityStream`: `Dump:f` `Entry:f` `FormatAsJsArray:f` `FormatAsJsMap:f` `GetDumpEnabled:f` `GetUrl:f` `New:f` `PostProcessResults:f` `SetDumpEnabled:f` `__index:t` `_dependencies:t` `dumpEnabled:b` `name:s` `stream_entry_action:t`
- `Actor`: `AllActors:f` `AllActorsWithProperty:f` `CollectionSize:f` `CreateProperty:f` `GetActorById:f` `GetActorByName:f` `GetActorFromColIdx:f` `GetActorId:f` `GetAnimationName:f` `GetLocalPlayer:f` `GetPlayer:f` `GetSqDistanceBetweenActors:f` `GetVehicleName:f` `GetViewedPlayer:f` `HasProperty:f` `PropertyEqualTo:f` `PropertyLessThan:f` `PropertyMoreThan:f` `PropertyNotEqualTo:f` `RemoveActor:f` `SpawnActor:f`
- `AiLiveries`: `Liveries:t` `LiveryForAiId:f` `New:f` `__index:t` `_dependencies:t` `name:s`
- `Async`: `Graph:f` `New:f` `Parallel:f` `Series:f` `__index:t` `_dependencies:t` `name:s`
- `AsyncLoad`: `IsLoadingInProgress:f`
- `AutoTestApi`: `BroadcastVehicleChoice:f` `GetCompleteSeasonData:f` `GetMatchSlotsData:f` `GetPermittedVehiclesList:f` `GoToLobbyGUI:f` `RegisterForEvent:f` `RequestMatchSlotsData:f`
- `Automation`: `Go:f` `GoReplay:f`
- `BestScore`: `UpdateScoresAndGhosts:f`
- `BootOptions`: `Get:f` `Set:f`
- `BuildOptions`: `Set:f`
- `CameraSequence`: `AdvanceToNext:f` `IsRunning:f` `New:f` `ResetTimeoutOnCurrentCamera:f` `Start:f` `StartAutoSequence:f` `Stop:f` `__index:t` `_dependencies:t` `name:s`
- `ChallengeInfo`: `GetChallengeTargetEgsId:f` `GetChallengeTargetName:f` `GetChallengeTargetScore:f` `GetMyBestChallengeScore:f`
- `ChallengeResponder`: `AcceptChallenge:f` `New:f` `__index:t` `_dependencies:t` `name:s`
- `CinematicsOptions`: `Set:f`
- `Config`: `GetDriverCount:f` `GetHumanDriverCount:f` `GetTotalHumanCount:f`
- `CountdownHelper`: `Init:f` `ManualUpdate:f` `New:f` `ResetCountdown:f` `Shutdown:f` `Update:f` `__index:t` `_dependencies:t` `name:s`
- `CrazySpeedConstants`: `New:f` `SetUp:f` `SetupParams:t` `__index:t` `_dependencies:t` `name:s`
- `DebugOptions`: `Set:f`
- `Diorama`: `GetRecentVehicle:f` `SetRecentVehicle:f`
- `Drift`: `New:f` `SetUpDrift:f` `SetupParams:t` `__index:t` `_dependencies:t` `name:s`
- `DriverActions`: `AddDriverAction:f` `DeferredActionCreation:f` `FinaliseDriverActionRewards:f` `GiveFinishPositionRewards:f` `New:f` `OnSplit:f` `Setup:f` `Stop:f` `Update:f` `__index:t` `_dependencies:t` `name:s`
- `E3Options`: `Get:f` `Set:f`
- `EGSConstants`: `AccountSessionStatus:t` `ActivityStreamSubjectType:t` `ActivityStreamVerb:t` `AggregateIncludes:t` `AggregatedProfileOption:t` `AssetAvailability:t` `AssetType:t` `AwardType:t` `BonusValueType:t` `ChallengeOrdering:t` `ChallengePrivacy:t` `ChallengeStatus:t` `ChallengeTypes:t` `Club:t` `ClubAttributeType:t` `ClubInviteStatus:t` `ClubMemberStatus:t` `ClubMemberType:t` `ClubOrderBy:t` `ClubStatus:t` `ClubType:t` `ConfigDataType:t` `Country:t` `DriverActionType:t` `EntitlementType:t` `ErrorCodes:t` `EventListName:t` `EventListType:t` `FinishPositionType:t` `FriendType:t` `GamePlayerFinishType:t` `GamePlayerStatus:t` `GameStatus:t` `LobbyPlayerStatus:t` `LobbyStatus:t` `LobbyType:t` `MissionLamsID:t` `New:f` `Objective:t` `ObjectiveType:t` `ProfileOwnerType:t` `Region:t` `RewardDisplayValue:t` `RewardType:t` `SeasonType:t` `SocialGoalType:t` `StreamEntryActions:t` `__index:t` `_dependencies:t` `name:s`
- `ErrorDialog`: `Close:f` `DevModal:f` `Show:f`
- `Event`: `Close:f` `Listen:f` `SendEvent:f`
- `FacebookDialog`: `Close:f` `Show:f`
- `Fame`: `AddFame:f` `ApplyBonusAwards:f` `ClearPot:f` `CommitPot:f` `MergePots:f`
- `FluidOptions`: `Set:f`
- `GUIConstants`: `Club:t` `Leaderboard:t` `Location:t` `New:f` `User:t` `__index:t` `_dependencies:t` `gameTips:t` `name:s`
- `GUIOptions`: `Get:f` `Set:f`
- `GameComponents`: `GatherComponents:f` `New:f` `__index:t` `_dependencies:t` `name:s`
- `GameConfig`: `AddRecordingComponent:f` `AddResource:f` `Clear:f` `ClearUsedTrackPassContext:f` `Deprecated_SetMissionStm:f` `Deprecated_SetNrLaps:f` `Deprecated_SetTimeOfDay:f` `GetAllowRetry:f` `GetCanChallenge:f` `GetChallengeId:f` `GetChallengeOriginalEventListID:f` `GetChallengePlayerGhostId:f` `GetChallengePlayerScore:f` `GetChallengeTargetAccountId:f` `GetChallengeTargetAccountName:f` `GetChallengeTargetDisplayName:f` `GetChallengeTargetGhostId:f` `GetChallengeTargetScore:f` `GetChallengeType:f` `GetCustomRetryUrl:f` `GetDisplayProgressScreens:f` `GetEgsGameID:f` `GetEgsGameSeriesID:f` `GetEnvSeed:f` `GetEnvironment:f` `GetEventID:f` `GetEventLams:f` `GetEventListID:f` `GetEventListName:f` `GetEventListType:f` `GetEventSeriesRound:f` `GetEventSeriesRoundTotal:f` `GetEventType:f` `GetFaceOffs:f` `GetFinishFameForPosition:f` `GetGameArea:f` `GetGameComponents:f` `GetGarageVehicle:f` `GetGarageWorld:f` `GetGlobalAIDifficulty:f` `GetHasObjectives:f` `GetIsDriveClubLiveEvent:f` `GetIsEventSeries:f` `GetIsFreeplayEvent:f` `GetIsLastEventInSeries:f` `GetIsTeamMode:f` `GetLiveTimers:f` `GetMatchingTimers:f` `GetMission:f` `GetMissionParams:f` `GetMissionStm:f` `GetNextSlotID:f` `GetNrCompetitors:f` `GetNrLaps:f` `GetPostRacePage:f` `GetPrecipitationType:f` `GetPresetTargetScore:f` `GetReplayFile:f` `GetResources:f` `GetSeasonID:f` `GetSingleplayerType:f` `GetSprintVariant:f` `GetTarget:f` `GetTargetAvatar:f` `GetTargetName:f` `GetTemplateID:f` `GetTimeMultiplier:f` `GetTimeOfDay:f` `GetTimeTrialDuration:f` `GetTrack:f` `GetTrackID:f` `GetTrackInfrastructureLevel:f` `GetUsedTrackPassContext:f` `GetVehicle:f` `GetWeatherType:f` `IsConfigDataInCPP:f` `IsInRace:f` `IsSingleplayer:f` `LoadedFromReplayFile:f` `New:f` `SetConfigDataInCPP:f` `SetResources:f` `__index:t` `_dependencies:t` `data:t` `dataInCPP:b` `kAllFaceOffs:n` `kNoFaceOffs:n` `kTourFaceOffsOnly:n` `name:s`
- `GameProduct`: `GetSelectedGameProduct:f`
- `GameSession`: `Create:f` `FinishSession:f` `GameJoined:f` `PostDriftScore:f` `PostFame:f` `PostLapTime:f` `PostResults:f` `UnblockAllSessions:f`
- `GameState`: `CurrentFrame:f` `CurrentGameTime:f` `EnableComponents:f` `EndGame:f` `GetCurrent:f` `GetFrameRateStats:f` `GetGameConfig:f` `GetGameProduct:f` `GetTracks:f` `GetVehicles:f` `LoadMode:f` `Quit:f` `ResetFrameRateStats:f` `RestartGame:f` `SetNoDataLayers:f` `SetProcessExitCode:f` `StartGame:f`
- `GamescomOptions`: `Get:f` `Set:f`
- `Ghosting`: `SetAllVehicleGhosting:f` `SetAllVehicleNoGhosting:f`
- `Ghosts`: `FinishRecording:f` `FinishRecordingAndStartNew:f` `StartRecording:f`
- `GlobalObjective`: `HaveDisplayableObjectives:f`
- `HUD`: `SetCountdownTimer:f`
- `Json`: `ToLua:f`
- `JsonMarshaller`: `New:f` `ToJson:f` `ToLua:f` `__index:t` `_dependencies:t` `name:s`
- `LAMS`: `Translate:f` `TranslateArray:f`
- `Livery`: `CompactLivery:f` `ExpandLivery:f` `GetLivery:f`
- `LiveryCompExpand`: `CompactLiveryDescription:f` `ExpandLiveryDescription:f` `New:f` `__index:t` `_dependencies:t` `name:s`
- `LiveryManager`: `LiveriesReady:f`
- `LuaDelegate`: `GetErrorHandler:f` `SetErrorHandler:f`
- `LuaGameEditor`: `LoadGameEditor:f` `New:f` `__index:t` `_dependencies:t` `name:s`
- `MainMenu`: `SetupGameConfig:f` `StartFirstGameExperience:f`
- `MessageOptions`: `Get:f` `Set:f`
- `MessageQueue`: `Connect:f` `Post:f` `SendGameStartTelemetry:f` `SetMessageDisabled:f` `SetMessageEnabled:f` `Update:f`
- `OSDController`: `Common_AddOverDrive:f` `Common_ClampedTimeToString:f` `Common_ClearTimersOnExit:f` `Common_ConfigureForCamera:f` `Common_ConfigureKERSAndDRS:f` `Common_ConfigureSpeedo:f` `Common_Depause:f` `Common_DisplayDiffTimeToActorAhead:f` `Common_DisplayDiffTimeToActorBehind:f` `Common_DisplayDiffTimeToTargetAhead:f` `Common_DisplayDiffTimeToTargetBehind:f` `Common_Exit:f` `Common_GetSplitColour:f` `Common_HideCatastrophicCrash:f` `Common_Pause:f` `Common_PercentageComplete:f` `Common_Reset:f` `Common_SelectTeam:f` `Common_SetBestInfo:f` `Common_SetBestInfoScore:f` `Common_SetOSDUnits:f` `Common_SetSpeedoVisibility:f` `Common_SetTargetInfo:f` `Common_SetTargetInfoScore:f` `Common_SetupTargetPanel:f` `Common_ShowCatastrophicCrash:f` `Common_ShowOnlyOverdrives:f` `Common_ShowResetToTrack:f` `Common_ShowWrongWay:f` `Common_StartPhotoMode:f` `Common_StopPhotoMode:f` `Common_TrackType:f` `Common_UpdateSpeedometer:f` `Common_UpdateTickerAction:f` `Drift_DisableScore:f` `Drift_DisplayDiffScore:f` `Drift_DriftZone:f` `Drift_ExitState:f` `Drift_Init:f` `Drift_Load:f` `Drift_ModeShowInfo:f` `Drift_SetScore:f` `Drift_SpeedZone:f` `Drift_UpdateDrift:f` `EnqueueMessage:f` `EnqueueTranslatedMessage:f` `Initialise:f` `MultiTimeTrial_Depause:f` `MultiTimeTrial_Pause:f` `New:f` `Race_Init:f` `Race_Load:f` `Race_SetLapInfo:f` `Race_SetPositionInfo:f` `Race_UpdateRaceTime:f` `Race_UpdateTracker:f` `ResetTimer:f` `TernaryOperator:f` `TimeTrial_ApplySplitColour:f` `TimeTrial_ClearHoldSplitTimers:f` `TimeTrial_HoldSplitTimers:f` `TimeTrial_Init:f` `TimeTrial_Load:f` `TimeTrial_ResetSplitTimers:f` `TimeTrial_SetCurrentLapTime:f` `TimeTrial_ShowEndSessionPrompt:f` `TimeTrial_SplitTime:f` `TimeTrial_SplitTimeNoAnim:f` `TimeTrial_UpdateDiffTime:f` `TimeTrial_UpdateLapDiffColour:f` `TimeTrial_UpdateSplitColour:f` `TopSpeed_Init:f` `TopSpeed_Load:f` `TopSpeed_ResetTotalScore:f` `__index:t` `_dependencies:t` `name:s`
- `OSDPage`: `Hide:f` `Send:f` `SetLayout:f` `Show:f`
- `OSDTime`: `GetOSDTimer:f`
- `OverDrive`: `Add:f` `Finalise:f`
- `PartyInfo`: `GetNumberInParty:f` `JoinByName:f`
- `PauseMultiTimeTrialPage`: `SetStartSession:f`
- `PausePage`: `AllowEndSession:f` `AllowRestartSeries:f`
- `Platform`: `CheckChatRestriction:f` `EgsDisconnect:f` `EgsReconnect:f` `GetE3Name:f` `GetEgsConnected:f` `GetEgsConnectionStatus:f` `GetIsAdapterUp:f` `GetIsPsnCheckInProgress:f` `GetPlatformInitialised:f` `GetPrimaryEgsAccountId:f` `GetPrimaryOnlineName:f` `GetPrimaryUserName:f` `GetPsnAvailable:f` `GetPsnPlusAvailable:f` `GetTimeInMs:f` `GetUserAvailabilityError:f` `SetEgsConnected:f` `UpdateServiceChecks:f` `UsingPlusFeature:f`
- `PlayerProfile`: `GetAccountId:f` `GetDisplayName:f` `GetDriverAppearence:f` `GetValue:f` `IsFGEComplete:f` `SetFGEComplete:f` `SetValue:f`
- `Plugins`: `LoadPlugin:f`
- `PrecipitationDriver`: `SetInitialPrecipitationState:f` `SetPrecipitationFromGameConfig:f` `StartDrivingPrecipitation:f`
- `ProductRegion`: `GetRegion:f`
- `Profiling`: `AddMemoryWarningCallback:f` `AddTimingWarningCallback:f` `ClearWarningCallbacks:f` `New:f` `Off:f` `On:f` `Profiler:f` `__index:t` `_dependencies:t` `name:s`
- `RenderOptions`: `Set:f`
- `Rewards`: `AwardDriverActionXP:f` `AwardFinishPositionXPandCash:f` `AwardObjectiveXp:f` `AwardSocialGoalXP:f` `Clear:f` `FinaliseQueuedRewards:f` `IsQueueFinished:f` `New:f` `Setup:f` `__index:t` `_dependencies:t` `name:s`
- `RoadShearOptions`: `Set:f`
- `Roads`: `GetPoslenRelativeInMetres:f` `IsPointToPoint:f` `IsPointToPointRoadLayout:f` `RegionToPoslen:f`
- `Rooms`: `GetLocation:f` `GetNetName:f` `GetRoomEnabled:f`
- `STMScript`: `LoadScript:f` `TriggerScript:f`
- `Schema`: `Boolean:f` `Check:f` `Coerce:f` `In:f` `Integer:f` `New:f` `Number:f` `Opt:f` `OptBoolean:f` `OptIn:f` `OptInteger:f` `OptNumber:f` `OptOr:f` `OptRange:f` `OptString:f` `OptStringContains:f` `OptStringPattern:f` `Or:f` `Range:f` `String:f` `StringContains:f` `StringPattern:f` `__index:t` `_dependencies:t` `name:s`
- `Selector`: `ClearAndPushPage:f` `ClearStack:f` `GetLoadingPage:f` `HandleGameCustomData:f` `InsertPage:f` `PopPage:f` `PopToPage:f` `PushPage:f` `SetActive:f` `ShowTutorial:f`
- `Settings`: `AreEGSFaceOffsVisible:f` `Audio:f` `Display:f` `DisplaySet:f`
- `SocialGoalSourceEGS`: `AllEGSGoalsAdded:f` `AllEGSGoalsUpdated:f` `CreateEGSSocialGoal:f` `DebugLogStatus:f` `EGSDisconnected:f` `EGSGoalUpdateFinished:f` `GetGoalsFromEGS:f` `GiveDevScores:f` `GiveFirstInWorldTarget:f` `GiveGoalDevTarget:f` `GiveGoalTargetFromEGS:f` `GiveInvalidScores:f` `HideEGSGoals:f` `Initialise:f` `IsNextUpdateDelayed:f` `IsTargetSane:f` `New:f` `NewEGSGoalTargetsFailed:f` `NoGoalUpdates:f` `OnGoalAttempted:f` `RemoveAllEGSGoals:f` `SetGoalUpdated:f` `SetGoalUpdating:f` `ShowEGSGoals:f` `Update:f` `UpdateGoalScore:f` `UpdateGoalScores:f` `UpdateGoalTargetFromEGS:f` `WipeSocialGoals:f` `__index:t` `_dependencies:t` `name:s`
- `SocialGoalSourceTour`: `CreateGoalInternal:f` `CreateTourGoal:f` `Initialise:f` `New:f` `OnPlayerChangedLap:f` `__index:t` `_dependencies:t` `name:s`
- `SocialGoals`: `AddActor:f` `AddSocialGoal:f` `AwardAverageSpeedXP:f` `AwardCorneringXP:f` `AwardMaxSpeedXP:f` `AwardNothing:f` `AwardSingleDriftXP:f` `CheckOverlap:f` `CheckOverlappingGoals:f` `DeactivateGoal:f` `DebugPrintGoal:f` `EvaluateGoal:f` `FindAndSetNextSocialGoal:f` `FindNextGoalFromPoslen:f` `GetActorData:f` `GetBestAttemptScore:f` `GetCurrentSocialGoal:f` `GetFailMessage:f` `GetInfoPanelDataForGoal:f` `GetPreviousGoalScore:f` `GetRunningTotalScore:f` `GetShortestWrappedDistance:f` `GetSuccessMessage:f` `GiveGoalInvalidTarget:f` `GiveGoalsToActor:f` `HideGoal:f` `HigherThanTarget:f` `Initialise:f` `InvalidGoalAttempt:f` `LeaveGoal:f` `LiveRestart:f` `New:f` `OnStatsUpdated:f` `SendSocialGoalTelemetry:f` `SetCurrentSocialGoalProgress:f` `SetGUIUnits:f` `SetGoalOverlapped:f` `SetGoalRacingLineVisibility:f` `SetNextSocialGoal:f` `SetRacingLinesShown:f` `ShowGoal:f` `Shutdown:f` `SortGoalsByZone:f` `StartGoal:f` `TellGameAboutGoal:f` `TellGameAboutGoals:f` `TranslateGoalTypes:f` `Update:f` `UpdateActorPositionInGoals:f` `UpdateDeferredShowingGoals:f` `WipeSocialGoals:f` `__index:t` `_dependencies:t` `name:s`
- `StatCollector`: `GetStats:f` `PrintStats:f` `SetEnabled:f` `SetStatFloat:f` `SetStatInt:f`
- `Stats`: `TrackStats:f`
- `StringUtils`: `IsEmptyString:f` `IsInvalidString:f` `IsNotEmptyString:f` `IsValidString:f` `New:f` `SplitParagraph:f` `StartsWith:f` `__index:t` `_dependencies:t` `join:f` `name:s` `split:f` `trim:f`
- `SystemDialog`: `Close:f` `Show:f`
- `TableUtils`: `DeepCopyTable:f` `GetValueOrAddEmptyTable:f` `IndexOf:f` `Invert:f` `IsEmpty:f` `Keys:f` `MergeArrays:f` `MergeTables:f` `New:f` `NumEntries:f` `Print:f` `ProcessTableInternal:f` `ShallowCopyTable:f` `TableToArrayInternal:f` `ToString:f` `ToStringCompact:f` `Values:f` `__index:t` `_dependencies:t` `allowPrintTable:b` `name:s`
- `TeamModeHelper`: `GetTeamFromPlayerName:f` `New:f` `SetTeamsToActors:f` `SetUpTeamsFromResources:f` `__index:t` `_dependencies:t` `name:s`
- `Telemetry`: `New:f` `PostStat:f` `__index:t` `_dependencies:t` `name:s`
- `Time`: `ExtractMilliseconds:f` `ExtractMinutes:f` `ExtractSeconds:f` `New:f` `SecondsToString:f` `TimeOfDayFloatTo24HTime:f` `ToString:f` `ToTimeOfDayString:f` `TranslateTimeStrings:f` `__index:t` `_dependencies:t` `name:s`
- `TimeManager`: `SetGameSpeed:f`
- `Timeline`: `Load:f` `Start:f` `Stop:f`
- `Tour`: `GetEventRoundTotal:f` `GetEventTotal:f` `GetNextEventWithNoStars:f` `GetNextEventWithoutMaxStars:f` `GetNumberOfStarsAvailable:f` `GetTotalStarsAvailable:f` `GetTotalStarsWon:f` `Go:f` `SetEventStarsCompleted:f`
- `Tracks`: `GetDetails:f`
- `Trophies`: `UpdateDeferredTrophies:f` `UpdatePostRaceTrophies:f`
- `UIOptions`: `Get:f` `Set:f`
- `UriUtils`: `BuildParams:f` `DecodeURI:f` `DecodeURIComponent:f` `EncodeURI:f` `EncodeURIComponent:f` `GetUriQuery:f` `New:f` `ParseQuery:f` `ParseUriRequest:f` `__index:t` `_dependencies:t` `name:s`
- `VehicleOptions`: `Set:f`
- `VehicleSelectPage`: `StartedVehicleLoading:f` `StoppedVehicleLoading:f`
- `VehicleUnlock`: `EvaluateUnlocks:f`
- `Vehicles`: `GetDetails:f` `GetPlayerControlType:f` `GetVehicleSelectCamSequence:f` `PlayerHasDRSorKERS:f`
- `Voice`: `PlayRaceCraftDialog:f` `SetVoiceDictionary:f`
- `WeatherPresetLookup`: `GetWeatherPreset:f` `New:f` `__index:t` `_dependencies:t` `name:s`
- `arg`: empty or not listable
- `argset`: `?app0?eboot.bin:b`
- `coroutine`: `create:f` `resume:f` `running:f` `status:f` `wrap:f` `yield:f`
- `dcmod`: `log:f` `version:n`
- `debug`: `debug:f` `getfenv:f` `gethook:f` `getinfo:f` `getlocal:f` `getmetatable:f` `getregistry:f` `getupvalue:f` `setfenv:f` `sethook:f` `setlocal:f` `setmetatable:f` `setupvalue:f` `traceback:f`
- `evoio`: `open:f`
- `gProfilerData`: `funcData:t` `memUsageFunc:f` `memWarningCallbacks:t` `timeWarningCallbacks:t` `timingFunc:f` `warnExcLevel:n` `warnExcTime:n` `warnIncLevel:n` `warnIncTime:n`
- `math`: `abs:f` `acos:f` `asin:f` `atan2:f` `atan:f` `ceil:f` `cos:f` `cosh:f` `deg:f` `exp:f` `floor:f` `fmod:f` `frexp:f` `huge:n` `ldexp:f` `log10:f` `log:f` `max:f` `min:f` `mod:f` `modf:f` `pi:n` `pow:f` `rad:f` `random:f` `randomseed:f` `sin:f` `sinh:f` `sqrt:f` `tan:f` `tanh:f`
- `os`: `clock:f` `date:f` `difftime:f` `exit:f` `remove:f` `rename:f` `time:f`
- `string`: `byte:f` `char:f` `dump:f` `find:f` `format:f` `gfind:f` `gmatch:f` `gsub:f` `len:f` `lower:f` `match:f` `rep:f` `reverse:f` `sub:f` `upper:f`
- `table`: `concat:f` `foreach:f` `foreachi:f` `getn:f` `insert:f` `maxn:f` `remove:f` `setn:f` `sort:f`

## Events

The game's scripts send these event names with `Event.SendEvent` and listen for `NewLap`, `GUIPushPage` and `GUIPopPage` with `Event.Listen`:

`AddSocialGoal Advance AwdFirstOfMany AwdSpeedDemon CameraFade ClearSocialGoals Countdown Driver EGSGoalAttempted EndInspector Finished GUI GameModeSetup HideGoalFinish HideSocialGoal Input IsFrontEnd Mission OSDVisible Photo Quit RacCntl RaceLineSection RaceOSD RaceState ReqEGSGoals RqNewGoalTargets SelectedVehicle SetCamera SetMirrorVis ShowGoalFinish ShowSocialGoal SkipLap SocialGoalResult SpeedTrapLines StartLiveStat StopLiveStat TGoalAttempted TGoalStarted TODSetup TogglePause UpdateFaceOff UpdateSocialGoal WDgDump WDgDumpReset WDgStart WDgStartVehicle WDgStop WebMsg eResults`
