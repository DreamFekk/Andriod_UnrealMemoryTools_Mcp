#include <atomic>
#include <iostream>
#include <stdexcept>
#include "../src/SDKExplorer.cpp"
#include "imgui_internal.h"

namespace {
std::atomic<int> samples{0},guarded{0};
void Require(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
void Drain(bool world){
    for(int i=0;i<2000;++i){
        SDKExplorer::Tick(world);
        if(!SDKExplorer::Busy()&&!SDKExplorer::plan.active)return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("native request queue stalled");
}
}
namespace App {
json State(){return {{"generation",1},{"pid",123},{"probed",true},{"busy",false}};}
json Explore(const json& request){
    auto action=request.value("action",std::string{});
    if(action=="worlds")return {{"generation",1},{"nextCursor",2},{"total",2},{"complete",true},
        {"items",json::array({{{"address","0x1000"},{"name","FixtureWorld"},{"hasGameInstance",true},{"levelCount",2}}})}};
    if(action=="actors")return {{"generation",1},{"nextCursor",2},{"totalSlots",2},{"complete",true},{"topology","one"},{"warnings",json::array()},
        {"levels",json::array({{{"address","0x2000"},{"name","PersistentLevel"},{"count",0}},{{"address","0x3000"},{"name","Sublevel"},{"count",2}}})},
        {"items",json::array({{{"address","0x4000"},{"name","ActorA"},{"className","Pawn"}},{{"address","0x5000"},{"name","ActorB"},{"className","Pawn"}}})}};
    if(action=="inspect"){
        if(request.contains("expectedIdentity"))++guarded;
        const auto address=request.at("address").get<std::string>();
        return {{"generation",1},{"address",address},{"sourceAddress",address},{"name","InstanceFixture"},{"kind",request.value("kind","object")},
            {"identity",address+":fixture"},{"sampledAtMs",++samples},{"properties",json::array({{{"name","Health"},{"address","0x4100"},{"value",std::to_string(samples.load())}}})},
            {"functions",json::array()},{"metadata",json::array()},{"containers",json::array()},{"runtime",json::object()}};
    }
    return {{"error","unsupported fixture action"}};
}
}
int main(){
    try{
        SDKExplorer::Reset(1,123);SDKExplorer::Discover();Drain(true);
        Require(SDKExplorer::worlds.size()==1,"native World discovery");
        Require(SDKExplorer::levels.size()==2 && SDKExplorer::actors.items.size()==2,"native sublevel actors");
        SDKExplorer::Inspect(SDKExplorer::Spec("0x4000","instance"));Drain(false);
        Require(SDKExplorer::detail["address"]=="0x4000","native instance inspection");
        SDKExplorer::Follow("0x5000");Drain(false);
        Require(SDKExplorer::history.size()==1,"native pointer history");
        SDKExplorer::Inspect(SDKExplorer::history.back(),false,true);Drain(false);
        Require(SDKExplorer::detail["address"]=="0x4000" && SDKExplorer::history.empty(),"native back navigation");
        SDKExplorer::Inspect(SDKExplorer::location,false,false,true);Drain(false);
        Require(guarded==1 && SDKExplorer::detail["changedCount"]==1,"native guarded live value changes");
        SDKExplorer::Inspect(SDKExplorer::Spec("0x5000"));SDKExplorer::Reset(2,123);Drain(false);
        Require(SDKExplorer::detail.is_null(),"stale asynchronous result discarded on generation change");

        ImGui::CreateContext();
        auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;io.DisplaySize={1600,1000};io.DeltaTime=1.f/60;
        io.Fonts->AddFontDefault();unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        char textA[64]{},textB[64]{};
        for(int i=0;i<2;++i){
            ImGui::NewFrame();ImGui::SetNextWindowPos({20,20});ImGui::SetNextWindowSize({1400,900});ImGui::Begin("host");
            ImGui::BeginChild(i?"type-child":"address-child",{300,300});ImGui::PushID(i?"struct-type":"instance-address");
            SDKExplorer::Keyboard(i?textB:textA,64,"Fixture keyboard");
            ImGui::PopID();ImGui::EndChild();
            SDKExplorer::DrawKeyboard();
            Require(ImGui::IsPopupOpen("##native_keyboard"),"keyboard popup must open outside caller child/PushID scope");
            Require(SDKExplorer::keyboardTarget==(i?textB:textA),"keyboard targets correct buffer");
            ImGui::ClosePopupToLevel(0,true);SDKExplorer::keyboardOpen=false;
            ImGui::End();ImGui::Render();
        }
        ImGui::DestroyContext();SDKExplorer::Shutdown();
        std::cout<<"PASS: native World/sublevel, instance navigation/back, live changes, generation guard, keyboard child-ID scopes and buffers\n";
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
