#include "DxccDeriver.h"

#include "Dxcc.h"
#include "StrUtil.h"

namespace dxccderive {

Fields derive(const std::string& call, const Fields& fallback) {
    const std::string up = strutil::toUpper(call);
    const dxcc::Info* info = up.empty() ? nullptr : dxcc::lookup(up);
    if (!info)
        return fallback;

    Fields f;
    f.country   = info->entity;
    f.cq_zone   = info->cqZone  ? std::to_string(info->cqZone)  : std::string{};
    f.itu_zone  = info->ituZone ? std::to_string(info->ituZone) : std::string{};
    f.continent = info->continent;
    return f;
}

std::string format(const Fields& f) {
    if (f.country.empty())
        return {};
    std::string s = f.country;
    if (!f.cq_zone.empty())    s += "  ·  CQ " + f.cq_zone;
    if (!f.itu_zone.empty())   s += "  ·  ITU " + f.itu_zone;
    if (!f.continent.empty())  s += "  ·  " + f.continent;
    return s;
}

}  // namespace dxccderive
