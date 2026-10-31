#include "SDKExplorer.hpp"
#include "App.hpp"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SDKExplorer {
namespace {
using json=nlohmann::json;
using Clock=std::chrono::steady_clock;
int lang=0,mode=0,rowLimit=100,valueInterval=1,listInterval=1;
uint32_t generation=0;
int targetPid=0;
bool lastWorld=false,liveValues=false,liveActors=false,worldAttempted=false;
char objectQuery[256]{},actorQuery[256]{},memberQuery[128]{},addressInput[40]{},typeInput[40]{};
bool dereference=false;
json detail,location,worlds=json::array(),levels=json::array();
std::vector<json> history,bookmarks;
std::string selectedWorld,selectedLevel,error,message;
Clock::time_point lastValues{},lastActors{};
const char* Tr(const char* zh,const char* en){return lang?en:zh;}
std::string S(const json& j,const char* key){if(!j.contains(key)||j[key].is_null())return "";return j[key].is_string()?j[key].get<std::string>():j[key].dump();}
std::string Lower(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c+=32;return s;}
bool Pointer(const std::string& s){return s.size()>2 && s.starts_with("0x") && s.find_first_not_of('0',2)!=std::string::npos;}
void Copy(char* target,size_t size,const std::string& s){std::snprintf(target,size,"%s",s.c_str());}
void Text(const std::string& s){ImGui::TextUnformatted(s.c_str());}
void Wrapped(const std::string& s){ImGui::TextWrapped("%s",s.c_str());}

struct Page {json items=json::array();int start=0,next=0,total=0,number=1;bool complete=false,loaded=false;std::vector<int> starts{0};};
Page objects,actors;
struct Plan {bool active=false,world=false;int cursor=0;std::string query,topology;};
Plan plan;
enum class Task {None,Objects,Worlds,Actors,Inspect};
struct Job {Task task=Task::None;json args;uint64_t epoch=0;bool remember=false,back=false,refresh=false;};
std::future<json> future;
Job job;
uint64_t epoch=0;
bool Busy(){return future.valid();}
int discoverCursor=0;

void Stop(){++epoch;plan.active=false;liveActors=false;message=Tr("已停止","Stopped");}
void Reset(uint32_t gen,int pid){
    ++epoch;generation=gen;targetPid=pid;plan={};objects={};actors={};
    detail=json();location=json();worlds=json::array();levels=json::array();history.clear();bookmarks.clear();
    selectedWorld.clear();selectedLevel.clear();error.clear();message.clear();worldAttempted=false;liveActors=liveValues=false;
    addressInput[0]=typeInput[0]=0;
}
bool Submit(Task task,json args,bool remember=false,bool back=false,bool refresh=false){
    if(Busy())return false;
    args["generation"]=generation;
    job={task,args,epoch,remember,back,refresh};
    future=std::async(std::launch::async,[args]{
        try{
            for(int i=0;i<10;++i){auto result=App::Explore(args);if(!result.value("busy",false)||i==9)return result;std::this_thread::sleep_for(std::chrono::milliseconds(100));}
        }catch(const std::exception& e){return json{{"error",e.what()}};}
        return json{{"error","Read request did not complete."}};
    });
    return true;
}
const char* Kind(int m){return m==1?"instance":m==2?"struct":m==3?"field":"object";}
json Spec(const std::string& address,const std::string& kind="object",const std::string& type="",bool indirect=false){
    json q={{"action","inspect"},{"address",address},{"kind",kind},{"maxRows",64},{"dereference",indirect}};
    if(!type.empty())q["typeAddress"]=type;return q;
}
void Inspect(json request,bool remember=true,bool back=false,bool refresh=false){
    if(Busy())return;
    if(!refresh){liveValues=false;plan.active=false;}
    if(refresh && detail.contains("identity"))request["expectedIdentity"]=detail["identity"];
    Submit(Task::Inspect,request,remember,back,refresh);
}
void Follow(const std::string& address,const std::string& kind="object",const std::string& type=""){
    if(Pointer(address))Inspect(Spec(address,kind,type));
}
void StartPage(bool world,int start=0,int number=1){
    if(Busy())return;
    auto& page=world?actors:objects;
    page.items=json::array();page.start=page.next=start;page.number=number;page.complete=false;page.loaded=true;
    if(number==1)page.starts={0};
    plan={true,world,start,world?actorQuery:objectQuery,world && start>0?plan.topology:""};
    error.clear();message=Tr("读取中…","Reading...");
}
void Discover(){
    if(Busy())return;
    Stop();worldAttempted=true;worlds=json::array();levels=json::array();actors={};discoverCursor=0;
    selectedWorld.clear();selectedLevel.clear();error.clear();message=Tr("扫描 UWorld…","Scanning UWorld...");
    Submit(Task::Worlds,{{"action","worlds"},{"cursor",0},{"limit",64},{"scanLimit",8192}});
}
void MarkChanges(const json& old,json& value){
    std::unordered_map<std::string,std::string> previous;
    if(old.contains("properties"))for(const auto& row:old["properties"])previous[S(row,"address")+S(row,"name")]=S(row,"value");
    int changed=0;
    if(value.contains("properties"))for(auto& row:value["properties"]){
        const auto key=S(row,"address")+S(row,"name");const auto found=previous.find(key);
        row["changed"]=found!=previous.end() && found->second!=S(row,"value");if(row["changed"].get<bool>())++changed;
    }
    value["changedCount"]=changed;
}
void Tick(bool world){
    if(Busy() && future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
        auto response=future.get();const auto finished=job;job={};
        if(finished.epoch!=epoch)return;
        if(response.contains("error")){
            error=S(response,"error");
            if(response.value("changed",false) && finished.task==Task::Actors){plan.topology.clear();StartPage(true);}
            else{plan.active=false;if(finished.task==Task::Inspect)liveValues=false;}
            return;
        }
        error.clear();
        if(finished.task==Task::Inspect){
            if(finished.remember && !location.is_null() && location!=finished.args){history.push_back(location);if(history.size()>64)history.erase(history.begin());}
            if(finished.back && !history.empty())history.pop_back();
            if(finished.refresh)MarkChanges(detail,response);
            detail=std::move(response);location=finished.args;location.erase("expectedIdentity");location.erase("generation");
            Copy(addressInput,sizeof(addressInput),S(detail,"sourceAddress").empty()?S(detail,"address"):S(detail,"sourceAddress"));
            Copy(typeInput,sizeof(typeInput),S(detail,"typeAddress"));
            const auto kind=S(detail,"kind");mode=kind=="instance"?1:kind=="struct"?2:kind=="field"?3:0;
            dereference=detail.value("dereferenced",false);lastValues=Clock::now();
        }else if(finished.task==Task::Worlds){
            for(const auto& item:response["items"])worlds.push_back(item);
            discoverCursor=response.value("nextCursor",discoverCursor);
            message=Tr("扫描 UWorld: ","Scanning UWorld: ")+std::to_string(discoverCursor)+"/"+S(response,"total");
            if(response.value("complete",false)){
                std::sort(worlds.begin(),worlds.end(),[](const json& a,const json& b){if(a.value("hasGameInstance",false)!=b.value("hasGameInstance",false))return a.value("hasGameInstance",false);return a.value("levelCount",0)>b.value("levelCount",0);});
                if(!worlds.empty()){selectedWorld=S(worlds[0],"address");StartPage(true);}
                else message=Tr("没有可用 World，请进入游戏场景后重扫。","No live World. Enter a game scene and rescan.");
            }else Submit(Task::Worlds,{{"action","worlds"},{"cursor",discoverCursor},{"limit",64},{"scanLimit",8192}});
        }else{
            const bool isWorld=finished.task==Task::Actors;auto& page=isWorld?actors:objects;
            std::unordered_set<std::string> seen;for(const auto& row:page.items)seen.insert(S(row,"address"));
            for(const auto& item:response["items"])if(seen.insert(S(item,"address")).second)page.items.push_back(item);
            page.total=response.value(isWorld?"totalSlots":"total",0);
            page.next=response.value("nextCursor",page.next);page.complete=response.value("complete",false);
            plan.cursor=page.next;
            if(isWorld){levels=response["levels"];plan.topology=S(response,"topology");lastActors=Clock::now();if(!response["warnings"].empty())error=response["warnings"].dump();}
            plan.active=!page.complete && page.items.size()<size_t(rowLimit);
            message=std::to_string(page.items.size())+Tr(" 项 / 已扫描槽位 "," rows / scanned slots ")+std::to_string(page.next)+"/"+std::to_string(page.total);
        }
    }
    if(Busy())return;
    if(plan.active){
        auto& page=plan.world?actors:objects;
        json request={{"action",plan.world?"actors":"list"},{"cursor",plan.cursor},{"limit",rowLimit-int(page.items.size())},
                      {"scanLimit",4096},{"query",plan.query}};
        if(plan.world){request["worldAddress"]=selectedWorld;if(!selectedLevel.empty())request["levelAddress"]=selectedLevel;if(!plan.topology.empty())request["expectedTopology"]=plan.topology;}
        Submit(plan.world?Task::Actors:Task::Objects,request);return;
    }
    const auto now=Clock::now();
    static const int valuesMs[]{500,1000,2000,5000},listsMs[]{1000,2000,5000};
    if(world && liveActors && !selectedWorld.empty() && now-lastActors>=std::chrono::milliseconds(listsMs[listInterval])){
        StartPage(true,actors.start,actors.number);lastActors=now;return;
    }
    if(liveValues && !location.is_null() && now-lastValues>=std::chrono::milliseconds(valuesMs[valueInterval])){
        Inspect(location,false,false,true);lastValues=now;
    }
}

char* keyboardTarget=nullptr;size_t keyboardSize=0;std::string keyboardTitle;bool keyboardOpen=false,keyboardPending=false,lowercase=false;
void Keyboard(char* buffer,size_t size,const char* title){
    keyboardTarget=buffer;keyboardSize=size;keyboardTitle=title;keyboardOpen=true;keyboardPending=true;
}
void DrawKeyboard(){
    // OpenPopup and BeginPopupModal must use the same parent window and ID stack.
    // Input() is inside a child window + PushID, so it only queues the request.
    if(keyboardPending){ImGui::OpenPopup("##native_keyboard");keyboardPending=false;}
    if(!keyboardOpen)return;
    const auto hostPos=ImGui::GetWindowPos(),hostSize=ImGui::GetWindowSize();
    ImGui::SetNextWindowPos(ImVec2(hostPos.x+hostSize.x*.5f,hostPos.y+hostSize.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1100.f,ImGui::GetIO().DisplaySize.x*.85f),0),ImGuiCond_Appearing);
    if(ImGui::BeginPopupModal("##native_keyboard",&keyboardOpen,ImGuiWindowFlags_AlwaysAutoResize)){
        Text(keyboardTitle);ImGui::SetNextItemWidth(-1);ImGui::InputText("##kb_value",keyboardTarget,keyboardSize);
        const float keyWidth=std::max(38.f,(ImGui::GetContentRegionAvail().x-9*ImGui::GetStyle().ItemSpacing.x)/10);
        for(const char* row:{"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM_.-","/:*+$()[]"}){
            for(size_t i=0;row[i];++i){if(i)ImGui::SameLine();char label[2]{row[i],0};if(lowercase && label[0]>='A'&&label[0]<='Z')label[0]+=32;
                ImGui::PushID(row);if(ImGui::Button(label,ImVec2(keyWidth,0))){size_t length=std::strlen(keyboardTarget);if(length+1<keyboardSize){keyboardTarget[length]=label[0];keyboardTarget[length+1]=0;}}ImGui::PopID();
            }
        }
        if(ImGui::Button("Aa"))lowercase=!lowercase;ImGui::SameLine();
        if(ImGui::Button(Tr("退格","Backspace"))){size_t n=std::strlen(keyboardTarget);if(n)keyboardTarget[n-1]=0;}ImGui::SameLine();
        if(ImGui::Button(Tr("清空","Clear")))keyboardTarget[0]=0;ImGui::SameLine();
        if(ImGui::Button(Tr("空格","Space"))){size_t n=std::strlen(keyboardTarget);if(n+1<keyboardSize){keyboardTarget[n]=' ';keyboardTarget[n+1]=0;}}ImGui::SameLine();
        if(ImGui::Button(Tr("完成","Done"))){keyboardOpen=false;ImGui::CloseCurrentPopup();}
        ImGui::EndPopup();
    }
}
void Input(const char* id,char* text,size_t size,const char* title,float width){
    ImGui::PushID(id);ImGui::SetNextItemWidth(width);ImGui::InputText("##input",text,size);ImGui::SameLine();
    if(ImGui::SmallButton(Tr("键盘","KB")))Keyboard(text,size,title);ImGui::PopID();
}
bool Link(const std::string& text,const std::string& address,const char* kind="object",const std::string& type=""){
    if(!Pointer(address)){Text(text);return false;}
    ImGui::PushID(address.c_str());ImGui::BeginDisabled(Busy());
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.35f,.78f,1,1));
    const bool clicked=ImGui::SmallButton(text.c_str());
    ImGui::PopStyleColor();ImGui::EndDisabled();ImGui::PopID();
    if(clicked)Follow(address,kind,type);return clicked;
}
void TypeCell(const json& row){
    if(row.contains("typeLinks")&&!row["typeLinks"].empty()){
        const auto& links=row["typeLinks"];
        for(size_t i=0;i<links.size();++i){ImGui::PushID(int(i));Link(links.size()==1?S(row,"type"):S(links[i],"name"),S(links[i],"address"));ImGui::PopID();}
    }else Text(S(row,"type"));
}
void NameCell(const json& row){
    if(Pointer(S(row,"pointer")))Link(S(row,"name"),S(row,"pointer"));
    else if(Pointer(S(row,"structAddress")))Link(S(row,"name"),S(row,"address"),"struct",S(row,"structAddress"));
    else if(row.contains("typeLinks")&&row["typeLinks"].size()==1)Link(S(row,"name"),S(row["typeLinks"][0],"address"));
    else if(Pointer(S(row,"descriptorAddress")))Link(S(row,"name"),S(row,"descriptorAddress"),S(row,"descriptorKind")=="field"?"field":"object");
    else Text(S(row,"name"));
}
void ValueCell(const json& row){
    if(row.value("changed",false))ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.78f,.2f,1));
    if(Pointer(S(row,"pointer")))Link(S(row,"value"),S(row,"pointer"));
    else if(Pointer(S(row,"structAddress")))Link(S(row,"value")+" >",S(row,"address"),"struct",S(row,"structAddress"));
    else if(S(row,"valueKind")=="declaration")ImGui::TextDisabled("%s",Tr("类型声明","Type declaration"));
    else Text(S(row,"value"));
    if(row.value("changed",false))ImGui::PopStyleColor();
    if(row.contains("children")&&!row["children"].empty() && ImGui::TreeNode(Tr("结构成员预览","Struct member preview"))){
        for(const auto& child:row["children"]){ImGui::PushID(S(child,"address").c_str());Text(S(child,"name")+" +"+S(child,"offset")+" "+S(child,"type"));ValueCell(child);ImGui::PopID();}
        ImGui::TreePop();
    }
    if(row.value("childrenTruncated",false))ImGui::TextDisabled("%s",Tr("预览受限，可点击进入","Preview bounded; click to inspect deeper."));
    if(!S(row,"note").empty())ImGui::TextDisabled("%s",S(row,"note").c_str());
}
void Properties(const json& rows){
    if(!rows.is_array())return;
    if(ImGui::BeginTable("##properties",6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)){
        for(auto name:{Tr("成员","Member"),Tr("类型","Type"),Tr("所属类","Owner"),Tr("偏移","Offset"),Tr("地址","Address"),Tr("值 / 跳转","Value / follow")})ImGui::TableSetupColumn(name);
        ImGui::TableHeadersRow();int i=0;const auto needle=Lower(memberQuery);
        for(const auto& row:rows){
            if(!needle.empty() && Lower(S(row,"name")+" "+S(row,"type")).find(needle)==std::string::npos)continue;
            ImGui::PushID(i++);ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);NameCell(row);
            ImGui::TableSetColumnIndex(1);TypeCell(row);
            ImGui::TableSetColumnIndex(2);Text(S(row,"owner"));
            ImGui::TableSetColumnIndex(3);Text(S(row,"offset"));
            ImGui::TableSetColumnIndex(4);
            if(Pointer(S(row,"descriptorAddress")))Link(S(row,"address"),S(row,"descriptorAddress"),S(row,"descriptorKind")=="field"?"field":"object");
            else Text(S(row,"address"));
            ImGui::TableSetColumnIndex(5);ValueCell(row);ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
void Inspector(){
    ImGui::BeginDisabled(Busy());
    if(ImGui::Button(Tr("返回","Back"))&&!history.empty())Inspect(history.back(),false,true);
    ImGui::SameLine();if(ImGui::Button(Tr("刷新数值","Refresh"))&&!location.is_null())Inspect(location,false,false,true);
    ImGui::SameLine();if(ImGui::Button(Tr("标记","Bookmark"))&&!location.is_null()){bookmarks.push_back({{"name",S(detail,"name")},{"request",location}});if(bookmarks.size()>32)bookmarks.erase(bookmarks.begin());}
    ImGui::SameLine();ImGui::Checkbox(Tr("实时值","Live values"),&liveValues);ImGui::SameLine();
    ImGui::SetNextItemWidth(120);const char* rates[]{"0.5 s","1 s","2 s","5 s"};ImGui::Combo("##value_rate",&valueInterval,rates,4);
    Input("address",addressInput,sizeof(addressInput),Tr("对象 / 结构地址","Object / struct address"),std::max(170.f,ImGui::GetContentRegionAvail().x*.42f));
    ImGui::SameLine();ImGui::SetNextItemWidth(190);const char* kinds[]{"UObject / Class","Instance","Struct","FField"};ImGui::Combo("##kind",&mode,kinds,4);
    ImGui::SameLine();if(ImGui::Button(Tr("检查","Inspect")))Inspect(Spec(addressInput,Kind(mode),mode==2?typeInput:"",dereference));
    if(mode==2)Input("type",typeInput,sizeof(typeInput),"UScriptStruct",std::max(170.f,ImGui::GetContentRegionAvail().x*.65f));
    ImGui::Checkbox(Tr("地址为指针槽，先解引用一次","Input is a pointer slot: dereference once"),&dereference);
    if(!bookmarks.empty() && ImGui::BeginCombo(Tr("标签","Bookmarks"),Tr("选择标签","Select bookmark"))){
        for(size_t i=0;i<bookmarks.size();++i){ImGui::PushID(int(i));if(ImGui::Selectable(S(bookmarks[i],"name").c_str()))Inspect(bookmarks[i]["request"]);ImGui::PopID();}
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if(!error.empty()){ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.45f,.35f,1));Wrapped(error);ImGui::PopStyleColor();}
    if(detail.is_null()){Wrapped(Tr("选择对象或输入地址，查看所属类、成员偏移、值及函数。","Select an object or enter its address to inspect types, members, values and functions."));return;}
    ImGui::Separator();ImGui::TextColored(ImVec4(.35f,.78f,1,1),"%s",S(detail,"name").c_str());Wrapped(S(detail,"fullName"));
    Text(S(detail,"address")+" | "+Tr("采样(ms): ","Sample(ms): ")+S(detail,"sampledAtMs")+" | "+Tr("变化: ","Changed: ")+S(detail,"changedCount"));
    for(auto key:{"classAddress","outerAddress","superAddress","classDefaultObjectAddress"})if(Pointer(S(detail,key))){ImGui::PushID(key);Link(std::string(key)+" "+S(detail,key),S(detail,key));ImGui::PopID();}
    if(!S(detail,"note").empty())Wrapped(S(detail,"note"));
    Input("member_search",memberQuery,sizeof(memberQuery),Tr("筛选成员","Filter members"),std::max(170.f,ImGui::GetContentRegionAvail().x*.65f));
    if(ImGui::CollapsingHeader(Tr("成员 / 偏移 / 实时数值","Members / offsets / live values"),ImGuiTreeNodeFlags_DefaultOpen))Properties(detail.value("properties",json::array()));
    if(detail.contains("objectProperties")&&ImGui::CollapsingHeader(Tr("对象自身属性","Metadata object values")))Properties(detail["objectProperties"]);
    if(ImGui::CollapsingHeader(Tr("Class 继承链与元数据","Class hierarchy / metadata"))){
        for(const auto& row:detail.value("classHierarchy",json::array()))Link(S(row,"name")+" "+S(row,"address"),S(row,"address"));
        for(const auto& row:detail.value("metadata",json::array()))Wrapped(S(row,"name")+": "+S(row,"value"));
        for(const auto& row:detail.value("enums",json::array()))Wrapped(S(row,"name")+" = "+S(row,"value"));
    }
    if(ImGui::CollapsingHeader((std::string(Tr("函数（含继承）","Functions (inherited included)"))+" ["+std::to_string(detail.value("functions",json::array()).size())+"]").c_str())){
        int i=0;for(const auto& fn:detail.value("functions",json::array())){ImGui::PushID(i++);Link(S(fn,"signature"),S(fn,"objectAddress"));Wrapped(S(fn,"owner")+" | "+S(fn,"address")+" | RVA "+S(fn,"rva")+" | "+S(fn,"flags"));ImGui::PopID();}
    }
    if(ImGui::CollapsingHeader(Tr("容器与数组元素","Containers / array elements"))){
        int i=0;for(const auto& c:detail.value("containers",json::array())){
            ImGui::PushID(i++);Text(S(c,"name")+" "+S(c,"type"));Wrapped("Count="+S(c,"count")+" Max="+S(c,"max")+" Data="+S(c,"dataAddress"));Wrapped(S(c,"note"));
            for(const auto& item:c.value("items",json::array())){ImGui::PushID(S(item,"address").c_str());Text("["+S(item,"index")+"] "+S(item,"address"));TypeCell(item);ValueCell(item);ImGui::PopID();}
            ImGui::PopID();
        }
    }
    if(ImGui::CollapsingHeader(Tr("运行时","Runtime"))){
        const auto runtime=detail.value("runtime",json::object());
        for(const auto& field:runtime.value("fields",json::array())){ImGui::PushID(S(field,"name").c_str());Text(S(field,"name"));ValueCell(field);ImGui::PopID();}
        for(const auto& actor:runtime.value("actors",json::array()))Link(S(actor,"name")+" "+S(actor,"address"),S(actor,"address"),"instance");
        if(runtime.contains("rowMap"))Wrapped(runtime["rowMap"].dump());
    }
}
void Browser(bool world){
    auto& page=world?actors:objects;auto* query=world?actorQuery:objectQuery;
    ImGui::BeginDisabled(Busy());
    if(world){
        if(ImGui::Button(Tr("扫描 World","Scan Worlds")))Discover();
        std::string worldLabel=selectedWorld;
        for(const auto& w:worlds)if(S(w,"address")==selectedWorld)worldLabel=S(w,"name")+" "+selectedWorld;
        ImGui::SetNextItemWidth(-1);
        if(ImGui::BeginCombo("##world",worldLabel.empty()?Tr("选择 World","Select World"):worldLabel.c_str())){
            for(const auto& w:worlds){const auto label=S(w,"name")+" "+(w.value("hasGameInstance",false)?"[GameInstance]":"")+"##"+S(w,"address");if(ImGui::Selectable(label.c_str(),selectedWorld==S(w,"address"))){selectedWorld=S(w,"address");selectedLevel.clear();levels=json::array();StartPage(true);}}
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(-1);
        if(ImGui::BeginCombo("##level",selectedLevel.empty()?Tr("全部已加载 Level","All loaded Levels"):selectedLevel.c_str())){
            if(ImGui::Selectable(Tr("全部已加载 Level","All loaded Levels"),selectedLevel.empty())){selectedLevel.clear();StartPage(true);}
            for(const auto& l:levels){const auto label=S(l,"name")+" ["+S(l,"count")+"]##"+S(l,"address");if(ImGui::Selectable(label.c_str(),selectedLevel==S(l,"address"))){selectedLevel=S(l,"address");StartPage(true);}}
            ImGui::EndCombo();
        }
        ImGui::Checkbox(Tr("实时列表","Live list"),&liveActors);ImGui::SameLine();ImGui::SetNextItemWidth(110);
        const char* rates[]{"1 s","2 s","5 s"};ImGui::Combo("##list_rate",&listInterval,rates,3);
    }
    Input(world?"actor_query":"object_query",query,256,Tr("名称 / 类型搜索","Name / class search"),std::max(140.f,ImGui::GetContentRegionAvail().x-110));
    if(ImGui::Button(Tr("搜索 / 刷新","Search / refresh")))StartPage(world);
    ImGui::SameLine();ImGui::SetNextItemWidth(110);
    if(ImGui::BeginCombo("##rows",std::to_string(rowLimit).c_str())){for(int n:{25,50,100,250})if(ImGui::Selectable(std::to_string(n).c_str(),n==rowLimit)){rowLimit=n;StartPage(world);}ImGui::EndCombo();}
    if(ImGui::Button("<")&&page.number>1){const int previous=page.number-1;StartPage(world,page.starts.at(previous-1),previous);}
    ImGui::SameLine();Text(std::to_string(page.number)+" | "+std::to_string(page.next)+"/"+std::to_string(page.total));
    ImGui::SameLine();if(ImGui::Button(">")&&!page.complete){const int next=page.next,num=page.number+1;if(page.starts.size()<size_t(num))page.starts.push_back(next);else page.starts[num-1]=next;StartPage(world,next,num);}
    ImGui::EndDisabled();
    if(Busy()||plan.active){if(ImGui::SmallButton(Tr("停止扫描","Stop scan")))Stop();}
    Wrapped(message);
    if(world)Text(Tr("Level 数: ","Levels: ")+std::to_string(levels.size()));
    ImGui::Separator();
    if(ImGui::BeginChild("##list_rows",ImVec2(0,0),false)){
        for(const auto& item:page.items){
            const auto addr=S(item,"address");ImGui::PushID(addr.c_str());ImGui::BeginDisabled(Busy());
            const auto label=S(item,"name")+"##actor";
            if(ImGui::Selectable(label.c_str(),S(detail,"address")==addr))Inspect(Spec(addr,world?"instance":"object"));
            ImGui::EndDisabled();ImGui::TextDisabled("%s",S(item,"className").c_str());Text(addr);
            if(world)ImGui::TextDisabled("[%s] %s",S(item,"actorSlot").c_str(),S(item,"levelName").c_str());
            ImGui::Separator();ImGui::PopID();
        }
    }
    ImGui::EndChild();
}
} // namespace
void SetLanguage(int value){lang=value;}
void Shutdown(){if(future.valid()){future.wait();future.get();}plan.active=false;}
void Render(bool world){
    const auto state=App::State();
    const auto gen=state.value("generation",0u);const auto pid=state.value("pid",0);
    if(gen!=generation || pid!=targetPid)Reset(gen,pid);
    if(lastWorld!=world){if(plan.active)Stop();lastWorld=world;}
    Tick(world);
    if(!Busy()&&!plan.active){
        if(world && !worldAttempted)Discover();
        else if(!world && !objects.loaded)StartPage(false);
    }
    const auto available=ImGui::GetContentRegionAvail();
    const float left=std::clamp(available.x*.31f,320.f,620.f);
    ImGui::BeginChild(world?"##world_left":"##sdk_left",ImVec2(left,0),true);Browser(world);ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##native_inspector",ImVec2(0,0),true,ImGuiWindowFlags_HorizontalScrollbar);Inspector();ImGui::EndChild();
    DrawKeyboard();
}
} // namespace SDKExplorer
