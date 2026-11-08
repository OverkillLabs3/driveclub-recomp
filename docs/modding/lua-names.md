# Names used by the game's Lua scripts

Read from 102 compiled scripts (1115 functions, 62027 instructions).
A call is listed under the name the code used to reach it; where the code gets a function from a local variable or
a parameter, the owner shows as `?`. Counts are the number of places in the code, not how often they run.

## Global variables read

| name | uses | scripts |
|---|---:|---|
| `DriveClubTestHelpers` | 809 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 27 more |
| `AutoTest` | 266 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 29 more |
| `print` | 213 | activitystream, alltracksprofile, alltracksprofile_race_test, allvehicleprofile, and 32 more |
| `type` | 213 | activitystream, alltracksprofile, alltracksprofile_race_test, allvehicleprofile, and 29 more |
| `tostring` | 185 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, async, and 27 more |
| `string` | 130 | activitystream, ailiveries, alltracksprofile_race_test, async, and 21 more |
| `table` | 130 | activitystream, async, asyncqueue, driveclub_testhelpers, and 23 more |
| `Event` | 126 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 21 more |
| `os` | 118 | allvehicleprofile, async, bodytypes_test, challenge_test, and 16 more |
| `global` | 115 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 19 more |
| `assert` | 114 | async, camerasequence, dashboardviewer, dispatcher, and 15 more |
| `require` | 114 | ailiveries, alltracksprofile, alltracksprofile_race_test, allvehicleprofile, and 47 more |
| `OSDController` | 106 | osd |
| `GameConfig` | 104 | driveclub_testhelpers, driveractions, gameconfig, pseudorandom_race_test, and 4 more |
| `BootOptions` | 95 | autotestbootscript, benchmark, blankbootscript, cupcakebootscript, and 22 more |
| `ipairs` | 92 | activitystream, alltracksprofile, alltracksprofile_race_test, allvehicleprofile, and 27 more |
| `EGSConstants` | 80 | driveclub_testhelpers, driveractions, egsconstants, guiutils, and 4 more |
| `Class` | 73 | activitystream, ailiveries, alltracksprofile, alltracksprofile_race_test, and 69 more |
| `pairs` | 70 | activitystream, async, class, driveclub_testhelpers, and 17 more |
| `OSDPage` | 66 | osd |
| `GUIOptions` | 64 | autotestbootscript, blankbootscript, cupcakebootscript, customisationbootscript, and 16 more |
| `SocialGoals` | 58 | socialgoals, socialgoalsourceegs |
| `math` | 52 | driveclub_testhelpers, driveractions, guiutils, luatodotviz, and 10 more |
| `_G` | 49 | autotesthelpers, class, evomock, guiutils, and 9 more |
| `error` | 49 | async, class, dispatcher, evomock, and 9 more |
| `TableUtils` | 45 | async, challenge_test, driveclub_testhelpers, gamecomponents, and 13 more |
| `Lams` | 43 | guiutils, lams, templater, time |
| `rawget` | 42 | autotesthelpers, class, evomock, jsonmarshaller, and 8 more |
| `Schema` | 41 | model, schema |
| `tonumber` | 41 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, challenge_test, and 13 more |
| `EvoMock` | 34 | evomock |
| `Actor` | 32 | allvehicleprofile, camerasequence, drift, driveclub_testhelpers, and 5 more |
| `GameState` | 32 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 6 more |
| `MessageOptions` | 28 | autotestbootscript, autotesthelpers, defaultbootscript, defaultbootscript30hz |
| `SocialGoalSourceEGS` | 28 | socialgoalsourceegs |
| `debug` | 27 | async, class, luatodotviz, luaunit, and 4 more |
| `LuaToDotViz` | 25 | luatodotviz, memwatcher |
| `LuaUnit` | 24 | autotesthelpers, framework, luaunit |
| `rawset` | 24 | class, evomock, guiutils, junitresult, and 3 more |
| `Model` | 23 | model |
| `gProfilerData` | 23 | profiling |
| `GUIUtils` | 20 | guiutils, templater |
| `StringUtils` | 20 | autotestbootscript, defaultbootscript, defaultbootscript30hz, driveclub_testhelpers, and 3 more |
| `Dispatcher` | 19 | dispatcher, templater |
| `MemWatcher` | 19 | memwatcher |
| `Plugins` | 19 | autotestbootscript, cupcakebootscript, customisationbootscript, defaultbootscript, and 15 more |
| `RenderOptions` | 19 | autotestbootscript, blankbootscript, cupcakebootscript, customisationbootscript, and 15 more |
| `ActivityStream` | 18 | activitystream |
| `errortostring` | 18 | async, model, schema, templater |
| `CameraSequence` | 17 | camerasequence, driveclub_testhelpers, watchdogtest |
| `Rewards` | 17 | driveractions, rewards, socialgoals |
| `evoio` | 17 | basicsmoketest, demo_race, junitresult, laptime_test, and 4 more |
| `UriUtils` | 16 | dispatcher, uriutils |
| `jUnitResult` | 16 | autotesthelpers, framework, junitresult |
| `Time` | 15 | laptime_test, osd, time |
| `Selector` | 14 | driveclub_testhelpers, loadingscreen_test, osd |
| `numbertostring` | 13 | async, driveclub_testhelpers, jsonmarshaller, laptime_test, and 3 more |
| `setmetatable` | 13 | autotesthelpers, class, evomock, lams, and 2 more |
| `TestRace` | 12 | demo_race, quick_race_test, race_fps_test |
| `UnitResult` | 12 | luaunit |
| `BuildConfig` | 11 | luatodotviz, memwatcher, model, referencefinder, and 2 more |
| `MessageQueue` | 11 | autotestbootscript, autotesthelpers, defaultbootscript, defaultbootscript30hz, and 1 more |
| `AutoTestApi` | 10 | driveclub_testhelpers, online_test, party_test |
| `ClearTimeout` | 10 | camerasequence, countdownhelper, osd, socialgoals |
| `arg` | 10 | alltracksprofile_race_test, dashboardviewer, driveclub_testhelpers |
| `LayoutSections` | 9 | dispatcher, layoutsections, templater |
| `PartyInfo` | 9 | party_test |
| `Tour` | 9 | tour_test |
| `DriverActions` | 8 | driveractions |
| `E3Options` | 8 | e3_demobootscript, e3_demobootscript_dev |
| `TestTrackFrameRate` | 8 | alltracksprofile, alltracksprofile_race_test |
| `TestTrackModesSmoke` | 8 | laptime_test, modes_vehicle_test |
| `select` | 8 | evomock, luatodotviz |
| `AsyncQueue` | 7 | asyncqueue |
| `Profiling` | 7 | profiling |
| `Automation` | 6 | driveclub_testhelpers, watchdogtest |
| `GUIConstants` | 6 | guiconstants, socialgoalsourceegs |
| `SetTimeout` | 6 | camerasequence, countdownhelper, osd, socialgoals |
| `CountdownHelper` | 5 | countdownhelper |
| `CrazySpeedConstants` | 5 | crazyspeed, driveractions |
| `JsonMarshaller` | 5 | dispatcher, guiutils, jsonmarshaller |
| `coroutine` | 5 | autotesthelpers, luaunit |
| `package` | 5 | class, framework |
| `AiLiveries` | 4 | ailiveries |
| `Async` | 4 | async, driveractions |
| `GamescomOptions` | 4 | gamescom2014_demobootscript, gamescom2014_demobootscript_dev |
| `ReloadClass` | 4 | dispatcher, model |
| `Settings` | 4 | osd |
| `SocialGoalSourceTour` | 4 | socialgoalsourcetour |
| `TestBasicSmoke` | 4 | basicsmoketest |
| `TestTimeTrial` | 4 | timetrial_test |
| `TestVehicle` | 4 | allvehicleprofile |
| `dofile` | 4 | framework |
| `getmetatable` | 4 | autotesthelpers, evomock, luatodotviz |
| `missionstm` | 4 | challenge_test, driveclub_testhelpers, tour_test |
| `toboolean` | 4 | schema, time |
| `Drift` | 3 | drift |
| `Fame` | 3 | rewards |
| `GameComponents` | 3 | driveclub_testhelpers, gamecomponents |
| `LAMS` | 3 | socialgoals, socialgoalsourcetour, time |
| `OverDrive` | 3 | rewards |
| `ReferenceFinder` | 3 | memwatcher, referencefinder |
| `TeamModeHelper` | 3 | teammode |
| `Telemetry` | 3 | rewards, telem |
| `Templater` | 3 | dispatcher, templater |
| `gcinfo` | 3 | class, memwatcher, profiling |
| `xpcall` | 3 | class, luaunit, templater |
| `ChallengeInfo` | 2 | challenge_test |
| `DevReload` | 2 | class |
| `HUD` | 2 | countdownhelper |
| `LiveryCompExpand` | 2 | liverycompexpand |
| `MacroRecorder` | 2 | dispatcher |
| `Platform` | 2 | autotesthelpers, driveclub_testhelpers |
| `ReloadTemplate` | 2 | dispatcher |
| `TestBodyType` | 2 | bodytypes_test |
| `TestBoot` | 2 | boottest, mainmenuboot_test |
| `TestSoak` | 2 | random_vehicle_track_mode_test, soak_test |
| `TestSubmitSmoke` | 2 | submit_smoke_fast, submit_smoke_test |
| `WeatherPresetLookup` | 2 | memwatcher, weatherpresetlookup |
| `argset` | 2 | weatherpresetlookup |
| `collectgarbage` | 2 | memwatcher |
| `getfenv` | 2 | luatodotviz |
| `guiUniqueIdCounter` | 2 | guiutils |
| `io` | 2 | luaunit |
| `loadfile` | 2 | class, lams |
| `loadstring` | 2 | luaunit, templater |
| `next` | 2 | guiutils, luatodotviz |
| `orderedPairs` | 2 | luaunit |
| `pcall` | 2 | luaunit, schema |
| `unpack` | 2 | evomock, luaunit |

## Global variables written

| name | uses | scripts |
|---|---:|---|

## Calls

| call | uses | scripts |
|---|---:|---|
| `DriveClubTestHelpers:info` | 250 | alltracksprofile_race_test, allvehicleprofile, bodytypes_test, challenge_test, and 16 more |
| `type` | 213 | activitystream, alltracksprofile, alltracksprofile_race_test, allvehicleprofile, and 29 more |
| `print` | 210 | activitystream, alltracksprofile, alltracksprofile_race_test, allvehicleprofile, and 31 more |
| `tostring` | 185 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, async, and 27 more |
| `Event.SendEvent` | 117 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 19 more |
| `assert` | 117 | async, camerasequence, class, dashboardviewer, and 16 more |
| `require` | 114 | ailiveries, alltracksprofile, alltracksprofile_race_test, allvehicleprofile, and 47 more |
| `AutoTest:WaitForSeconds` | 107 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 26 more |
| `table.insert` | 97 | activitystream, async, asyncqueue, driveclub_testhelpers, and 21 more |
| `BootOptions.Set` | 92 | autotestbootscript, benchmark, blankbootscript, cupcakebootscript, and 20 more |
| `ipairs` | 92 | activitystream, alltracksprofile, alltracksprofile_race_test, allvehicleprofile, and 27 more |
| `DriveClubTestHelpers:GetArgs` | 81 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 16 more |
| `DriveClubTestHelpers:outputPair` | 80 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, bodytypes_test, and 16 more |
| `Class` | 73 | activitystream, ailiveries, alltracksprofile, alltracksprofile_race_test, and 69 more |
| `pairs` | 70 | activitystream, async, class, driveclub_testhelpers, and 17 more |
| `GUIOptions.Set` | 63 | autotestbootscript, blankbootscript, cupcakebootscript, customisationbootscript, and 15 more |
| `OSDPage.Send` | 54 | osd |
| `DriveClubTestHelpers:status` | 51 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, bodytypes_test, and 11 more |
| `string.format` | 51 | activitystream, async, class, driveclub_testhelpers, and 12 more |
| `error` | 49 | async, class, dispatcher, evomock, and 9 more |
| `os.date` | 46 | challenge_test, driveclub_testhelpers, guiutils, kioskdemo_test, and 5 more |
| `os.time` | 44 | challenge_test, driveclub_testhelpers, kioskdemo_test, online_test, and 4 more |
| `Log` | 43 | laptime_test, socialgoalsourceegs |
| `rawget` | 42 | autotesthelpers, class, evomock, jsonmarshaller, and 8 more |
| `?:write` | 41 | basicsmoketest, laptime_test, long_game_test, luatodotviz, and 2 more |
| `tonumber` | 40 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, challenge_test, and 13 more |
| `AutoTest:InitAutoTest` | 31 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 27 more |
| `AutoTest:RunAllTestsAndQuit` | 31 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 27 more |
| `Lams:Translate` | 30 | guiutils, templater, time |
| `AutoTest:Wait` | 29 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 14 more |
| `AutoTest:WaitForEvent` | 29 | challenge_test, driveclub_testhelpers, kioskdemo_test, online_test, and 3 more |
| `DriveClubTestHelpers:BootToMainMenu` | 28 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 24 more |
| `MessageOptions.Set` | 25 | autotestbootscript, autotesthelpers, defaultbootscript, defaultbootscript30hz |
| `?:gsub` | 24 | activitystream, autotesthelpers, guiphotos_tod, guiutils, and 4 more |
| `rawset` | 24 | class, evomock, guiutils, junitresult, and 3 more |
| `Actor.GetPlayer` | 22 | allvehicleprofile, driveclub_testhelpers, driveractions, guiphotos_tod, and 2 more |
| `math.random` | 22 | online_test, party_test, random_vehicle_track_mode_test, soak_test, and 1 more |
| `AutoTest:WaitForFrames` | 21 | alltracksprofile_race_test, allvehicleprofile, autotesthelpers, driveclub_testhelpers, and 2 more |
| `TestRaceTrack` | 21 | alltracksprofile, alltracksprofile_race_test, demo_race, guiphotos_tod, and 2 more |
| `errortostring` | 21 | async, class, model, schema, and 1 more |
| `Plugins.LoadPlugin` | 19 | autotestbootscript, cupcakebootscript, customisationbootscript, defaultbootscript, and 15 more |
| `RenderOptions.Set` | 19 | autotestbootscript, blankbootscript, cupcakebootscript, customisationbootscript, and 15 more |
| `string.sub` | 19 | alltracksprofile_race_test, driveclub_testhelpers, guiutils, liverycompexpand, and 3 more |
| `evoio.open` | 17 | basicsmoketest, demo_race, junitresult, laptime_test, and 4 more |
| `math.floor` | 17 | driveclub_testhelpers, driveractions, guiutils, rewards, and 2 more |
| `table.concat` | 17 | activitystream, jsonmarshaller, layoutsections, liverycompexpand, and 4 more |
| `DriveClubTestHelpers:StartMode` | 16 | allvehicleprofile, bodytypes_test, driveclub_testhelpers, laptime_test, and 8 more |
| `string.find` | 16 | alltracksprofile_race_test, driveclub_testhelpers, guiutils, liverycompexpand, and 3 more |
| `string.gsub` | 16 | ailiveries, driveclub_testhelpers, guiphotos_tod, guiutils, and 3 more |
| `DriveClubTestHelpers:EndEvent` | 15 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 11 more |
| `EndTest` | 15 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, bodytypes_test, and 11 more |
| `gc.edge` | 15 | referencefinder |
| `os.clock` | 15 | allvehicleprofile, bodytypes_test, demo_race, driveclub_testhelpers, and 4 more |
| `?:close` | 14 | basicsmoketest, laptime_test, long_game_test, luatodotviz, and 2 more |
| `DriveClubTestHelpers:startswith` | 14 | challenge_test, driveclub_testhelpers, tour_test |
| `TableUtils:Print` | 14 | challenge_test, driveclub_testhelpers, online_test, party_test, and 4 more |
| `numbertostring` | 14 | async, class, driveclub_testhelpers, jsonmarshaller, and 4 more |
| `DriveClubTestHelpers:GetTrackList` | 13 | alltracksprofile, alltracksprofile_race_test, basicsmoketest, demo_race, and 8 more |
| `setmetatable` | 13 | autotesthelpers, class, evomock, lams, and 2 more |
| `?:AddDriverAction` | 12 | driveractions |
| `DriveClubTestHelpers:EndOfRaceProgress` | 12 | challenge_test, kioskdemo_test, laptime_test, pseudorandom_race_test, and 6 more |
| `ActionTest` | 11 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, bodytypes_test, and 7 more |
| `DriveClubTestHelpers:GetCarList` | 11 | allvehicleprofile, basicsmoketest, demo_race, laptime_test, and 6 more |
| `DriveClubTestHelpers:PauseTillPlayerFinished` | 11 | alltracksprofile_race_test, challenge_test, demo_race, pseudorandom_race_test, and 5 more |
| `ClearTimeout` | 10 | camerasequence, countdownhelper, osd, socialgoals |
| `DriveClubTestHelpers:output` | 10 | demo_race, driveclub_testhelpers, quick_race_test, race_fps_test |
| `resultHandler` | 10 | model |
| `string.gmatch` | 10 | driveclub_testhelpers, guiutils |
| `?:append` | 9 | luatodotviz |
| `Schema:Opt` | 9 | schema |
| `clock` | 9 | async |
| `os.difftime` | 9 | challenge_test, kioskdemo_test, party_test, random_vehicle_track_mode_test, and 1 more |
| `?:dottify_table` | 8 | luatodotviz |
| `DriveClubTestHelpers:CycleView` | 8 | alltracksprofile_race_test, allvehicleprofile, modes_vehicle_test |
| `DriveClubTestHelpers:SwitchToAI` | 8 | driveclub_testhelpers, kioskdemo_test, online_test, party_test, and 1 more |
| `E3Options.Set` | 8 | e3_demobootscript, e3_demobootscript_dev |
| `MakeMatcher` | 8 | schema |
| `OSDPage.SetLayout` | 8 | osd |
| `PartyInfo.GetNumberInParty` | 8 | party_test |
| `Selector.PushPage` | 8 | loadingscreen_test, osd |
| `TestTrack` | 8 | laptime_test, modes_vehicle_test |
| `select` | 8 | evomock, luatodotviz |
| `?:find` | 7 | autotesthelpers, evomock, gameconfig, guiutils, and 3 more |
| `DriveClubTestHelpers:RunningOnPS4` | 7 | demo_race, driveclub_testhelpers |
| `GameState.CurrentGameTime` | 7 | autotesthelpers, driveclub_testhelpers |
| `TestActions` | 7 | challenge_test, loadingscreen_test, online_test, party_test, and 3 more |
| `?.file:write` | 6 | junitresult |
| `?:FindAndSetNextSocialGoal` | 6 | socialgoals |
| `Automation.Go` | 6 | driveclub_testhelpers, watchdogtest |
| `DriveClubTestHelpers:StartLineCrossed` | 6 | laptime_test, pseudorandom_race_test, race_fps_test, restart_test, and 1 more |
| `OSDController:Common_SetSpeedoVisibility` | 6 | osd |
| `ReloadClass` | 6 | class, dispatcher, model |
| `SetTimeout` | 6 | camerasequence, countdownhelper, osd, socialgoals |
| `TestTimeTrialTrack` | 6 | long_game_test, timetrial_test |
| `math.randomseed` | 6 | online_test, party_test, random_vehicle_track_mode_test |
| `shallowcopy` | 6 | long_game_test |
| `testresultfile:write` | 6 | demo_race |
| `?._EntityNameFromModelName` | 5 | model |
| `?:GetChallengeId` | 5 | gameconfig, osd |
| `?:define_node` | 5 | luatodotviz |
| `?:gmatch` | 5 | templater, uriutils |
| `?:lower` | 5 | class |
| `ContextToString` | 5 | schema |
| `DriveClubTestHelpers:ChangeView` | 5 | alltracksprofile_race_test, allvehicleprofile, bodytypes_test, timetrial_test |
| `DriveClubTestHelpers:IsPoint2Point` | 5 | alltracksprofile_race_test, driveclub_testhelpers, laptime_test, tour_test |
| `DriveClubTestHelpers:PauseTillIsRaceDist1` | 5 | alltracksprofile, kioskdemo_test, modes_vehicle_test, race_fps_test, and 1 more |
| `DriveClubTestHelpers:SkipPreRaceCameras` | 5 | challenge_test, driveclub_testhelpers, kioskdemo_test, restart_test, and 1 more |
| `DriveClubTestHelpers:StartRace` | 5 | alltracksprofile, alltracksprofile_race_test, demo_race, quick_race_test, and 1 more |
| `DriveClubTestHelpers:StartTimeTrial` | 5 | alltracksprofile, basicsmoketest, guiphotos_tod, long_game_test, and 1 more |
| `GameState.ResetFrameRateStats` | 5 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, guiphotos_tod, and 1 more |
| `OSDController:Common_ClampedTimeToString` | 5 | osd |
| `OSDController:Common_ConfigureSpeedo` | 5 | osd |
| `Time:ToString` | 5 | laptime_test, osd |
| `ValToString` | 5 | schema |
| `string.len` | 5 | driveclub_testhelpers, liverycompexpand, luatodotviz, stringutils |
| `table.remove` | 5 | async, asyncqueue, long_game_test, memwatcher |
| `?.datasource:Read` | 4 | model |
| `?:IsNextUpdateDelayed` | 4 | socialgoalsourceegs |
| `?:SetGoalUpdated` | 4 | socialgoalsourceegs |
| `?:dottify_ref` | 4 | luatodotviz |
| `?:dottify_upvalue_ref` | 4 | luatodotviz |
| `?:sub` | 4 | schema, stringutils, uriutils |
| `?:update_node_depth` | 4 | luatodotviz |
| `AutoTest:WaitForGameState` | 4 | autotesthelpers, driveclub_testhelpers, online_test, party_test |
| `CameraSequence:Start` | 4 | camerasequence, driveclub_testhelpers, watchdogtest |
| `CameraSequence:Stop` | 4 | camerasequence, driveclub_testhelpers, watchdogtest |
| `Dispatcher:ProcessTemplate` | 4 | dispatcher, templater |
| `DoRace` | 4 | kioskdemo_test, party_test |
| `DriveClubTestHelpers:AddAutoPageResponse` | 4 | challenge_test, driveclub_testhelpers, kioskdemo_test, tour_test |
| `DriveClubTestHelpers:SkipLap` | 4 | pseudorandom_race_test, quick_race_test, restart_test |
| `DriveClubTestHelpers:StartModeOnly` | 4 | driveclub_testhelpers, qa_soak_test, restart_test |
| `Event.Close` | 4 | autotesthelpers, driveclub_testhelpers, timetrial_test |
| `GameConfig:GetNrLaps` | 4 | pseudorandom_race_test, quick_race_test, restart_test |
| `GamescomOptions.Set` | 4 | gamescom2014_demobootscript, gamescom2014_demobootscript_dev |
| `KeyToString` | 4 | schema |
| `OSDController:ResetTimer` | 4 | osd |
| `OSDController:TernaryOperator` | 4 | osd |
| `Rewards:AwardSocialGoalXP` | 4 | socialgoals |
| `Settings.Display` | 4 | osd |
| `StringUtils:IsEmptyString` | 4 | autotestbootscript, defaultbootscript, defaultbootscript30hz, stringutils |
| `TestBasicSmokeTrack` | 4 | basicsmoketest |
| `TestVehicleLoad` | 4 | allvehicleprofile |
| `WriteToFile` | 4 | long_game_test, timetrial_test |
| `callersFailureHandler` | 4 | model |
| `debug.getfenv` | 4 | luatodotviz, referencefinder |
| `debug.traceback` | 4 | async, class, luaunit, templater |
| `dofile` | 4 | framework |
| `failure` | 4 | async, model |
| `table.foreachi` | 4 | luaunit |
| `?.socialGoals:DeactivateGoal` | 3 | socialgoalsourceegs, socialgoalsourcetour |
| `?:EGSGoalUpdateFinished` | 3 | socialgoalsourceegs |
| `?:IsUsable` | 3 | socialgoals |
| `?:LeaveGoal` | 3 | socialgoals |
| `?:ProcessTableInternal` | 3 | tableutils |
| `?:SetGoalOverlapped` | 3 | socialgoals |
| `?:SetGoalRacingLineVisibility` | 3 | socialgoals |
| `?:ShowGoal` | 3 | socialgoals |
| `?:_FixQueryValue` | 3 | model |
| `?:_FixQueryValues` | 3 | model |
| `?:dottify_environment_ref` | 3 | luatodotviz |
| `?:dottify_function` | 3 | luatodotviz |
| `?:dottify_thread` | 3 | luatodotviz |
| `?:dottify_userdata` | 3 | luatodotviz |
| `?:get_environment` | 3 | luatodotviz |
| `?:make_label` | 3 | luatodotviz |
| `?:match` | 3 | luatodotviz, schema, uriutils |
| `ActivityStream:FormatAsJsMap` | 3 | activitystream |
| `Actor.GetActorByName` | 3 | camerasequence, watchdogtest |
| `CameraSequence:ResetTimeoutOnCurrentCamera` | 3 | camerasequence |
| `DriveClubTestHelpers:GetFPS` | 3 | timetrial_test |
| `DriveClubTestHelpers:GetPlayersCurrentPosition` | 3 | demo_race, race_fps_test |
| `DriveClubTestHelpers:PauseTillPlayerRacing` | 3 | challenge_test, restart_test |
| `DriveClubTestHelpers:PauseToggle` | 3 | qa_soak_test, restart_test |
| `DriveClubTestHelpers:SetupTimeOfDay` | 3 | bodytypes_test, guiphotos_tod, quick_race_test |
| `DriveClubTestHelpers:TakePhoto` | 3 | timetrial_test |
| `Event.Listen` | 3 | autotesthelpers, driveclub_testhelpers |
| `EvoMock.MatchTableContentsSubset` | 3 | evomock |
| `Fame.AddFame` | 3 | rewards |
| `GameState.GetCurrent` | 3 | autotesthelpers, driveclub_testhelpers, gameconfig |
| `GameState.IsHMD` | 3 | driveclub_testhelpers, tour_test |
| `LAMS.TranslateArray` | 3 | socialgoals, socialgoalsourcetour, time |
| `LayoutSections:New` | 3 | dispatcher, templater |
| `LuaToJsonRecursive` | 3 | jsonmarshaller |
| `MessageOptions.Get` | 3 | autotestbootscript, defaultbootscript, defaultbootscript30hz |
| `MessageQueue.Connect` | 3 | autotestbootscript, defaultbootscript, defaultbootscript30hz |
| `MessageQueue.SetMessageDisabled` | 3 | autotestbootscript, defaultbootscript, defaultbootscript30hz |
| `MessageQueue.SetMessageEnabled` | 3 | autotestbootscript, defaultbootscript, defaultbootscript30hz |
| `OSDPage.Show` | 3 | osd |
| `PerformRace` | 3 | challenge_test, random_vehicle_track_mode_test, tour_test |
| `Selector.PopPage` | 3 | osd |
| `StringUtils:trim` | 3 | driveclub_testhelpers, stringutils |
| `TableUtils:IsEmpty` | 3 | junitresult, socialgoalsourceegs |
| `TableUtils:MergeArrays` | 3 | gamecomponents, model |
| `TableUtils:MergeTables` | 3 | lams, model, uriutils |
| `debug.getinfo` | 3 | profiling, referencefinder |
| `debug.getmetatable` | 3 | luatodotviz, referencefinder |
| `failTask` | 3 | async |
| `gc.traverse` | 3 | referencefinder |
| `getmetatable` | 3 | autotesthelpers, evomock, luatodotviz |
| `request.handler` | 3 | dispatcher |
| `self._EntityNameFromModelName` | 3 | model |
| `self:_FixRowResultModelName` | 3 | model |
| `self:_GetModelToSourceMap` | 3 | model |
| `string.byte` | 3 | driveclub_testhelpers, jsonmarshaller, luatodotviz |
| `string.char` | 3 | driveclub_testhelpers, uriutils |
| `string.match` | 3 | guiphotos_tod, stringutils |
| `success` | 3 | async |
| `table.getn` | 3 | luaunit |
| `table.sort` | 3 | activitystream, luaunit, socialgoals |
| `toboolean` | 3 | time |
| `xpcall` | 3 | class, luaunit, templater |
| `?.GetShortestWrappedDistance` | 2 | socialgoals |
| `?.current:sub` | 2 | lams |
| `?.datasource:Delete` | 2 | model |
| `?.datasource:Update` | 2 | model |
| `?.file:close` | 2 | junitresult |
| `?.isFunction` | 2 | luaunit |
| `?.socialGoals:AddSocialGoal` | 2 | socialgoalsourceegs, socialgoalsourcetour |
| `?.socialGoals:GiveGoalInvalidTarget` | 2 | socialgoalsourceegs |
| `?.socialGoals:GiveGoalsToActor` | 2 | socialgoalsourceegs, socialgoalsourcetour |
| `?.task` | 2 | async, asyncqueue |
| `?:AddCss` | 2 | layoutsections |
| `?:AddJs` | 2 | layoutsections |
| `?:CheckOverlap` | 2 | socialgoals |
| `?:CheckOverlappingGoals` | 2 | socialgoals |
| `?:Coerce` | 2 | schema |
| `?:CreateGoalInternal` | 2 | socialgoalsourcetour |
| `?:DebugPrintGoal` | 2 | socialgoals |
| `?:GetActorData` | 2 | socialgoals |
| `?:GiveDevScores` | 2 | socialgoalsourceegs |
| `?:GiveGoalDevTarget` | 2 | socialgoalsourceegs |
| `?:HideGoal` | 2 | socialgoals |
| `?:InvalidGoalAttempt` | 2 | socialgoals |
| `?:MakeGraph` | 2 | memwatcher |
| `?:RemoveAllEGSGoals` | 2 | socialgoalsourceegs |
| `?:RemoveObjectsFromListFoundInGraph` | 2 | memwatcher |
| `?:SaveLeakyObjectsList` | 2 | memwatcher |
| `?:SetGoalUpdating` | 2 | socialgoalsourceegs |
| `?:StyleGraphWithLeakiness` | 2 | memwatcher |
| `?:TableToArrayInternal` | 2 | tableutils |
| `?:TellGameAboutGoal` | 2 | socialgoals |
| `?:UpdateGoalScores` | 2 | socialgoalsourceegs |
| `?:_FixQueryKeys` | 2 | model |
| `?:_GetIdFieldName` | 2 | model |
| `?:byte` | 2 | uriutils |
| `?:dottify_metatable_ref` | 2 | luatodotviz |
| `?:get_metatable` | 2 | luatodotviz |
| `?:len` | 2 | driveclub_testhelpers, stringutils |
| `?:process_options` | 2 | luatodotviz |
| `?:reverse` | 2 | guiutils |
| `?:upper` | 2 | driveclub_testhelpers |
| `AbstractClass` | 2 | class, model |
| `Actor.GetActorId` | 2 | camerasequence, drift |
| `Actor.HasProperty` | 2 | driveclub_testhelpers |
| `AddToExpectationList` | 2 | evomock |
| `AutoTest:Transition` | 2 | autotesthelpers, driveclub_testhelpers |
| `AutoTestApi.BroadcastVehicleChoice` | 2 | online_test, party_test |
| `AutoTestApi.GetCompleteSeasonData` | 2 | driveclub_testhelpers |
| `AutoTestApi.GetPermittedVehiclesList` | 2 | online_test, party_test |
| `BootOptions.Get` | 2 | dispatcher, gamecomponents |
| `CheckFinished` | 2 | async |
| `CleanUnwanatedTableEntries` | 2 | long_game_test |
| `CrazySpeedConstants:SetUp` | 2 | driveractions |
| `DriveClubTestHelpers:DartScreenShot` | 2 | bodytypes_test |
| `DriveClubTestHelpers:DartTestCase` | 2 | driveclub_testhelpers |
| `DriveClubTestHelpers:GetLapTime` | 2 | laptime_test |
| `DriveClubTestHelpers:JoinLobby` | 2 | online_test, party_test |
| `DriveClubTestHelpers:PauseTillRaceDist` | 2 | challenge_test, random_vehicle_track_mode_test |
| `DriveClubTestHelpers:PrintLoadCompleteInfo` | 2 | driveclub_testhelpers |
| `DriveClubTestHelpers:SetCompetitorsAI` | 2 | driveclub_testhelpers |
| `DriveClubTestHelpers:SyncToTime` | 2 | online_test, party_test |
| `DriveClubTestHelpers:warning` | 2 | driveclub_testhelpers |
| `GameComponents:GatherComponents` | 2 | driveclub_testhelpers |
| `GameConfig:GetGlobalAIDifficulty` | 2 | driveclub_testhelpers |
| `GameConfig:GetTrack` | 2 | driveclub_testhelpers, socialgoals |
| `GameState.CurrentFrame` | 2 | autotesthelpers |
| `GetRandomPlayerInfo` | 2 | socialgoals, socialgoalsourceegs |
| `GetSetCardinalityFunc` | 2 | evomock |
| `GetTarget` | 2 | evomock |
| `HUD.SetCountdownTimer` | 2 | countdownhelper |
| `HandleCardinality` | 2 | evomock |
| `JsonMarshaller:ToJson` | 2 | dispatcher, guiutils |
| `LuaToDotViz:SaveGraph` | 2 | memwatcher |
| `LuaUnit:runTestMethodName` | 2 | luaunit |
| `MacroRecorder:RecordRequest` | 2 | dispatcher |
| `MessageQueue.Post` | 2 | autotesthelpers, socialgoals |
| `OSDController:Common_GetSplitColour` | 2 | osd |
| `OverDrive.Add` | 2 | rewards |
| `ProcessTasks` | 2 | async |
| `ProgressToNextAutoSequenceCamera` | 2 | camerasequence |
| `ReloadTemplate` | 2 | dispatcher |
| `Rewards:AwardDriverActionXP` | 2 | driveractions |
| `SafeToString` | 2 | schema |
| `Schema:Coerce` | 2 | model, schema |
| `Selector.PopToPage` | 2 | osd |
| `SetCurrentCameraIndex` | 2 | camerasequence |
| `StringUtils:IsNotEmptyString` | 2 | lams, stringutils |
| `StringUtils:StartsWith` | 2 | model |
| `TableKeyString` | 2 | tableutils |
| `TableUtils:DeepCopyTable` | 2 | async, tableutils |
| `TableValueString` | 2 | tableutils |
| `Telemetry:PostStat` | 2 | rewards |
| `TestAction` | 2 | kioskdemo_test, pseudorandom_race_test |
| `TestBodies` | 2 | bodytypes_test |
| `TestSetup` | 2 | qa_soak_test, restart_test |
| `Time:TranslateTimeStrings` | 2 | time |
| `Tour.GetTotalStarsAvailable` | 2 | tour_test |
| `Tour.GetTotalStarsWon` | 2 | tour_test |
| `UriUtils:DecodeURIComponent` | 2 | uriutils |
| `UriUtils:ParseQuery` | 2 | dispatcher, uriutils |
| `collectgarbage` | 2 | memwatcher |
| `coroutine.yield` | 2 | autotesthelpers, luaunit |
| `debug.getlocal` | 2 | referencefinder |
| `debug.getupvalue` | 2 | luatodotviz, referencefinder |
| `debug.sethook` | 2 | profiling |
| `gProfilerData.memUsageFunc` | 2 | profiling |
| `gProfilerData.timingFunc` | 2 | profiling |
| `gcinfo` | 2 | class, memwatcher |
| `io.stdout:write` | 2 | luaunit |
| `jUnitResult:Init` | 2 | autotesthelpers, framework |
| `jUnitResult:WriteFinalResults` | 2 | junitresult |
| `jUnitResult:WriteInitialResults` | 2 | junitresult |
| `loadfile` | 2 | class, lams |
| `loadstring` | 2 | luaunit, templater |
| `next` | 2 | guiutils, luatodotviz |
| `orderedPairs` | 2 | luaunit |
| `pcall` | 2 | luaunit, schema |
| `self:Check` | 2 | schema |
| `self:Execute` | 2 | asyncqueue |
| `self:Pop` | 2 | asyncqueue |
| `self:_FixSingleResult` | 2 | model |
| `string.lower` | 2 | guiutils |
| `string.upper` | 2 | guiutils |
| `unpack` | 2 | evomock, luaunit |

## Table keys and methods

| key | uses | scripts |
|---|---:|---|
| `GroundWindSpeedRatio` | 1821 | weatherpresetlookup |
| `HighCloud` | 1821 | weatherpresetlookup |
| `LowCloud` | 1821 | weatherpresetlookup |
| `Sky` | 1821 | weatherpresetlookup |
| `SkyVisBias` | 1821 | weatherpresetlookup |
| `WindDirection` | 1821 | weatherpresetlookup |
| `WindSpeed` | 1821 | weatherpresetlookup |
| `info` | 251 | alltracksprofile_race_test, allvehicleprofile, bodytypes_test, challenge_test, and 16 more |
| `Set` | 211 | autotestbootscript, autotesthelpers, benchmark, blankbootscript, and 21 more |
| `data` | 161 | autotesthelpers, gameconfig, lams, model, and 2 more |
| `TransitionTime` | 149 | weatherpresetlookup |
| `Clear` | 147 | gameconfig, rewards, weatherpresetlookup |
| `Cloudy` | 143 | weatherpresetlookup |
| `Overcast` | 143 | weatherpresetlookup |
| `Stormy` | 143 | weatherpresetlookup |
| `SendEvent` | 117 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 19 more |
| `WaitForSeconds` | 108 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 27 more |
| `lamsid` | 102 | driveractions, egsconstants, socialgoals |
| `icon` | 99 | egsconstants |
| `insert` | 97 | activitystream, async, asyncqueue, driveclub_testhelpers, and 21 more |
| `GetArgs` | 82 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 17 more |
| `outputPair` | 81 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, bodytypes_test, and 16 more |
| `name` | 77 | activitystream, async, class, driveclub_testhelpers, and 6 more |
| `status` | 58 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 15 more |
| `write` | 55 | basicsmoketest, demo_race, junitresult, laptime_test, and 5 more |
| `Send` | 54 | osd |
| `format` | 51 | activitystream, async, class, driveclub_testhelpers, and 12 more |
| `source` | 50 | model |
| `date` | 46 | challenge_test, driveclub_testhelpers, guiutils, kioskdemo_test, and 5 more |
| `time` | 44 | challenge_test, driveclub_testhelpers, kioskdemo_test, online_test, and 4 more |
| `gsub` | 41 | activitystream, ailiveries, autotesthelpers, driveclub_testhelpers, and 9 more |
| `PrecipitationOverride` | 38 | weatherpresetlookup |
| `target` | 35 | camerasequence, dispatcher, driveclub_testhelpers, gameconfig, and 5 more |
| `InitAutoTest` | 32 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 28 more |
| `RunAllTestsAndQuit` | 32 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 28 more |
| `outT` | 32 | liverycompexpand |
| `Translate` | 31 | guiutils, lams, templater, time |
| `id` | 31 | driveclub_testhelpers, memwatcher, osd |
| `inT` | 31 | liverycompexpand |
| `Wait` | 30 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, autotesthelpers, and 14 more |
| `WaitForEvent` | 30 | autotesthelpers, challenge_test, driveclub_testhelpers, kioskdemo_test, and 4 more |
| `BootToMainMenu` | 29 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 25 more |
| `goalsData` | 25 | socialgoals, socialgoalsourceegs |
| `sub` | 25 | alltracksprofile_race_test, driveclub_testhelpers, guiutils, lams, and 6 more |
| `zoneStart` | 24 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `find` | 23 | alltracksprofile_race_test, autotesthelpers, driveclub_testhelpers, evomock, and 8 more |
| `node2id` | 23 | luatodotviz |
| `GetPlayer` | 22 | allvehicleprofile, driveclub_testhelpers, driveractions, guiphotos_tod, and 2 more |
| `WaitForFrames` | 22 | alltracksprofile_race_test, allvehicleprofile, autotesthelpers, driveclub_testhelpers, and 2 more |
| `datasource` | 22 | model |
| `random` | 22 | online_test, party_test, random_vehicle_track_mode_test, soak_test, and 1 more |
| `value` | 21 | class, evomock, luatodotviz, memwatcher, and 1 more |
| `edge` | 20 | referencefinder |
| `zoneEnd` | 20 | socialgoals, socialgoalsourcetour |
| `LoadPlugin` | 19 | autotestbootscript, cupcakebootscript, customisationbootscript, defaultbootscript, and 15 more |
| `currentGoalId` | 19 | socialgoals |
| `overdriveType` | 19 | egsconstants, rewards |
| `statName` | 19 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `DriverActionType` | 18 | driveractions, egsconstants, rewards |
| `avatar` | 18 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `nodes` | 18 | luatodotviz, memwatcher |
| `StartMode` | 17 | allvehicleprofile, bodytypes_test, driveclub_testhelpers, laptime_test, and 8 more |
| `clock` | 17 | allvehicleprofile, async, bodytypes_test, demo_race, and 6 more |
| `close` | 17 | basicsmoketest, demo_race, junitresult, laptime_test, and 4 more |
| `concat` | 17 | activitystream, jsonmarshaller, layoutsections, liverycompexpand, and 4 more |
| `floor` | 17 | driveclub_testhelpers, driveractions, guiutils, rewards, and 2 more |
| `important` | 17 | luatodotviz, memwatcher |
| `open` | 17 | basicsmoketest, demo_race, junitresult, laptime_test, and 4 more |
| `EndEvent` | 16 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, basicsmoketest, and 12 more |
| `Print` | 15 | challenge_test, driveclub_testhelpers, online_test, party_test, and 5 more |
| `actorsData` | 15 | socialgoals |
| `club` | 15 | socialgoals |
| `egsGoals` | 15 | socialgoalsourceegs |
| `gmatch` | 15 | driveclub_testhelpers, guiutils, templater, uriutils |
| `socialGoals` | 15 | socialgoalsourceegs, socialgoalsourcetour |
| `startswith` | 15 | challenge_test, driveclub_testhelpers, tour_test |
| `GetTrackList` | 14 | alltracksprofile, alltracksprofile_race_test, basicsmoketest, demo_race, and 9 more |
| `goalId` | 14 | socialgoals, socialgoalsourcetour |
| `query` | 14 | dispatcher, model |
| `scoreUpdated` | 14 | socialgoalsourceegs |
| `AddDriverAction` | 13 | driveractions |
| `EndOfRaceProgress` | 13 | challenge_test, driveclub_testhelpers, kioskdemo_test, laptime_test, and 7 more |
| `testResults` | 13 | junitresult |
| `zone` | 13 | socialgoals, socialgoalsourceegs |
| `GetCarList` | 12 | allvehicleprofile, basicsmoketest, demo_race, driveclub_testhelpers, and 7 more |
| `PauseTillPlayerFinished` | 12 | alltracksprofile_race_test, challenge_test, demo_race, driveclub_testhelpers, and 6 more |
| `SocialGoalType` | 12 | egsconstants, rewards, socialgoals, socialgoalsourceegs, and 1 more |
| `action` | 12 | async, dispatcher, evomock, uriutils |
| `failureCount` | 12 | junitresult, luaunit |
| `file` | 12 | junitresult |
| `next` | 12 | async, asyncqueue, evomock |
| `object` | 12 | driveclub_testhelpers |
| `objects` | 12 | memwatcher |
| `raceGrid` | 12 | driveclub_testhelpers |
| `scoreUpdating` | 12 | socialgoalsourceegs |
| `sections` | 12 | layoutsections |
| `viewtype` | 12 | camerasequence, driveclub_testhelpers, guiphotos_tod, watchdogtest |
| `ActorName` | 11 | socialgoals |
| `expectations` | 11 | evomock |
| `output` | 11 | demo_race, driveclub_testhelpers, quick_race_test, race_fps_test |
| `previousGoalInfoTimeOutId` | 11 | socialgoals |
| `targetInvalid` | 11 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `testCount` | 11 | junitresult, luaunit |
| `Opt` | 10 | schema |
| `append` | 10 | luatodotviz |
| `defaultSection` | 10 | layoutsections, templater |
| `funcData` | 10 | profiling |
| `onError` | 10 | model |
| `onSuccess` | 10 | model |
| `removed` | 10 | socialgoals, socialgoalsourceegs |
| `sync` | 10 | model |
| `CycleView` | 9 | alltracksprofile_race_test, allvehicleprofile, driveclub_testhelpers, modes_vehicle_test |
| `ReduceSpeedFactor` | 9 | crazyspeed |
| `StartLineCrossed` | 9 | driveclub_testhelpers, laptime_test, pseudorandom_race_test, race_fps_test, and 2 more |
| `SwitchToAI` | 9 | driveclub_testhelpers, kioskdemo_test, online_test, party_test, and 1 more |
| `_EntityNameFromModelName` | 9 | model |
| `area` | 9 | class, dispatcher, uriutils |
| `cardinality` | 9 | evomock |
| `conditions` | 9 | model |
| `difftime` | 9 | challenge_test, kioskdemo_test, party_test, random_vehicle_track_mode_test, and 1 more |
| `dottify_table` | 9 | luatodotviz |
| `durationTimeoutId` | 9 | camerasequence |
| `hidden` | 9 | socialgoals, socialgoalsourceegs |
| `iTimeTrialSplitTimer` | 9 | osd |
| `image` | 9 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `label` | 9 | luatodotviz, memwatcher |
| `playerInGoal` | 9 | socialgoals, socialgoalsourceegs |
| `queue` | 9 | asyncqueue |
| `result` | 9 | autotesthelpers, framework, luaunit, socialgoals |
| `targetLookupId` | 9 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `type` | 9 | activitystream, driveclub_testhelpers, gameconfig, guiutils, and 1 more |
| `DriftCollisionTimeOut` | 8 | drift |
| `DriftScoringAngleHighDegrees` | 8 | drift |
| `DriftScoringAngleHighScore` | 8 | drift |
| `DriftScoringAngleLowDegrees` | 8 | drift |
| `DriftScoringAngleLowScore` | 8 | drift |
| `DriftScoringAngleLowSpeedMPH` | 8 | drift |
| `DriftScoringAngleLowSpeedModiier` | 8 | drift |
| `DriftScoringChainCooldown` | 8 | drift |
| `DriftScoringProxCooldown` | 8 | drift |
| `DriftScoringProxDistance` | 8 | drift |
| `DriftScoringProxHighMult` | 8 | drift |
| `DriftScoringProxLowMult` | 8 | drift |
| `DriftScoringSingleDistHighM` | 8 | drift |
| `DriftScoringSingleDistLowM` | 8 | drift |
| `DriftScoringSpeedHighMPH` | 8 | drift |
| `DriftScoringSpeedHighScore` | 8 | drift |
| `DriftScoringSpeedLowMPH` | 8 | drift |
| `DriftScoringSpeedLowScore` | 8 | drift |
| `DriftScoringTimeHighMult` | 8 | drift |
| `DriftScoringTimeHighSeconds` | 8 | drift |
| `DriftScoringTimeLowMult` | 8 | drift |
| `DriftScoringTimeLowSeconds` | 8 | drift |
| `DriftUnitAngleScore` | 8 | drift |
| `DriftUnitDistanceScore` | 8 | drift |
| `DriftingMaxHoldingAngleDegrees` | 8 | drift |
| `DriftingMinHoldingAngleDegrees` | 8 | drift |
| `DriftingMinHoldingTimeSeconds` | 8 | drift |
| `DriftingMinInitialAngleDegrees` | 8 | drift |
| `DriftingMinSpeedMPH` | 8 | drift |
| `GetNumberInParty` | 8 | party_test |
| `Go` | 8 | challenge_test, driveclub_testhelpers, tour_test, watchdogtest |
| `None` | 8 | egsconstants |
| `Poslen` | 8 | driveclub_testhelpers, socialgoals, socialgoalsourceegs |
| `PushPage` | 8 | loadingscreen_test, osd |
| `RunningOnPS4` | 8 | demo_race, driveclub_testhelpers |
| `SetLayout` | 8 | osd |
| `ToString` | 8 | laptime_test, osd, tableutils, time |
| `current` | 8 | lams |
| `filename` | 8 | junitresult |
| `lower` | 8 | autotesthelpers, class, guiutils |
| `prefix` | 8 | egsconstants, guiutils |
| `snapshots` | 8 | memwatcher |
| `style` | 8 | luatodotviz, memwatcher |
| `use_html` | 8 | luatodotviz |
| `A` | 7 | luatodotviz |
| `B` | 7 | luatodotviz |
| `Common_SetSpeedoVisibility` | 7 | osd |
| `CurrentGameTime` | 7 | autotesthelpers, driveclub_testhelpers |
| `FindAndSetNextSocialGoal` | 7 | socialgoals |
| `OverrideArea` | 7 | dispatcher |
| `ProcessTemplate` | 7 | dispatcher, templater |
| `bHoldSplitTimers` | 7 | osd |
| `color` | 7 | luatodotviz, memwatcher |
| `edges` | 7 | luatodotviz |
| `funcs` | 7 | referencefinder |
| `goalOverlapped` | 7 | socialgoals |
| `iDriftScoreTimer` | 7 | osd |
| `iTimeDifferenceTimer` | 7 | osd |
| `kMaxTime` | 7 | osd |
| `laps` | 7 | gameconfig, laptime_test |
| `len` | 7 | driveclub_testhelpers, liverycompexpand, luatodotviz, stringutils |
| `localisedNames` | 7 | socialgoals |
| `max_depth` | 7 | luatodotviz |
| `missionStm` | 7 | gameconfig, tour_test |
| `nameMessage` | 7 | socialgoals, socialgoalsourceegs |
| `nextGoalId` | 7 | socialgoals |
| `numResults` | 7 | async |
| `onData` | 7 | model |
| `timeGoingFast` | 7 | driveractions |
| `ChangeView` | 6 | alltracksprofile_race_test, allvehicleprofile, bodytypes_test, driveclub_testhelpers, and 1 more |
| `Coerce` | 6 | model, schema |
| `Common_ClampedTimeToString` | 6 | osd |
| `Common_ConfigureSpeedo` | 6 | osd |
| `Get` | 6 | autotestbootscript, defaultbootscript, defaultbootscript30hz, dispatcher, and 2 more |
| `GetChallengeId` | 6 | gameconfig, osd |
| `IsHMD` | 6 | driveclub_testhelpers, tour_test |
| `IsPoint2Point` | 6 | alltracksprofile_race_test, driveclub_testhelpers, laptime_test, tour_test |
| `IsUsable` | 6 | socialgoals, socialgoalsourceegs |
| `New` | 6 | class, dispatcher, evomock, templater |
| `PauseTillIsRaceDist1` | 6 | alltracksprofile, driveclub_testhelpers, kioskdemo_test, modes_vehicle_test, and 2 more |
| `RaceDistance` | 6 | driveclub_testhelpers, tour_test |
| `SkipPreRaceCameras` | 6 | challenge_test, driveclub_testhelpers, kioskdemo_test, restart_test, and 1 more |
| `StartRace` | 6 | alltracksprofile, alltracksprofile_race_test, demo_race, driveclub_testhelpers, and 2 more |
| `StartTimeTrial` | 6 | alltracksprofile, basicsmoketest, driveclub_testhelpers, guiphotos_tod, and 2 more |
| `Stop` | 6 | camerasequence, driveclub_testhelpers, driveractions, watchdogtest |
| `Update` | 6 | countdownhelper, driveractions, model, socialgoals, and 1 more |
| `__orderedIndex` | 6 | luaunit |
| `__subjectType` | 6 | activitystream |
| `class` | 6 | driveractions, guiutils, long_game_test |
| `currentpage` | 6 | driveclub_testhelpers |
| `define_node` | 6 | luatodotviz |
| `delayBeforeNextGoalUpdate` | 6 | socialgoalsourceegs |
| `duration` | 6 | camerasequence |
| `enabled` | 6 | rewards |
| `goal` | 6 | socialgoals |
| `goalsToShow` | 6 | socialgoals |
| `hadInvalidAttempt` | 6 | socialgoals |
| `jsonString` | 6 | jsonmarshaller |
| `marked` | 6 | referencefinder |
| `min` | 6 | driveclub_testhelpers, guiutils, luatodotviz |
| `model` | 6 | model |
| `node2depth` | 6 | luatodotviz |
| `node_override_func` | 6 | luatodotviz, memwatcher |
| `onCacheComplete` | 6 | model |
| `overlapping` | 6 | socialgoals |
| `playerApproaching` | 6 | socialgoals, socialgoalsourceegs |
| `racingLineShown` | 6 | socialgoals |
| `randomseed` | 6 | online_test, party_test, random_vehicle_track_mode_test |
| `shape` | 6 | luatodotviz, memwatcher |
| `show_environments` | 6 | luatodotviz |
| `test` | 6 | bootscript_autotest, boottest, challenge_test, discover_test, and 2 more |
| `testsInCurrentSplit` | 6 | junitresult |
| `trackBannerText` | 6 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `A_id` | 5 | luatodotviz |
| `AddAutoPageResponse` | 5 | challenge_test, driveclub_testhelpers, kioskdemo_test, tour_test |
| `AnyTimes` | 5 | evomock |
| `AwardSocialGoalXP` | 5 | rewards, socialgoals |
| `B_id` | 5 | luatodotviz |
| `DeactivateGoal` | 5 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `GetNrLaps` | 5 | gameconfig, pseudorandom_race_test, quick_race_test, restart_test |
| `IsEmptyString` | 5 | autotestbootscript, defaultbootscript, defaultbootscript30hz, stringutils |
| `IsNextUpdateDelayed` | 5 | socialgoalsourceegs |
| `IsPlayer` | 5 | countdownhelper, driveclub_testhelpers, timetrial_test |
| `Read` | 5 | model |
| `ResetFrameRateStats` | 5 | alltracksprofile, alltracksprofile_race_test, allvehicleprofile, guiphotos_tod, and 1 more |
| `ResetTimer` | 5 | osd |
| `SetGoalUpdated` | 5 | socialgoalsourceegs |
| `ShowGoal` | 5 | socialgoals, socialgoalsourceegs |
| `SkipLap` | 5 | driveclub_testhelpers, pseudorandom_race_test, quick_race_test, restart_test |
| `Start` | 5 | camerasequence, driveclub_testhelpers, watchdogtest |
| `StartModeOnly` | 5 | driveclub_testhelpers, qa_soak_test, restart_test |
| `TernaryOperator` | 5 | osd |
| `UnderPlayerControl` | 5 | allvehicleprofile, driveclub_testhelpers, guiphotos_tod, watchdogtest |
| `WaitForGameState` | 5 | autotesthelpers, driveclub_testhelpers, online_test, party_test |
| `_FixQueryValue` | 5 | model |
| `_FixRowResultModelName` | 5 | model |
| `_GetModelToSourceMap` | 5 | model |
| `__index` | 5 | class, evomock, lams, schema, and 1 more |
| `_cancelled` | 5 | async |
| `_tries` | 5 | async |
| `actions` | 5 | activitystream, driveractions |
| `byte` | 5 | driveclub_testhelpers, jsonmarshaller, luatodotviz, uriutils |
| `connection` | 5 | driveclub_testhelpers |
| `dottify_ref` | 5 | luatodotviz |
| `dottify_upvalue_ref` | 5 | luatodotviz |
| `getfenv` | 5 | luatodotviz, referencefinder |
| `goalUpdateInProgress` | 5 | socialgoalsourceegs |
| `last` | 5 | referencefinder |
| `log` | 5 | socialgoalsourceegs |
| `match` | 5 | guiphotos_tod, luatodotviz, schema, stringutils, and 1 more |
| `message` | 5 | socialgoals, socialgoalsourceegs |
| `missionparams` | 5 | gameconfig |
| `node_importance_func` | 5 | luatodotviz |
| `numTasksInLastRun` | 5 | asyncqueue |
| `playerBest` | 5 | socialgoals |
| `previousGoalId` | 5 | socialgoals |
| `raceLineEnd` | 5 | socialgoals |
| `remove` | 5 | async, asyncqueue, long_game_test, memwatcher |
| `resources` | 5 | gameconfig |
| `show_metatables` | 5 | luatodotviz |
| `targetText` | 5 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `timeEntirelyOutside` | 5 | driveractions |
| `tourGoalsOnSpecificLap` | 5 | socialgoalsourcetour |
| `track` | 5 | egsconstants, gameconfig, laptime_test |
| `trackBannerHeader` | 5 | socialgoals, socialgoalsourceegs |
| `update_node_depth` | 5 | luatodotviz |
| `vehicle` | 5 | egsconstants, gameconfig, laptime_test |
| `wantsToBeShown` | 5 | socialgoalsourceegs |
| `Account` | 4 | challenge_test |
| `AddCss` | 4 | layoutsections, templater |
| `AddJs` | 4 | layoutsections, templater |
| `Challenges` | 4 | challenge_test |
| `Close` | 4 | autotesthelpers, driveclub_testhelpers, timetrial_test |
| `Club` | 4 | egsconstants, guiconstants |
| `Display` | 4 | osd |
| `EGSGoalUpdateFinished` | 4 | socialgoalsourceegs |
| `Execute` | 4 | asyncqueue |
| `FormatAsJsMap` | 4 | activitystream |
| `GetFPS` | 4 | driveclub_testhelpers, timetrial_test |
| `GetPlayersCurrentPosition` | 4 | demo_race, driveclub_testhelpers, race_fps_test |
| `GetShortestWrappedDistance` | 4 | socialgoals, socialgoalsourceegs |
| `GiveGoalInvalidTarget` | 4 | socialgoals, socialgoalsourceegs |
| `HideGoal` | 4 | socialgoals, socialgoalsourceegs |
| `Id` | 4 | challenge_test |
| `Init` | 4 | autotesthelpers, countdownhelper, framework, junitresult |
| `Initialise` | 4 | osd, socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `IsEmpty` | 4 | junitresult, socialgoalsourceegs, tableutils |
| `LeaveGoal` | 4 | socialgoals |
| `Listen` | 4 | autotesthelpers, driveclub_testhelpers, timetrial_test |
| `MatchTableContentsSubset` | 4 | evomock |
| `MergeArrays` | 4 | gamecomponents, model, tableutils |
| `MergeTables` | 4 | lams, model, tableutils, uriutils |
| `PauseTillPlayerRacing` | 4 | challenge_test, driveclub_testhelpers, restart_test |
| `PauseToggle` | 4 | driveclub_testhelpers, qa_soak_test, restart_test |
| `ProcessTableInternal` | 4 | tableutils |
| `ResetTimeoutOnCurrentCamera` | 4 | camerasequence |
| `RewardType` | 4 | egsconstants, rewards |
| `SaveGraph` | 4 | luatodotviz, memwatcher |
| `SetGoalOverlapped` | 4 | socialgoals |
| `SetGoalRacingLineVisibility` | 4 | socialgoals |
| `SetupParams` | 4 | crazyspeed, drift |
| `SetupTimeOfDay` | 4 | bodytypes_test, driveclub_testhelpers, guiphotos_tod, quick_race_test |
| `TakePhoto` | 4 | driveclub_testhelpers, timetrial_test |
| `Track` | 4 | challenge_test, driveclub_testhelpers |
| `_FixQueryValues` | 4 | model |
| `__type` | 4 | activitystream |
| `actionInstanceId` | 4 | driveractions |
| `associations` | 4 | model |
| `attemptOrder` | 4 | rewards |
| `autoPageResponses` | 4 | driveclub_testhelpers |
| `childTime` | 4 | profiling |
| `childUsage` | 4 | profiling |
| `classCount` | 4 | junitresult |
| `cssUrisCodeOrdered` | 4 | layoutsections |
| `currentGoalProgess` | 4 | socialgoals |
| `currentSplit` | 4 | junitresult |
| `currentTestName` | 4 | luaunit |
| `default` | 4 | crazyspeed, drift, guiutils |
| `dottify_environment_ref` | 4 | luatodotviz |
| `dottify_function` | 4 | luatodotviz |
| `dottify_thread` | 4 | luatodotviz |
| `dottify_userdata` | 4 | luatodotviz |
| `dumpEnabled` | 4 | activitystream |
| `errorList` | 4 | luaunit |
| `fadeout` | 4 | camerasequence |
| `fadeoutTimeoutId` | 4 | camerasequence |
| `fields` | 4 | model |
| `first` | 4 | referencefinder |
| `foreachi` | 4 | luaunit |
| `gavePenalty` | 4 | driveractions |
| `gcIncFromFirst` | 4 | memwatcher |
| `gcIncFromPrevious` | 4 | memwatcher |
| `gcSize` | 4 | memwatcher |
| `get_environment` | 4 | luatodotviz |
| `getmetatable` | 4 | luatodotviz, referencefinder |
| `huge` | 4 | referencefinder |
| `isFunction` | 4 | luaunit |
| `jsUrisCodeOrdered` | 4 | layoutsections |
| `lastDraftingXPTotal` | 4 | driveractions |
| `lastDriftXPTotal` | 4 | driveractions |
| `lastXPTotalGiven` | 4 | driveractions |
| `limit` | 4 | model |
| `list` | 4 | referencefinder |
| `make_label` | 4 | luatodotviz |
| `matchSlots` | 4 | driveclub_testhelpers |
| `memWarningCallbacks` | 4 | profiling |
| `n_nodes` | 4 | luatodotviz |
| `nameMessageUsesTarget` | 4 | socialgoals, socialgoalsourceegs |
| `nextSnapshotId` | 4 | memwatcher |
| `noGoalUpdates` | 4 | socialgoalsourceegs |
| `oldGlobalValue` | 4 | evomock |
| `overlapPriority` | 4 | socialgoals, socialgoalsourcetour |
| `passedLastGoal` | 4 | socialgoals |
| `previousGoalScore` | 4 | socialgoals |
| `replayfile` | 4 | gameconfig |
| `runTestClassByName` | 4 | luaunit |
| `showEGSGoals` | 4 | socialgoalsourceegs |
| `show_unimportant` | 4 | luatodotviz |
| `show_upvalues` | 4 | luatodotviz |
| `splitPassed` | 4 | driveractions |
| `suiteName` | 4 | junitresult |
| `task` | 4 | async, asyncqueue |
| `testHasFailure` | 4 | luaunit |
| `timeWarningCallbacks` | 4 | profiling |
| `toldGUI` | 4 | socialgoals, socialgoalsourceegs |
| `tourGoals` | 4 | socialgoalsourcetour |
| `traceback` | 4 | async, class, luaunit, templater |
| `tracks` | 4 | submit_smoke_fast, submit_smoke_test |
| `traverse` | 4 | referencefinder |
| `trim` | 4 | driveclub_testhelpers, stringutils |
| `units` | 4 | osd |
| `upper` | 4 | driveclub_testhelpers, guiutils |
| `vehicles` | 4 | submit_smoke_fast, submit_smoke_test |
| `verbosity` | 4 | luaunit |
| `view` | 4 | dispatcher |
| `wasActive` | 4 | driveractions |
| `AccumulatedWheelieScore` | 3 | driveractions |
| `AddFame` | 3 | rewards |
| `AddSocialGoal` | 3 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `AnyArgs` | 3 | evomock |
| `AverageSpeed` | 3 | egsconstants, socialgoals |
| `AwardDriverActionXP` | 3 | driveractions, rewards |
| `Bonus` | 3 | crazyspeed, drift, egsconstants |
| `Cash` | 3 | egsconstants |
| `Check` | 3 | schema |
| `CheckOverlap` | 3 | socialgoals |
| `CheckOverlappingGoals` | 3 | socialgoals |
| `Common_GetSplitColour` | 3 | osd |
| `Connect` | 3 | autotestbootscript, defaultbootscript, defaultbootscript30hz |
| `ConstructClass` | 3 | class, model |
| `CreateGoalInternal` | 3 | socialgoalsourcetour |
| `CreateGraph` | 3 | luatodotviz, memwatcher |
| `CurrentSeason` | 3 | driveclub_testhelpers |
| `DartScreenShot` | 3 | bodytypes_test, driveclub_testhelpers |
| `DartTestCase` | 3 | driveclub_testhelpers |
| `DebugPrintGoal` | 3 | socialgoals |
| `DecodeURIComponent` | 3 | uriutils |
| `DeepCopyTable` | 3 | async, tableutils |
| `Delete` | 3 | model |
| `EnviornmentCollisionsTotal` | 3 | driveractions |
| `EventId` | 3 | challenge_test |
| `ExitedFront` | 3 | socialgoals |
| `FinishPosition` | 3 | driveractions, egsconstants, rewards |
| `GatherComponents` | 3 | driveclub_testhelpers, gamecomponents |
| `GetActorByName` | 3 | camerasequence, watchdogtest |
| `GetActorData` | 3 | socialgoals |
| `GetCurrent` | 3 | autotesthelpers, driveclub_testhelpers, gameconfig |
| `GetGlobalAIDifficulty` | 3 | driveclub_testhelpers, gameconfig |
| `GetLapTime` | 3 | driveclub_testhelpers, laptime_test |
| `GetLatest` | 3 | model |
| `GetSection` | 3 | dispatcher, layoutsections, templater |
| `GetTrack` | 3 | driveclub_testhelpers, gameconfig, socialgoals |
| `GiveDevScores` | 3 | socialgoalsourceegs |
| `GiveGoalDevTarget` | 3 | socialgoalsourceegs |
| `GiveGoalsToActor` | 3 | socialgoals, socialgoalsourceegs, socialgoalsourcetour |
| `Integer` | 3 | egsconstants, schema |
| `InvalidGoalAttempt` | 3 | socialgoals |
| `IsFinished` | 3 | driveclub_testhelpers |
| `IsNotEmptyString` | 3 | lams, stringutils |
| `JoinLobby` | 3 | driveclub_testhelpers, online_test, party_test |
| `Liveries` | 3 | ailiveries |
| `LocalTime` | 3 | driveclub_testhelpers, guiphotos_tod |
| `MakeGraph` | 3 | memwatcher |
| `MaxSpeed` | 3 | egsconstants, socialgoals |
| `Mode` | 3 | challenge_test |
| `ObjectiveType` | 3 | egsconstants, guiutils |
| `OnHideGoal` | 3 | socialgoals, socialgoalsourceegs |
| `OnShowGoal` | 3 | socialgoals, socialgoalsourceegs |
| `ParseQuery` | 3 | dispatcher, uriutils |
| `PauseTillRaceDist` | 3 | challenge_test, driveclub_testhelpers, random_vehicle_track_mode_test |
| `Pop` | 3 | asyncqueue |
| `PopPage` | 3 | osd |
| `PostStat` | 3 | rewards, telem |
| `Presets` | 3 | weatherpresetlookup |
| `PrintLoadCompleteInfo` | 3 | driveclub_testhelpers |
| `RemoveAllEGSGoals` | 3 | socialgoalsourceegs |
| `RemoveObjectsFromListFoundInGraph` | 3 | memwatcher |
| `SaveLeakyObjectsList` | 3 | memwatcher |
| `SetCompetitorsAI` | 3 | driveclub_testhelpers |
| `SetGoalUpdating` | 3 | socialgoalsourceegs |
| `SetMessageDisabled` | 3 | autotestbootscript, defaultbootscript, defaultbootscript30hz |
| `SetMessageEnabled` | 3 | autotestbootscript, defaultbootscript, defaultbootscript30hz |
| `SetUp` | 3 | crazyspeed, driveractions |
| `Show` | 3 | osd |
| `StartsWith` | 3 | model, stringutils |
| `String` | 3 | egsconstants, schema |
| `StyleGraphWithLeakiness` | 3 | memwatcher |
| `Submit` | 3 | model |
| `SyncToTime` | 3 | driveclub_testhelpers, online_test, party_test |
| `TableToArrayInternal` | 3 | tableutils |
| `TellGameAboutGoal` | 3 | socialgoals |
| `TellGameAboutGoals` | 3 | socialgoals, socialgoalsourcetour |
| `ToJson` | 3 | dispatcher, guiutils, jsonmarshaller |
| `ToLua` | 3 | dispatcher, jsonmarshaller |
| `Transition` | 3 | autotesthelpers, driveclub_testhelpers |
| `TranslateArray` | 3 | socialgoals, socialgoalsourcetour, time |
| `TranslateTimeStrings` | 3 | time |
| `UpdateGoalScores` | 3 | socialgoalsourceegs |
| `VehicleCollisionsTotal` | 3 | driveractions |
| `WipeSocialGoals` | 3 | socialgoals, socialgoalsourceegs |
| `WriteFinalResults` | 3 | junitresult |
| `WriteInitialResults` | 3 | junitresult |
| `_FixQueryKeys` | 3 | model |
| `_FixSingleResult` | 3 | model |
| `_GetIdFieldName` | 3 | model |
| `_dependencies` | 3 | class |
| `_failed` | 3 | async |
| `_id` | 3 | activitystream |
| `_startTime` | 3 | async |
| `addFailure` | 3 | junitresult, luaunit |
| `aiDifficulty` | 3 | gameconfig |
| `allowPrintTable` | 3 | tableutils |
| `bFirstPersonCamera` | 3 | osd |
| `char` | 3 | driveclub_testhelpers, uriutils |
| `className` | 3 | junitresult |
| `completeCallback` | 3 | asyncqueue |
| `cssUris` | 3 | layoutsections |
| `currentClassName` | 3 | luaunit |
| `dataInCPP` | 3 | gameconfig |
| `disabledDriverActions` | 3 | driveractions |
| `displayFinalResult` | 3 | junitresult, luaunit |
| `dottify_metatable_ref` | 3 | luatodotviz |
| `endTest` | 3 | junitresult, luaunit |
| `event` | 3 | autotesthelpers, guiutils, socialgoals |
| `eventLists` | 3 | driveclub_testhelpers |
| `eventSeriesRound` | 3 | gameconfig |
| `eventSeriesRoundTotal` | 3 | gameconfig |
| `failure` | 3 | asyncqueue |
| `gamecomponents` | 3 | gameconfig |
| `get_metatable` | 3 | luatodotviz |
| `getinfo` | 3 | profiling, referencefinder |
| `getn` | 3 | luaunit |
| `getupvalue` | 3 | luatodotviz, referencefinder |
| `isEventSeries` | 3 | gameconfig |
| `isLong` | 3 | lams |
| `isP2PTrack` | 3 | socialgoals |
| `isUsingMPH` | 3 | osd |
| `jsUris` | 3 | layoutsections |
| `lastpage` | 3 | driveclub_testhelpers, loadingscreen_test |
| `matching` | 3 | driveclub_testhelpers |
| `memUsageFunc` | 3 | profiling |
| `nextcamerafunc` | 3 | camerasequence |
| `onGoalAttempted` | 3 | socialgoals |
| `onGoalStarted` | 3 | socialgoals |
| `page` | 3 | model |
| `pageSize` | 3 | model |
| `playerCount` | 3 | driveclub_testhelpers |
| `position` | 3 | osd |
| `process_options` | 3 | luatodotviz |
| `queueFinished` | 3 | rewards |
| `raceLineStart` | 3 | socialgoals |
| `racingLineType` | 3 | socialgoals |
| `request` | 3 | dispatcher |
| `returns` | 3 | evomock |
| `rowSchema` | 3 | model |
| `runTestMethodName` | 3 | luaunit |
| `sec` | 3 | driveclub_testhelpers, guiutils |
| `set` | 3 | evomock |
| `showingRacingLines` | 3 | socialgoals |
| `singleplayer` | 3 | gameconfig |
| `sort` | 3 | activitystream, luaunit, socialgoals |
| `startCallback` | 3 | asyncqueue |
| `startClass` | 3 | junitresult, luaunit |
| `startSize` | 3 | profiling |
| `startTest` | 3 | junitresult, luaunit |
| `stat` | 3 | socialgoals, socialgoalsourcetour |
| `stopAllGoalsOnError` | 3 | socialgoalsourceegs |
| `success` | 3 | asyncqueue |
| `teammode` | 3 | drift, gameconfig |
| `tempassociations` | 3 | model |
| `timeOfDay` | 3 | gameconfig |
| `timingFunc` | 3 | profiling |
| `tourObjectiveTranslated` | 3 | socialgoalsourcetour |
| `trackInfrastructureLevel` | 3 | gameconfig |
| `useDevTimes` | 3 | socialgoalsourceegs |
| `usedTrackPassContext` | 3 | gameconfig |
| `warning` | 3 | driveclub_testhelpers |
| `what` | 3 | profiling |
| `Active` | 2 | egsconstants |
| `Add` | 2 | rewards |
| `AddActor` | 2 | socialgoals |
| `AppendMarkup` | 2 | layoutsections, templater |
| `AssignObjectSnapshotIds` | 2 | memwatcher |
| `AwardFinishPositionXPandCash` | 2 | driveractions, rewards |
| `Boolean` | 2 | schema |
| `BroadcastVehicleChoice` | 2 | online_test, party_test |
| `CleanLap` | 2 | driveractions, egsconstants |
| `CleanRace` | 2 | driveractions, egsconstants |
| `CleanSplit` | 2 | driveractions, egsconstants |
| `CleanSplitPassedThisFrame` | 2 | driveractions |
| `CollectLuaTraverseObjects` | 2 | memwatcher |
| `Common_ClearTimersOnExit` | 2 | osd |
| `Common_DisplayDiffTimeToActorAhead` | 2 | osd |
| `Common_DisplayDiffTimeToActorBehind` | 2 | osd |
| `Common_Reset` | 2 | osd |
| `CompletedLaps` | 2 | driveclub_testhelpers |
| `Construct` | 2 | driveclub_testhelpers |
| `Cornering` | 2 | egsconstants, socialgoals |
| `CoroutineCall` | 2 | autotesthelpers, luaunit |
| `CrazySpeed` | 2 | driveractions, egsconstants |
| `CreatedBy` | 2 | challenge_test |
| `CurrentFrame` | 2 | autotesthelpers |
| `DecodeURI` | 2 | uriutils |
| `Deconstruct` | 2 | autotesthelpers, driveclub_testhelpers |
| `DefaultIsUsable` | 2 | socialgoalsourceegs |
| `DeferredActionCreation` | 2 | driveractions |
| `Deleted` | 2 | egsconstants |
| `Dispatch` | 2 | dispatcher |
| `Drafting` | 2 | driveractions, egsconstants |
| `DriftScore` | 2 | driveractions, egsconstants |
| `Dump` | 2 | activitystream |
| `ElapsedTime` | 2 | guiutils |
| `EncodeURIComponent` | 2 | uriutils |
| `EndTimeTrial` | 2 | driveclub_testhelpers, timetrial_test |
| `Entry` | 2 | activitystream |
| `EnvironmentCollision` | 2 | driveractions, egsconstants |
| `EscapeJsString` | 2 | guiutils, templater |
| `EvaluateGoal` | 2 | socialgoals |
| `EventAward` | 2 | egsconstants, rewards |
| `ExpectCall` | 2 | evomock |
| `ExpectGet` | 2 | evomock |
| `Fame` | 2 | egsconstants |
| `FinaliseQueuedRewards` | 2 | driveractions, rewards |
| `FindNextGoalFromPoslen` | 2 | socialgoals |
| `FormatAsJsArray` | 2 | activitystream |
| `GetActorId` | 2 | camerasequence, drift |
| `GetCompleteSeasonData` | 2 | driveclub_testhelpers |
| `GetEventListID` | 2 | gameconfig, socialgoals |
| `GetFailMessage` | 2 | socialgoals |
| `GetFinishFameForPosition` | 2 | driveractions, gameconfig |
| `GetGameArea` | 2 | gameconfig, socialgoals |
| `GetIsDriveClubLiveEvent` | 2 | gameconfig |
| `GetIsFreeplayEvent` | 2 | gameconfig |
| `GetMission` | 2 | gameconfig, socialgoals |
| `GetNumberOfLapsCompleted` | 2 | driveclub_testhelpers, laptime_test |
| `GetPermittedVehiclesList` | 2 | online_test, party_test |
| `GetSeasonData` | 2 | driveclub_testhelpers |
| `GetSuccessMessage` | 2 | socialgoals |
| `GetTotalStarsAvailable` | 2 | tour_test |
| `GetTotalStarsWon` | 2 | tour_test |
| `GetUriQuery` | 2 | dispatcher, uriutils |
| `GetUrl` | 2 | activitystream |
| `GetVehicle` | 2 | gameconfig, socialgoals |
| `GiveFinishPositionRewards` | 2 | driveractions |
| `GiveFirstInWorldTarget` | 2 | socialgoalsourceegs |
| `GiveGoalTargetFromEGS` | 2 | socialgoalsourceegs |
| `GiveInvalidScores` | 2 | socialgoalsourceegs |
| `HasProperty` | 2 | driveclub_testhelpers |
| `Hot_Hatch` | 2 | crazyspeed, drift |
| `Hyper` | 2 | crazyspeed, drift |
| `In` | 2 | schema |
| `Inactive` | 2 | egsconstants |
| `InsertParameters` | 2 | lams |
| `IsSingleplayer` | 2 | gameconfig |
| `IsTargetSane` | 2 | socialgoalsourceegs |
| `LoadLanguage` | 2 | lams |
| `MakeRoomForNewSnapshot` | 2 | memwatcher |
| `Merge` | 2 | layoutsections, templater |
| `Number` | 2 | schema |
| `OffTrack` | 2 | driveractions, egsconstants |
| `OnGoalAttempted` | 2 | socialgoalsourceegs |
| `Or` | 2 | schema |
| `Parallel` | 2 | async, driveractions |
| `ParseArgs` | 2 | driveclub_testhelpers |
| `ParseUriRequest` | 2 | dispatcher, uriutils |
| `PauseTillLoadingComplete` | 2 | challenge_test, driveclub_testhelpers |
| `Pending` | 2 | egsconstants |
| `Performance` | 2 | crazyspeed, drift |
| `Personal` | 2 | egsconstants, guiutils |
| `PopToPage` | 2 | osd |
| `Post` | 2 | autotesthelpers, socialgoals |
| `Profiler` | 2 | profiling |
| `RTTEnabled` | 2 | driveclub_testhelpers, guiphotos_tod |
| `Range` | 2 | schema |
| `RecordRequest` | 2 | dispatcher |
| `Requested` | 2 | egsconstants |
| `Reset` | 2 | lams |
| `RestartEvent` | 2 | driveclub_testhelpers, restart_test |
| `SaveGraphToSVGBatFile` | 2 | memwatcher |
| `SaveObjectList` | 2 | memwatcher |
| `SaveSnapshotInfo` | 2 | memwatcher |
| `SentBy` | 2 | challenge_test |
| `SetCountdownTimer` | 2 | countdownhelper |
| `SetNextSocialGoal` | 2 | socialgoals |
| `SetOverride` | 2 | dispatcher |
| `SetRacingLinesShown` | 2 | socialgoals |
| `Setup` | 2 | driveractions, rewards |
| `SetupGrid` | 2 | driveclub_testhelpers, pseudorandom_race_test |
| `SetupWeather` | 2 | alltracksprofile_race_test, driveclub_testhelpers |
| `ShallowCopyTable` | 2 | memwatcher, tableutils |
| `Shutdown` | 2 | countdownhelper, socialgoals |
| `SingleChallenges` | 2 | challenge_test |
| `SingleDrift` | 2 | egsconstants, socialgoals |
| `SkipAttractVideo` | 2 | driveclub_testhelpers |
| `SocialGoal` | 2 | egsconstants, rewards |
| `SortGoalsByZone` | 2 | socialgoals |
| `SplitCornering` | 2 | driveractions, egsconstants |
| `Sports` | 2 | crazyspeed, drift |
| `Star` | 2 | egsconstants |
| `StartGoal` | 2 | socialgoals |
| `Stoppie` | 2 | driveractions, egsconstants |
| `StringContains` | 2 | schema |
| `StringPattern` | 2 | schema |
| `Super` | 2 | crazyspeed, drift |
| `Superbike` | 2 | crazyspeed, drift |
| `TimeTrial_ApplySplitColour` | 2 | osd |
| `TimeTrial_ResetSplitTimers` | 2 | osd |
| `TimeTrial_UpdateDiffTime` | 2 | osd |
| `Times` | 2 | evomock |
| `TopSpeedThreshold` | 2 | crazyspeed, driveractions |
| `TotalActorToActorCollisions` | 2 | driveractions |
| `TotalActorToWorldCollisions` | 2 | driveractions |
| `TranslateGoalTypes` | 2 | socialgoals |
| `Traverse` | 2 | memwatcher, referencefinder |
| `UniquePosition` | 2 | driveractions, egsconstants |
| `UpdateActorPositionInGoals` | 2 | socialgoals |
| `UpdateDeferredShowingGoals` | 2 | socialgoals |
| `UpdateGoalScore` | 2 | socialgoalsourceegs |
| `User` | 2 | guiconstants, socialgoalsourceegs |
| `Valid` | 2 | challenge_test |
| `ValidateData` | 2 | model |
| `Vehicle` | 2 | driveclub_testhelpers |
| `VehicleCollision` | 2 | driveractions, egsconstants |
| `WaitForIsFrontEnd` | 2 | driveclub_testhelpers |
| `Wheelie` | 2 | driveractions, egsconstants |
| `_AddExtraConditionsToQuery` | 2 | model |
| `_AddJoinsToQuery` | 2 | model |
| `_ControllerNameFromRequest` | 2 | dispatcher |
| `_ExpandQuery` | 2 | model |
| `_FixLocalJoins` | 2 | model |
| `_FixSaveData` | 2 | model |
| `_HandleViewResponse` | 2 | dispatcher |
| `_ViewFilenameFromRequest` | 2 | dispatcher |
| `__newindex` | 2 | evomock, schema |
| `_started` | 2 | async |
| `_succeeded` | 2 | async |
| `_timestamp` | 2 | class |
| `abbrev` | 2 | luatodotviz |
| `actionId` | 2 | driveractions |
| `active` | 2 | driveclub_testhelpers |
| `allowRetry` | 2 | gameconfig |
| `arrowtail` | 2 | luatodotviz |
| `awardFunc` | 2 | socialgoals |
| `canchallenge` | 2 | gameconfig |
| `car_demo_level` | 2 | weatherpresetlookup |
| `challengePlayerGhostId` | 2 | gameconfig |
| `challengePlayerScore` | 2 | gameconfig |
| `challengeTargetAccountId` | 2 | gameconfig |
| `challengeTargetAccountName` | 2 | gameconfig |
| `challengeTargetDisplayName` | 2 | gameconfig |
| `challengeTargetGhostId` | 2 | gameconfig |
| `challengeTargetScore` | 2 | gameconfig |
| `challengeid` | 2 | gameconfig |
| `challengetype` | 2 | gameconfig |
| `condition` | 2 | socialgoals |
| `content` | 2 | layoutsections |
| `context` | 2 | socialgoals, socialgoalsourcetour |
| `customRetryUrl` | 2 | gameconfig |
| `debugName` | 2 | socialgoals |
| `defaultAvatarUrl` | 2 | guiconstants, socialgoalsourceegs |
| `defaultName` | 2 | guiconstants |
| `displayClassName` | 2 | luaunit |
| `displayFailedTests` | 2 | luaunit |
| `displayFailure` | 2 | luaunit |
| `displayOneFailedTest` | 2 | luaunit |
| `displayProgressScreens` | 2 | gameconfig |
| `displaySuccess` | 2 | luaunit |
| `displayTestName` | 2 | luaunit |
| `echo` | 2 | templater |
| `egsGameID` | 2 | gameconfig |
| `envSeed` | 2 | gameconfig |
| `environment` | 2 | gameconfig |
| `eventID` | 2 | gameconfig |
| `eventLams` | 2 | gameconfig |
| `eventListID` | 2 | gameconfig |
| `event_type` | 2 | autotesthelpers, socialgoals |
| `eventlistname` | 2 | gameconfig |
| `eventlisttype` | 2 | gameconfig |
| `events` | 2 | driveclub_testhelpers |
| `extraFindConditions` | 2 | model |
| `f` | 2 | referencefinder |
| `faceOffs` | 2 | gameconfig |
| `fadein` | 2 | camerasequence |
| `finalised` | 2 | rewards |
| `findallpaths` | 2 | referencefinder |
| `finishPosFame` | 2 | gameconfig |
| `free` | 2 | driveclub_testhelpers, guiphotos_tod |
| `garageVehicle` | 2 | gameconfig |
| `garageWorld` | 2 | gameconfig |
| `getenv` | 2 | driveclub_testhelpers, framework |
| `getlocal` | 2 | referencefinder |
| `gettemplate` | 2 | templater |
| `globalAIDifficulty` | 2 | gameconfig |
| `h` | 2 | referencefinder |
| `handler` | 2 | dispatcher |
| `hasObjectives` | 2 | gameconfig |
| `isDriftMode` | 2 | driveractions |
| `isDriveClubLiveEvent` | 2 | gameconfig |
| `isFreeplayEvent` | 2 | gameconfig |
| `isPointToPoint` | 2 | driveractions |
| `isempty` | 2 | referencefinder |
| `isocode` | 2 | lams |
| `json` | 2 | dispatcher |
| `lap2` | 2 | laptime_test |
| `lap3` | 2 | laptime_test |
| `layout` | 2 | dispatcher |
| `layoutSections` | 2 | dispatcher, templater |
| `liveTimers` | 2 | gameconfig |
| `makeAnAttr` | 2 | guiutils |
| `matchingTimers` | 2 | gameconfig |
| `maxSnapshots` | 2 | memwatcher |
| `metricUnits` | 2 | socialgoals |
| `mimeType` | 2 | dispatcher |
| `model_message` | 2 | model |
| `new` | 2 | referencefinder |
| `nextSlotID` | 2 | gameconfig |
| `nrCompetitors` | 2 | gameconfig |
| `params` | 2 | evomock |
| `path` | 2 | framework |
| `players` | 2 | driveclub_testhelpers |
| `pop` | 2 | referencefinder |
| `postRacePage` | 2 | gameconfig |
| `precipitationType` | 2 | gameconfig |
| `presetTargetScore` | 2 | gameconfig |
| `push` | 2 | referencefinder |
| `race` | 2 | egsconstants, gamecomponents |
| `registration` | 2 | driveclub_testhelpers |
| `retry` | 2 | async |
| `reverse` | 2 | guiutils |
| `run` | 2 | autotesthelpers, luaunit |
| `runTestMethod` | 2 | luaunit |
| `season` | 2 | driveclub_testhelpers |
| `seedPlayers` | 2 | socialgoals, socialgoalsourceegs |
| `setUp` | 2 | luaunit |
| `setcamfunc` | 2 | camerasequence |
| `sethook` | 2 | profiling |
| `sprintVariant` | 2 | gameconfig |
| `standardiseString` | 2 | guiutils |
| `startTime` | 2 | profiling |
| `statId` | 2 | socialgoalsourceegs |
| `stdout` | 2 | luaunit |
| `stream_entry_action` | 2 | activitystream |
| `strip_luaunit_stack` | 2 | luaunit |
| `strsplit` | 2 | luaunit |
| `switch` | 2 | guiutils |
| `targetPlayer` | 2 | socialgoals |
| `tearDown` | 2 | luaunit |
| `template` | 2 | templater |
| `templateID` | 2 | gameconfig |
| `testName` | 2 | autotesthelpers |
| `testracesoak` | 2 | random_vehicle_track_mode_test, soak_test |
| `timeMultiplier` | 2 | gameconfig |
| `timeTrialDuration` | 2 | gameconfig |
| `timestamp` | 2 | class |
| `timetrial` | 2 | egsconstants, gamecomponents |
| `title` | 2 | dispatcher |
| `trackID` | 2 | gameconfig |
| `trackInstances` | 2 | driveractions |
| `track_type` | 2 | laptime_test |
| `updateContinually` | 2 | driveractions |
| `v` | 2 | referencefinder |
| `visited` | 2 | referencefinder |
| `warnExcLevel` | 2 | profiling |
| `warnExcTime` | 2 | profiling |
| `warnIncLevel` | 2 | profiling |
| `warnIncTime` | 2 | profiling |
| `weatherType` | 2 | gameconfig |
| `write_edges` | 2 | luatodotviz |
| `write_nodes` | 2 | luatodotviz |
| `xpCalcFunc` | 2 | driveractions |
| `yield` | 2 | autotesthelpers, luaunit |
| `zoneId` | 2 | socialgoals |
