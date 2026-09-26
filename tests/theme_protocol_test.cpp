#include "prism/launch/control_protocol.hpp"
#include "prism/launch/worker_protocol.hpp"
#include "prism/theme/compiler.hpp"
#include <cassert>
#include <limits>
#include <iostream>
using namespace prism;
template<class F> void Reject(F f) {bool rejected=false;try{f();}catch(const std::invalid_argument&){rejected=true;}assert(rejected);}
int main(){
    auto t=theme::LoadTheme(PRISM_SOURCE_THEMES,"glass",41);
    const auto encoded=contracts::EncodeTheme(t);
    assert(contracts::DecodeTheme(encoded)==t);
    for(auto size:{0u,1u,12u,64u})Reject([&]{contracts::DecodeTheme(std::span(encoded).first(size));});
    auto bytes=encoded;bytes.push_back(0);Reject([&]{contracts::DecodeTheme(bytes);});
    auto invalid=t;invalid.numbers.push_back(invalid.numbers.front());Reject([&]{contracts::EncodeTheme(invalid);});
    invalid=t;invalid.materials.front().backdrop_blur=49;Reject([&]{contracts::EncodeTheme(invalid);});
    invalid=t;invalid.layout.outer_gap=std::numeric_limits<double>::quiet_NaN();Reject([&]{contracts::EncodeTheme(invalid);});
    invalid=t;invalid.id="../glass";Reject([&]{contracts::EncodeTheme(invalid);});
    launch::ControlMessage m;m.type=launch::ControlType::InstallTheme;m.permit.session=17;m.theme=t;
    auto c=launch::EncodeControl(m);assert(launch::ControlFrameSize(c)==c.size());
    assert(launch::DecodeControl(c).theme==t);c.push_back(0);Reject([&]{launch::DecodeControl(c);});
    m.type=launch::ControlType::ThemeApplied;m.theme_applied={41,true,{}};
    assert(launch::DecodeControl(launch::EncodeControl(m)).theme_applied.generation==41);
    auto worker=launch::DecodeWorker(launch::EncodeWorker(t));assert(std::get<contracts::ThemeSnapshot>(worker)==t);
    worker=launch::DecodeWorker(launch::EncodeWorker(contracts::ThemeApplied{41,false,"Unsupported material"}));
    assert(!std::get<contracts::ThemeApplied>(worker).success);
    auto query=std::get<contracts::ThemeRequest>(launch::DecodeMessage(launch::EncodeMessage(contracts::ThemeRequest{8,{}})));
    assert(query.request==8&&query.id.empty());
    contracts::ThemeEvent event{8,41,contracts::ThemeStatus::Applied,t.id,t.name,"Installed"};
    auto result=std::get<contracts::ThemeEvent>(launch::DecodeMessage(launch::EncodeMessage(event)));
    assert(result.request==8&&result.generation==41&&result.id==t.id);
    auto frame=launch::EncodeMessage(contracts::ThemeRequest{8,"square"});frame[19]=9;
    Reject([&]{launch::DecodeMessage(frame);});
    Reject([&]{launch::EncodeMessage(contracts::ThemeRequest{9,"/tmp/theme"});});
    std::cout<<"Theme snapshot bounds, round trips, identity and typed PRL/PRW/PWC extensions passed\n";
}
