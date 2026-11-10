#include "aimj/wav.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace aimj {
namespace {
void le16(std::ostream& o, std::uint16_t v) {o.put(static_cast<char>(v));o.put(static_cast<char>(v>>8));}
void le32(std::ostream& o, std::uint32_t v) {for(int i=0;i<4;i++)o.put(static_cast<char>(v>>(8*i)));}
}
void write_pcm16_wav(const std::string& path,const AudioBuffer& a) {
    if(a.sample_rate<=0 || a.samples.empty() || a.samples.size()> (std::numeric_limits<std::uint32_t>::max()-36u)/2u)
        throw std::invalid_argument("invalid PCM");
    std::ofstream f(path,std::ios::binary|std::ios::trunc);
    if(!f) throw std::runtime_error("cannot open WAV path");
    auto bytes=static_cast<std::uint32_t>(a.samples.size()*2);
    f.write("RIFF",4);le32(f,36+bytes);f.write("WAVEfmt ",8);
    le32(f,16);le16(f,1);le16(f,1);le32(f,static_cast<std::uint32_t>(a.sample_rate));
    le32(f,static_cast<std::uint32_t>(a.sample_rate*2));le16(f,2);le16(f,16);
    f.write("data",4);le32(f,bytes);
    for(float x:a.samples){
        if(!std::isfinite(x)) x=0;
        auto clamped=std::clamp(x,-1.0f,1.0f);
        auto v=static_cast<std::int16_t>(std::round(clamped*32767.0f));
        le16(f,static_cast<std::uint16_t>(v));
    }
    if(!f) throw std::runtime_error("WAV write failed");
}
}
