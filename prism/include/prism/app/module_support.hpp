#pragma once
#include "prism/contracts/app_module.h"
#include "prism/contracts/display_list.hpp"
#include <string_view>
#include <cstdint>
namespace prism::app {
inline bool ValidHost(const PrismAppInitV1* init) {
    return init && init->struct_size>=sizeof(*init) && init->abi_version==PRISM_APP_ABI_V1 &&
        init->host && init->host->struct_size>=offsetof(PrismHostApiV1,subscribe_instances) &&
        init->host->abi_version==PRISM_APP_ABI_V1 && init->host->set_binding &&
        init->host->backend_ready && init->host->schedule_tick;
}
inline bool Text(const PrismHostApiV1* host,std::string_view key,std::string_view text) {
    PrismValueV1 value{}; value.kind=PRISM_VALUE_STRING_V1; value.as.string={text.data(),text.size()};
    return host->set_binding(host->context,{key.data(),key.size()},value)==0;
}
inline bool Number(const PrismHostApiV1* host,std::string_view key,double number) {
    PrismValueV1 value{}; value.kind=PRISM_VALUE_NUMBER_V1; value.as.number=number;
    return host->set_binding(host->context,{key.data(),key.size()},value)==0;
}
inline bool Boolean(const PrismHostApiV1* host,std::string_view key,bool boolean) {
    PrismValueV1 value{}; value.kind=PRISM_VALUE_BOOL_V1; value.as.boolean=boolean;
    return host->set_binding(host->context,{key.data(),key.size()},value)==0;
}
inline bool Color(const PrismHostApiV1* host,std::string_view key,contracts::Color color) {
    PrismValueV1 value{}; value.kind=PRISM_VALUE_COLOR_V1;
    value.as.rgba=(static_cast<std::uint32_t>(color.r)<<24) |
        (static_cast<std::uint32_t>(color.g)<<16) | (static_cast<std::uint32_t>(color.b)<<8) | color.a;
    return host->set_binding(host->context,{key.data(),key.size()},value)==0;
}
inline bool Ready(const PrismHostApiV1* host) { return host->backend_ready(host->context)==0; }
inline bool Tick(const PrismHostApiV1* host,std::uint64_t delay=1000000000ULL) {
    return host->schedule_tick(host->context,delay)==0;
}
inline std::uint64_t SelectTheme(const PrismHostApiV1* host,std::string_view id) {
    if (!host || host->struct_size < offsetof(PrismHostApiV1,select_theme)+sizeof(host->select_theme) ||
        !host->select_theme) return 0;
    return host->select_theme(host->context,{id.data(),id.size()});
}
} // namespace prism::app
