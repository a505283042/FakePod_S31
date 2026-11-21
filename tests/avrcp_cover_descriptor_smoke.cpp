// Standalone host-side test:
// g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/avrcp_cover_descriptor_smoke.cpp -o cover_smoke
// ./cover_smoke
#include "system/bt_avrc_cover_descriptor.h"
#include <cassert>
#include <cstring>
#include <cstdio>

using namespace bt_avrc_cover_descriptor;

static Selection check(const char *xml, bool expected)
{
    Selection sel = {};
    const bool ok = select(xml, strlen(xml), 512U * 1024U, &sel);
    assert(ok == expected);
    return sel;
}
int main()
{
    const char *ranges =
        "<?xml version=\"1.0\"?><image-properties version=\"1.0\" handle=\"0000001\">"
        "<native encoding=\"JPEG\" pixel=\"1200*1200\" size=\"900000\"/>"
        "<variant encoding=\"JPEG\" pixel=\"200*200-800*800\"/>"
        "</image-properties>";
    Selection sel = check(ranges, true);
    assert(sel.variant && sel.width == 460 && sel.height == 460);
    char xml[160] = {};
    const size_t n = make_xml(sel, xml, sizeof(xml));
    assert(n != 0 && n == strlen(xml));
    assert(strstr(xml, "pixel=\"460*460\"") != nullptr);
    assert(strstr(xml, "encoding=\"JPEG\"") != nullptr);

    const char *native =
        "<image-properties version=\"1.0\" handle=\"0000001\">"
        "<native encoding=\"JPEG\" pixel=\"512*512\" size=\"180000\"/>"
        "</image-properties>";
    sel = check(native, true);
    assert(!sel.variant && sel.width == 512 && sel.height == 512);
    assert(make_xml(sel, xml, sizeof(xml)) > 0);

    const char *over =
        "<image-properties version=\"1.0\"><native encoding=\"JPEG\" "
        "pixel=\"1200*1200\" size=\"900000\"/></image-properties>";
    check(over, false);

    const char *thumb =
        "<image-properties version=\"1.0\"><native encoding=\"JPEG\" "
        "pixel=\"200*200\" size=\"20000\"/></image-properties>";
    check(thumb, false);

    const char *fixed =
        "<image-properties version=\"1.0\"><variant encoding=\"JPEG\" "
        "pixel=\"512*512\"/></image-properties>";
    sel = check(fixed, true);
    assert(sel.variant && sel.width == 512 && sel.height == 512);

    const char *png =
        "<image-properties version=\"1.0\"><native encoding=\"PNG\" "
        "pixel=\"512*512\" size=\"80000\"/></image-properties>";
    check(png, false);
    assert(make_xml(sel, xml, 5) == 0U);
    std::puts("AVRCP HD Cover Descriptor: 6 cases PASS");
}
