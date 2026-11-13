#pragma once
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>

// A complete flat JSON object, parsed before any live state is changed.
namespace nr_params {
struct Values {
    float model = 1, intensity = 1, tone = 1, structure = 1, skin = -1;
    bool nr = true, fg = false, has_fg = false, has_multiplier = false;
    int multiplier = 2;
};
inline bool Parse(std::string_view text, Values &output) {
    size_t i = 0;
    unsigned seen = 0;
    Values value;
    auto ws = [&] { while (i < text.size() && (text[i]==' ' || text[i]=='\t' || text[i]=='\r' || text[i]=='\n')) ++i; };
    ws();
    if (i == text.size() || text[i++] != '{') return false;
    for (;;) {
        ws();
        if (i == text.size() || text[i++] != '"') return false;
        const size_t start = i;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' || static_cast<unsigned char>(text[i]) < 32) return false;
            ++i;
        }
        if (i == text.size()) return false;
        const auto key = text.substr(start, i++ - start);
        constexpr std::string_view keys[] = {"model","intensity","localTone","localStructure","skinStructure","neuralRendering","frameGeneration","fgMultiplier"};
        unsigned field = 0;
        while (field < 8 && key != keys[field]) ++field;
        if (field == 8 || (seen & (1u << field))) return false;
        seen |= 1u << field;
        ws();
        if (i == text.size() || text[i++] != ':') return false;
        ws();
        if (field == 5 || field == 6) {
            bool flag;
            if (text.substr(i,4) == "true") { flag=true; i+=4; }
            else if (text.substr(i,5) == "false") { flag=false; i+=5; }
            else return false;
            if (field == 5) value.nr=flag;
            else { value.fg=flag; value.has_fg=true; }
        } else {
            const size_t first = i;
            if (i < text.size() && text[i]=='-') ++i;
            if (i == text.size()) return false;
            if (text[i]=='0') ++i;
            else {
                if (text[i]<'1' || text[i]>'9') return false;
                while (i<text.size() && text[i]>='0' && text[i]<='9') ++i;
            }
            if (i<text.size() && text[i]=='.') {
                const size_t digits=++i;
                while (i<text.size() && text[i]>='0' && text[i]<='9') ++i;
                if (i==digits) return false;
            }
            if (i<text.size() && (text[i]=='e' || text[i]=='E')) {
                ++i;
                if (i<text.size() && (text[i]=='+' || text[i]=='-')) ++i;
                const size_t digits=i;
                while (i<text.size() && text[i]>='0' && text[i]<='9') ++i;
                if (i==digits) return false;
            }
            float number=0;
            const auto result=std::from_chars(text.data()+first,text.data()+i,number);
            if (result.ec != std::errc{} || result.ptr != text.data()+i || !std::isfinite(number)) return false;
            if (field==0) {
                if (number<1 || number>3 || std::floor(number)!=number) return false;
                value.model=number;
            } else if (field==7) {
                if (number<2 || number>4 || std::floor(number)!=number) return false;
                value.multiplier=static_cast<int>(number); value.has_multiplier=true;
            } else {
                float *fields[]={nullptr,&value.intensity,&value.tone,&value.structure,&value.skin};
                *fields[field]=number;
            }
        }
        ws();
        if (i==text.size()) return false;
        if (text[i]=='}') { ++i; break; }
        if (text[i++]!=',') return false;
    }
    ws();
    if (i!=text.size() || (seen & 63u)!=63u) return false;
    output=value;
    return true;
}
}
