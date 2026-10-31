#include "NativeUI.hpp"
#include "App.hpp"
#include "SDKExplorer.hpp"
#include "Utils/Logger.hpp"
#include "Android_draw/draw.h"
#include "Android_Graphics/GraphicsManager.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace {
int language=0,selectedPid=0;
bool selectExplorer=false,selectWorld=false;
App::json processes=App::json::array();
App::json snapshot;
std::chrono::steady_clock::time_point nextPoll{};
const char* Tr(const char* zh,const char* en) {return language?en:zh;}
void Refresh() {
    processes=App::Processes();
    bool exists=false;
    for(const auto& p:processes) exists|=p["pid"].get<int>()==selectedPid;
    if(!exists) {
        selectedPid=processes.empty()?0:processes.front()["pid"].get<int>();
        const int active=App::State().value("pid",0);
        for(const auto& p:processes) if(p["pid"]==active) selectedPid=active;
    }
}
void Command(const char* action) {
    auto result=App::Action({{"action",action},{"pid",selectedPid}});
    nextPoll={};
    if(!result.value("accepted",false)) LOGW("%s",result.value("error",std::string{}).c_str());
}
void Text(const App::json& value) {const auto s=value.is_string()?value.get<std::string>():value.dump();ImGui::TextUnformatted(s.c_str());}
void Status(bool found) {
    ImGui::TextColored(found?ImVec4(.32f,.84f,.64f,1):ImVec4(.95f,.57f,.36f,1),"%s",found?Tr("已识别","Found"):Tr("未知","Unknown"));
}
void Offsets(const App::json& state) {
    if(ImGui::BeginTable("offsets",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp)) {
        for(auto h:{Tr("名称","Name"),Tr("地址","Address"),Tr("相对基址","RVA"),Tr("状态","Status")}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for(const auto& entry:state["offsets"]) {
            ImGui::TableNextRow();
            int c=0;for(auto key:{"name","value","relative"}) {ImGui::TableSetColumnIndex(c++);Text(entry[key]);}
            ImGui::TableSetColumnIndex(3);Status(entry.value("found",false));
        }
        ImGui::EndTable();
    }
}
void Structures(const App::json& state) {
    for(const auto& group:state["groups"]) {
        const auto name=group["name"].get<std::string>();
        if(!ImGui::CollapsingHeader(name.c_str(),ImGuiTreeNodeFlags_DefaultOpen)) continue;
        ImGui::PushID(name.c_str());
        if(ImGui::BeginTable("fields",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_SizingStretchProp)) {
            for(auto h:{Tr("字段","Field"),Tr("类型","Type"),Tr("偏移","Offset"),Tr("状态","Status"),Tr("说明","Notes")}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for(const auto& f:group["fields"]) {
                ImGui::TableNextRow();
                int c=0;for(auto key:{"name","type","offset"}) {ImGui::TableSetColumnIndex(c++);Text(f[key]);}
                ImGui::TableSetColumnIndex(3);Status(f.value("found",false));
                ImGui::TableSetColumnIndex(4);Text(f["description"]);
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}
}

void RenderAutoUEDumpPanel(bool* running) {
    static bool first=true;
    if(first) {Refresh();first=false;}
    const auto now=std::chrono::steady_clock::now();
    if(now>=nextPoll) {snapshot=App::State();nextPoll=now+std::chrono::milliseconds(100);}
    const auto& state=snapshot;
    const bool busy=state.value("busy",false),probed=state.value("probed",false);
    ImGui::TextColored(ImVec4(.35f,.78f,1,1),"UNREAL MEMORY TOOLS");
    ImGui::SameLine();ImGui::TextDisabled("1.0.1");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x-200);
    if(ImGui::SmallButton(language?"中文":"English")) {language=!language;SDKExplorer::SetLanguage(language);}
    ImGui::SameLine();if(ImGui::SmallButton(Tr("退出","Exit"))) *running=false;
    ImGui::Separator();
    ImGui::BeginDisabled(busy);
    const char* placeholder=Tr("选择 Unreal 进程","Select Unreal process");
    std::string selection=placeholder;
    for(const auto& p:processes) if(p["pid"]==selectedPid) selection=p["package"].get<std::string>()+"  ["+std::to_string(selectedPid)+"]";
    ImGui::SetNextItemWidth(std::max(280.0f,ImGui::GetContentRegionAvail().x-170.0f));
    if(ImGui::BeginCombo("##process",selection.c_str())) {
        for(const auto& p:processes) {
            const auto label=p["package"].get<std::string>()+"  ["+p["pid"].dump()+"]";
            if(ImGui::Selectable(label.c_str(),p["pid"]==selectedPid)) selectedPid=p["pid"].get<int>();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();if(ImGui::Button(Tr("刷新","Refresh"))) Refresh();
    ImGui::BeginDisabled(selectedPid<=0);
    if(ImGui::Button(Tr("1. 探测","1. Probe"))) Command("probe");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!probed || state.value("pid",0)!=selectedPid);
    if(ImGui::Button(Tr("2. 导出 SDK","2. Dump SDK"))) Command("dump");
    ImGui::SameLine();if(ImGui::Button(Tr("导出动态库","Dump Library"))) Command("library");
    ImGui::EndDisabled();ImGui::EndDisabled();
    ImGui::SameLine();Text(state["phase"]);
    if(!state.value("error",std::string{}).empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.48f,.40f,1));ImGui::TextWrapped("%s",state["error"].get<std::string>().c_str());ImGui::PopStyleColor();
    }
    if(busy) {
        ImGui::ProgressBar(state.value("objectsPercent",0)/100.0f,ImVec2(-1,22),Tr("对象","Objects"));
        ImGui::ProgressBar(state.value("dumpPercent",0)/100.0f,ImVec2(-1,22),Tr("生成 / 导出","Generate / Export"));
    }
    if(!state.value("profile",std::string{}).empty()) {ImGui::TextDisabled("%s | PID %d",state["profile"].get<std::string>().c_str(),state.value("pid",0));}
    ImGui::Separator();
    if(ImGui::BeginTabBar("content")) {
        if(ImGui::BeginTabItem(Tr("概览","Overview"))) {
            ImGui::BeginChild("overview");
            Offsets(state);
            ImGui::Spacing();
            if(!state["metrics"].empty()) {
                ImGui::TextDisabled("%s",Tr("性能统计","Performance"));
                for(auto it=state["metrics"].begin();it!=state["metrics"].end();++it) {
                    const auto value=it.value().dump();ImGui::Text("%s: %s",it.key().c_str(),value.c_str());
                }
            }
            const auto path=state.value("resultPath",std::string{});
            if(!path.empty()) ImGui::TextWrapped("%s: %s",Tr("输出","Output"),path.c_str());
            ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr("结构偏移","Structures"))) {
            ImGui::BeginChild("structures");Structures(state);ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr("SDK 对象浏览","SDK Explorer"),nullptr,selectExplorer?ImGuiTabItemFlags_SetSelected:0)) {
            selectExplorer=false;
            if(busy) ImGui::TextDisabled("%s",Tr("任务完成后可浏览对象","Explorer is available after the operation finishes."));
            else if(!probed || state.value("pid",0)!=selectedPid) ImGui::TextDisabled("%s",Tr("先完成探测","Probe a process first."));
            else App::RenderExplorer();
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr("世界对象列表","World Actors"),nullptr,selectWorld?ImGuiTabItemFlags_SetSelected:0)) {
            selectWorld=false;
            if(busy) ImGui::TextDisabled("%s",Tr("任务完成后可浏览对象","Wait for the current operation."));
            else if(!probed || state.value("pid",0)!=selectedPid) ImGui::TextDisabled("%s",Tr("先探测选中的进程","Probe the selected process first."));
            else App::RenderExplorer(true);
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr("日志","Logs"))) {
            if(ImGui::SmallButton(Tr("清空日志","Clear logs"))) Command("clear_logs");
            ImGui::BeginChild("logs",ImVec2(0,0),false,ImGuiWindowFlags_HorizontalScrollbar);
            const bool bottom=ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-10;
            ImGuiListClipper clipper;clipper.Begin(int(state["logs"].size()));
            while(clipper.Step()) for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) Text(state["logs"][i]);
            if(bottom) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(Tr("设置","Settings"))) {
            static int theme=1;
            ImGui::SetNextItemWidth(300);
            if(ImGui::Combo(Tr("主题","Theme"),&theme,Tr("白色主题\0蓝色主题\0紫色主题\0","Light\0Dark\0Classic\0"))) {
                if(theme==0) ImGui::StyleColorsLight();
                else if(theme==2) ImGui::StyleColorsClassic();
                else ImGui::StyleColorsDark();
            }
            ImGui::Text("%s: 曦曦(DreamFekk)",Tr("创作者","Author"));
            ImGui::TextUnformatted("https://github.com/DreamFekk");
            ImGui::TextWrapped("%s: %s",Tr("输出目录","Output"),state.value("outputRoot",std::string{}).c_str());
            ImGui::Text("%s: %s",Tr("渲染接口","Renderer"),graphics->RenderName);
            ImGui::Text("FPS: %.1f",ImGui::GetIO().Framerate);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

int RunNativeUI(bool openExplorer,bool openWorld) {
    selectWorld=openWorld;
    selectExplorer=openExplorer;
    graphics=GraphicsManager::getGraphicsInterface(GraphicsManager::VULKAN);
    if(!graphics) {LOGE("Cannot create graphics backend.");return 1;}
    screen_config();
    const int maximum=std::max(displayInfo.width,displayInfo.height);
    native_window_screen_x=maximum;native_window_screen_y=maximum;
    abs_ScreenX=maximum;abs_ScreenY=std::min(displayInfo.width,displayInfo.height);
    window=android::ANativeWindowCreator::Create("UnrealMemoryTools",maximum,maximum,permeate_record);
    if(!window) {LOGE("Cannot create Android window.");return 1;}
    if(!graphics->Init_Render(window,maximum,maximum)) {android::ANativeWindowCreator::Destroy(window);return 1;}
    Touch::Init({float(abs_ScreenX),float(abs_ScreenY)},false);
    Touch::setOrientation(displayInfo.orientation);
    init_My_drawdata();
    auto& style=ImGui::GetStyle();
    style.WindowPadding={24,20};style.FramePadding={14,10};style.ItemSpacing={12,10};
    style.WindowRounding=12;style.FrameRounding=7;style.TabRounding=7;style.ScrollbarSize=22;
    bool running=true;
    while(running) {
        const auto frame=std::chrono::steady_clock::now();
        drawBegin();
        if(!permeate_record) android::ANativeWindowCreator::ProcessMirrorDisplay();
        graphics->NewFrame();Layout_tick_UI(&running);graphics->EndFrame();
        std::this_thread::sleep_until(frame+std::chrono::milliseconds(16));
    }
    Touch::Close();graphics->Shutdown();android::ANativeWindowCreator::Destroy(window);
    return 0;
}
