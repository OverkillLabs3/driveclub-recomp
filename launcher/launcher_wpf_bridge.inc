// File-based IPC for the WPF shell. Settings/patch selection and launch remain native.
using Json = nlohmann::json;
void WriteJsonFile(const std::wstring& path, const Json& value) {
  const auto temp=path+L".tmp";
  {std::ofstream out(temp.c_str(),std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("Cannot write UI response");out<<value.dump(2);}
  if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot publish UI response");
}
void WriteExternalProgress(int percent,const std::wstring& text) {
  if(g_external_progress.empty())return;
  try{WriteJsonFile(g_external_progress,{{"Percent",percent},{"Text",Narrow(text)},{"GameProcessId",g_started_game_process_id}});}catch(...){}
}
// Cache both optional startup features in one streaming pass per backend process.
struct RuntimeFeatures { bool camera=false; bool startup=false; };
const RuntimeFeatures& GetRuntimeFeatures() {
  static const RuntimeFeatures features=[] {
    RuntimeFeatures result;
    std::ifstream file(GameExe().c_str(),std::ios::binary);
    const std::string camera="CameraPatch: experimental high-FPS camera/replay workaround enabled";
    const std::string startup="LauncherReady: first game frame submitted";
    const size_t overlap=(std::max)(camera.size(),startup.size())-1;
    std::string buffer(65536+overlap, '\0'); size_t keep=0;
    while(file) {
      file.read(buffer.data()+keep,65536); const size_t count=keep+size_t(file.gcount());
      const auto contains=[&](const std::string& marker) {
        return std::search(buffer.begin(),buffer.begin()+count,marker.begin(),marker.end())!=buffer.begin()+count;
      };
      result.camera=result.camera || contains(camera);
      result.startup=result.startup || contains(startup);
      if(result.camera && result.startup)break;
      keep=(std::min)(count,overlap);
      std::memmove(buffer.data(),buffer.data()+count-keep,keep);
    }
    return result;
  }();
  return features;
}
bool CameraPatchAvailable() { return GetRuntimeFeatures().camera; }
std::wstring UiInstalledVersion() {
  const auto lines = ReadLines(g_home + L"\\version.txt");
  if (lines.empty()) return {};
  auto value = Widen(lines[0]);
  while (!value.empty() && iswspace(value.back())) value.pop_back();
  const auto first = value.find_first_not_of(L" \t\r\n");
  return first == std::wstring::npos ? L"" : value.substr(first);
}
bool UiReleaseVersion(std::wstring value) {
  if (!value.empty() && (value[0] == L'v' || value[0] == L'V')) value.erase(0, 1);
  int parts = 0, digits = 0;
  for (wchar_t c : value) {
    if (c >= L'0' && c <= L'9') { if (++digits > 8) return false; }
    else if (c == L'.' && digits) { ++parts; digits = 0; }
    else return false;
  }
  return digits && parts >= 1 && parts <= 3;
}
std::wstring g_ui_update_version, g_ui_update_note;
bool g_ui_capture_cancelled = false, g_ui_restart_required = false;
Json UiState(const std::wstring& error=L"") {
  Json controls=Json::array(),cards=Json::array();
  for(size_t i=0;i<g_c.size();++i){
    const auto& c=g_c[i];Json options=Json::array();int selected=c.sel;
    if(c.kind==Kind::Slider) selected=(std::max)(c.min,(std::min)(c.max,selected));
    std::string kind=c.kind==Kind::Toggle?"Toggle":c.kind==Kind::Action?"Action":c.kind==Kind::Slider?"Slider":c.kind==Kind::Button?"Button":c.kind==Kind::Binding?"Binding":c.kind==Kind::Info?"Info":c.kind==Kind::Monitor?"Monitor":(c.patches[0]=="@name"||c.patches[0]=="@onhost")?"Text":"Choice";    if(int(i)==g_lang){for(auto* n:kLangName)options.push_back(Narrow(n));selected=g_lang_id;}
    else if(int(i)==g_btrack){for(const auto& x:kBenchTracks)options.push_back(Narrow(std::wstring(x.name)+L" ("+x.group+L")"));selected=g_bench_track;}
    else if(int(i)==g_bcar){for(const auto& x:kBenchCars)options.push_back(Narrow(x.name));selected=g_bench_car;}
    else for(const auto& x:c.items)options.push_back(Narrow(x));
    const bool listed=std::any_of(g_cards.begin(),g_cards.end(),[&](const Card& card){return std::find(card.ctls.begin(),card.ctls.end(),int(i))!=card.ctls.end();});
    controls.push_back({{"Id",i},{"Label",Narrow(c.label)},{"Hint",Narrow(int(i)==g_camera_patch_row && !CameraPatchAvailable() ? L"This runtime does not expose the temporary camera/replay workaround. It may be unnecessary if native timing is already corrected." : int(i)==g_motion_blur ? MotionBlurHint(c.sel) : c.hint)},{"Kind",kind},{"Tag",c.patches[0]},
      {"Minimum",c.min},{"Maximum",c.max},{"Unit",Narrow(c.unit)},{"Hidden",!listed || RowHidden(c)},
      {"Enabled",!BlurRowLocked(c) && (int(i)!=g_camera_patch_row || CameraPatchAvailable())},
      {"Options",options},{"Selected",selected},{"Text",c.patches[0]=="@name"?Narrow(g_name):c.patches[0]=="@onhost"?Narrow(g_online_host):""},      {"Details",c.more?Narrow(c.more):""}});
  }
  // The native model owns category/card membership, reset scopes and Driving subpages.
  for(const auto& c:g_cards) {
    cards.push_back({{"Title",Narrow(c.title)},{"Page",c.tab},{"Subpage",c.sub},
      {"Controls",c.ctls},{"Note",c.note?Narrow(c.note):""}});
  }

  return {{"Version",3},{"GameProcessId",g_started_game_process_id},{"Controls",controls},{"Cards",cards},{"UpdatePath",Narrow(g_folder[0].path)},{"BasePath",Narrow(g_folder[1].path)},
    {"FoldersOk",FoldersOk()},{"FolderProblem",Narrow(!FolderProblem(0).empty()?FolderProblem(0):FolderProblem(1))},
    {"HardwareProblem",Narrow(g_gpu_problem)},{"RuntimeAvailable",Exists(GameExe())},{"StartupSignalSupported",GetRuntimeFeatures().startup},{"Error",Narrow(error)},
    {"QuickControls",{g_preset,g_fps,g_outres,g_aa}},{"PresetId",g_preset},
    {"BenchmarkFps",Narrow(g_bench_result.fps)},{"BenchmarkLows",Narrow(g_bench_result.lows)},{"BenchmarkAbout",Narrow(g_bench_result.about)},
    {"BenchmarkRanSummary",Narrow(g_bench_ran_short)},{"BenchmarkRanDetails",[&]{Json lines=Json::array();for(const auto& line:g_bench_ran_lines)lines.push_back(Narrow(line));return lines;}()},
    {"Message",Narrow(g_status)},{"CaptureCancelled",g_ui_capture_cancelled},{"XboxNames",g_xbox_names},
    {"ReadSlot",g_read_slot},
    {"InstalledVersion",Narrow(UiInstalledVersion())},{"UpdateVersion",Narrow(g_ui_update_version)},
    {"UpdateNote",Narrow(g_ui_update_note)},{"RestartRequired",g_ui_restart_required}};
}
void ImportUiState(const Json& input) {
  g_folder[0].path=Widen(input.value("UpdatePath",Narrow(g_folder[0].path)));
  g_folder[1].path=Widen(input.value("BasePath",Narrow(g_folder[1].path)));
  for(const auto& item:input.at("Controls")){
    const int id=item.at("Id").get<int>();if(id<0||id>=int(g_c.size()))throw std::runtime_error("Invalid control ID");
    auto& c=g_c[id];const int selected=item.value("Selected",c.sel);
    if(c.kind==Kind::Action || c.kind==Kind::Button || c.kind==Kind::Info || c.kind==Kind::Monitor)continue;
    if(c.patches[0]=="@name"){
      const std::string name=item.value("Text",Narrow(g_name));
      if(name.empty()||name.size()>20||std::any_of(name.begin(),name.end(),[](unsigned char ch){return ch<32||ch>126;}))throw std::runtime_error("Player name must contain 1-20 printable Latin characters");
      g_name=Widen(name);c.items[0]=g_name;
    }else if(c.patches[0]=="@onhost"){
      const std::string host=item.value("Text",Narrow(g_online_host));
      if(host.size()>15||std::any_of(host.begin(),host.end(),[](unsigned char ch){return !(ch=='.'||(ch>='0'&&ch<='9'));}))throw std::runtime_error("Host address must be an IPv4 address");
      g_online_host=Widen(host);c.items[0]=g_online_host.empty()?L"(not set)":g_online_host;
    }else if(id==g_lang){if(selected<0||selected>=21)throw std::runtime_error("Invalid language");g_lang_id=selected;c.items[0]=kLangName[selected];}
    else if(id==g_btrack){if(selected<0||selected>=kBenchTrackCount)throw std::runtime_error("Invalid track");g_bench_track=selected;}
    else if(id==g_bcar){if(selected<0||selected>=kBenchCarCount)throw std::runtime_error("Invalid car");g_bench_car=selected;}
    else if(c.kind==Kind::Slider){c.sel=(std::max)(c.min,(std::min)(c.max,selected));}
    else {const int count=c.kind==Kind::Toggle?2:int(c.items.size());if(selected<((id==g_drive_preset||id==g_ffb_preset)?-1:0)||selected>=count)throw std::runtime_error("Invalid setting value");c.sel=selected;}
    if(c.patches[0]=="@skipgpu")g_skip_gpu_check=c.sel!=0;
  }
  NameNativeUpscale();ShowBenchChoices();CheckFolders();
  // Preserve the displayed check result across short-lived IPC workers. Launch
  // always performs its own hardware check, never trusting this UI-only value.
  g_gpu_problem = g_skip_gpu_check ? L"" : Widen(input.value("HardwareProblem",std::string()));
}
int RunWpfBridge(int argc,wchar_t** argv) {
  if(argc<3||wcsncmp(argv[1],L"--ui-",5)!=0)return -1;
  const std::wstring command=argv[1];
  const std::wstring output=argv[argc-1];
  try{
    if(command==L"--ui-state"){
      if(!g_skip_gpu_check){g_gpu_problem=CheckCpu();if(g_gpu_problem.empty())g_gpu_problem=CheckGpu();}
    }else if(argc>=4){
      std::ifstream in(argv[2],std::ios::binary);const Json request=Json::parse(in);ImportUiState(request);
      if(command==L"--ui-reset-page"){
        const int page=request.value("ResetPage",-2), sub=request.value("ResetSubpage",-1);
        if(page < -1 || page >= kTabCount || (page==kTabDriving && (sub<0 || sub>=kSubCount)))
          throw std::runtime_error("Invalid reset page");
        std::vector<int> ids;
        if(page==-1)ids={g_preset,g_fps,g_outres,g_aa};
        else for(const auto& card:g_cards)if(card.tab==page && (page!=kTabDriving || card.sub==sub))
          ids.insert(ids.end(),card.ctls.begin(),card.ctls.end());
        for(int id:ids){
          auto& c=g_c[id];
          if(c.kind==Kind::Action || c.kind==Kind::Button || c.kind==Kind::Info || c.kind==Kind::Monitor || c.patches[0]=="@name" || c.patches[0]=="@onhost" || id==g_lang)continue;          c.sel=id==g_camera_patch_row ? (FrameModeNeedsCameraPatch()?1:0) : c.def;
          if(id==g_btrack)g_bench_track=0;
          if(id==g_bcar)g_bench_car=kBenchDefaultCar;
          if(c.patches[0]=="@skipgpu")g_skip_gpu_check=c.sel!=0;
        }
        // Controller and keyboard: "Reset this page" puts every binding back on the built-in template.
        if(page==kTabDriving && sub==kSubInput){
          if(!WriteFullTemplate()) throw std::runtime_error("Could not write the default bindings to default.ini");
          RefreshBindingChips();
        }
        DerivePreset();
        if(page==kTabDriving && sub==kSubAssists)DeriveDrivePreset();
        if(page==kTabDriving && sub==kSubWheel)DeriveWheelPreset();
        NameNativeUpscale();ShowBenchChoices();
      }
      else if(command==L"--ui-validate"){
        const int changed=request.value("ChangedId",-1);
        if(changed==g_l3r3_row && changed>=0){
          g_l3r3_touchpad=g_c[size_t(changed)].sel!=0;
          WriteL3R3Setting(g_l3r3_touchpad);
        }

        if(changed==g_fps)ApplyFrameModeCameraDefault();
        if(changed==g_wide && g_c[g_wide].sel>0)g_c[g_outres].sel=g_c[g_inres].sel=0;
        DerivePreset();DeriveDrivePreset();DeriveWheelPreset();
        if(changed>=0 && changed<int(g_c.size()) && g_c[changed].patches[0]=="@skipgpu") {
          g_gpu_problem=g_skip_gpu_check?L"":CheckCpu();
          if(!g_skip_gpu_check && g_gpu_problem.empty())g_gpu_problem=CheckGpu();
        }
      }
      else if(command==L"--ui-preset")ApplyPreset(g_c[g_preset].sel);
      else if(command==L"--ui-drive-preset"){if(g_c[g_drive_preset].sel>=0)ApplyDrivePreset(g_c[g_drive_preset].sel);}
      else if(command==L"--ui-wheel-preset"){if(g_c[g_ffb_preset].sel>=0)ApplyWheelPreset(g_c[g_ffb_preset].sel);}
      else if(command==L"--ui-usedevice"){
        const int slot=request.value("DeviceSlot",-1);
        if(slot < -1 || slot > 3 || (slot >= 0 && !XInputRead(slot,new unsigned short(0))))
          throw std::runtime_error("That device is not connected");
        g_read_slot=slot;                      // remembered once the saved choice was read (see SyncDevices)
        g_xbox_names=DetectXboxNaming();       // the pad's kind names the buttons and picks the template
        const bool filled=WritePadTemplate();  // double-click: the pad half only, the keys stay the player's
        if(filled) RefreshBindingChips();
        SyncDevices(false);                    // the red name and the monitor follow the pad just chosen
        g_status=g_read_slot<0?L"Reading the first connected pad.":L"Reading the pad in XInput slot "+std::to_wstring(g_read_slot+1)+L".";
        if(filled) g_status+=g_xbox_names?L" The Xbox pad template was filled in.":L" The PlayStation pad template was filled in.";
        g_status_color=kOk;
        g_status_sticky=true;
      }

      else if(command==L"--ui-wheelsetup"){
        OpenWheelSetup();
        if(g_status_color.GetValue()==kBad.GetValue())throw std::runtime_error(Narrow(g_status));
      }
      else if(command==L"--ui-savebackup" || command==L"--ui-saverestore"){
        std::wstring message;
        const bool ok=RunSaveButton(command==L"--ui-saverestore",message);
        g_status=message;g_status_color=ok?kOk:kBad;
        if(!ok)throw std::runtime_error(Narrow(message));
      }
      else if(command==L"--ui-check-update" || command==L"--ui-install-update") {
        const auto current=UiInstalledVersion();
        const bool enabled=std::any_of(g_c.begin(),g_c.end(),[](const Ctl& c){return c.patches[0]=="@updatecheck" && c.sel!=0;});
        if(command==L"--ui-check-update")Updater::CleanUp(g_home);
        if(!UiReleaseVersion(current)) {
          g_ui_update_note=current.empty()?L"The installed version is unknown; automatic release updates are unavailable.":
            L"Development build ("+current+L"). Automatic replacement with a release is disabled.";
          if(command==L"--ui-install-update")throw std::runtime_error(Narrow(g_ui_update_note));
        } else if(enabled || command==L"--ui-install-update") {
          Updater::Release release;std::string why;
          const auto stable=current[0]==L'v'||current[0]==L'V'?current.substr(1):current;
          if(Updater::CheckNewer(stable,release,why)) {
            g_ui_update_version=release.version;
            if(command==L"--ui-install-update") {
              if(request.value("UpdateVersion",std::string())!=Narrow(release.version))throw std::runtime_error("The available release changed. Check for updates again.");
              const auto pid=request.value("ShellProcessId",0ul);
              g_external_progress=output+L".progress";
              std::wstring error;
              if(!Updater::InstallForShell(release,g_home,pid,[](int p){SetProgress(p,L"Downloading and verifying update...");},error))throw std::runtime_error(Narrow(error));
              g_ui_restart_required=true;
            }
          } else {
            g_ui_update_note=Widen(why);
            if(command==L"--ui-install-update")throw std::runtime_error(why.empty()?"No newer release is available.":why);
          }
        }
      }
      else if(command==L"--ui-monitor"){
        for(auto& c:g_c) if(c.kind==Kind::Monitor) c.items[0]=MonitorText();
      }
      else if(command==L"--ui-capture"){
        const int id=request.value("Id",request.value("CaptureId",-1));
        if(id<0 || id>=int(g_c.size()) || g_c[id].kind!=Kind::Binding)
          throw std::runtime_error("Not a binding row");
        const int side=request.value("CaptureSide",0);
        if(side!=0 && side!=1) throw std::runtime_error("Invalid capture side");
        if(side==1 && BindPadLocked(g_c[id]))
          throw std::runtime_error("That control is read as an axis and cannot be remapped.");
        std::string value;
        if(side==1){
          const char* token=CapturePadToken(15000,output+L".control");
          if(!token){
            g_ui_capture_cancelled=true;
            g_status=ReadLines(output+L".control").empty()?L"No controller button was pressed.":L"Binding unchanged.";
          }
          else if(!ApplyBinding(id,1,token)) throw std::runtime_error("Could not save the binding to default.ini");
        }
        else {
          if(!request.value("CaptureClear",false)) value=CaptureKey(15000, output+L".control");
          if(value.empty()){ g_ui_capture_cancelled=true; g_status=L"Binding unchanged."; }
          else if(!ApplyBinding(id,side,value)) throw std::runtime_error("Could not save the binding to default.ini");
        }
      }
      else if(command==L"--ui-ffbgainup" || command==L"--ui-ffbgaindown"){
        // Reuse the native wheel capture without opening the native launcher.
        HWND capture=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!capture)throw std::runtime_error("Cannot start wheel button capture");
        const int which=command==L"--ui-ffbgainup"?0:1;
        if(request.value("CaptureClear",false))SetFfbGainButton(capture,which,-1);
        else StartFfbCapture(capture,which);
        const auto until=GetTickCount64()+15000;
        while(g_ffb_capture>=0 && GetTickCount64()<until){
          const auto commands=ReadLines(output+L".control");
          if(!commands.empty() && commands[0]=="cancel") {
            StopFfbCapture(capture);g_ui_capture_cancelled=true;g_status=L"Wheel button unchanged.";break;
          }
          if(!commands.empty() && commands[0]=="clear") {SetFfbGainButton(capture,which,-1);break;}
          PollFfbCapture(capture);Sleep(30);
        }
        if(g_ffb_capture>=0){StopFfbCapture(capture);g_status=L"No new wheel button pressed within 15 seconds.";g_status_color=kWarn;}
        DestroyWindow(capture);
        if(!g_ui_capture_cancelled && g_status_color.GetValue()!=kOk.GetValue())throw std::runtime_error(Narrow(g_status));
      }
      else if(command==L"--ui-apply"||command==L"--ui-run"||command==L"--ui-benchmark"){
        DerivePreset();DeriveDrivePreset();DeriveWheelPreset();
        if(!SaveSettings()) throw std::runtime_error(Narrow(g_settings_error));
        if(command!=L"--ui-apply"){
          if(!FoldersOk()){WriteJsonFile(output,UiState(L"Choose valid game folders on Play."));return 1;}
          if(!g_skip_gpu_check){g_gpu_problem=CheckCpu();if(g_gpu_problem.empty())g_gpu_problem=CheckGpu();}
          if(GpuBlocked()){WriteJsonFile(output,UiState(g_gpu_problem));return 1;}
          g_external_progress=output+L".progress";StartLauncherLog();
          if(command==L"--ui-benchmark")BenchWorker();
          else {
            SetProgress(0,L"Preparing the game...");std::wstring error=Preflight();
            if(error.empty())error=RunPrepare();
            if(error.empty()){SetProgress(100,L"Starting DRIVECLUB...");error=StartGame();}
            WriteJsonFile(output,UiState(error));return error.empty()?0:1;
          }
          LoadBenchResult();WriteJsonFile(output,UiState(g_status_color.GetValue()==kWarn.GetValue()?g_status:L""));return 0;
        }
      }else throw std::runtime_error("Unknown UI command");
    }else throw std::runtime_error("Missing UI request");
    WriteJsonFile(output,UiState());return 0;
  }catch(const std::exception& ex){
    try{WriteJsonFile(output,{{"Error",ex.what()}});}catch(...){}
    return 1;
  }
}
