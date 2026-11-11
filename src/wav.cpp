#include "aimj/wav.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
namespace aimj {
namespace {
void le16(std::ostream& o, std::uint16_t v) {o.put(static_cast<char>(v));o.put(static_cast<char>(v>>8));}
void le32(std::ostream& o, std::uint32_t v) {for(int i=0;i<4;i++)o.put(static_cast<char>(v>>(8*i)));}
std::uint16_t read_le16(const char* p) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(p[0]) |
        (static_cast<std::uint16_t>(static_cast<unsigned char>(p[1])) << 8));
}
std::uint32_t read_le32(const char* p) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}
}
WavWriteStats write_pcm16_wav(const std::string& path,const AudioBuffer& a) {
    if(a.sample_rate<=0 || a.samples.empty() || a.samples.size()> (std::numeric_limits<std::uint32_t>::max()-36u)/2u)
        throw std::invalid_argument("invalid PCM");
    std::ofstream f(path,std::ios::binary|std::ios::trunc);
    if(!f) throw std::runtime_error("cannot open WAV path");
    auto bytes=static_cast<std::uint32_t>(a.samples.size()*2);
    f.write("RIFF",4);le32(f,36+bytes);f.write("WAVEfmt ",8);
    le32(f,16);le16(f,1);le16(f,1);le32(f,static_cast<std::uint32_t>(a.sample_rate));
    le32(f,static_cast<std::uint32_t>(a.sample_rate*2));le16(f,2);le16(f,16);
    f.write("data",4);le32(f,bytes);
    WavWriteStats stats;
    for(float x:a.samples){
        if(!std::isfinite(x)) {x=0; ++stats.non_finite_samples;}
        else if(x>1.0f || x<-1.0f) ++stats.clipped_samples;
        auto clamped=std::clamp(x,-1.0f,1.0f);
        auto v=static_cast<std::int16_t>(std::round(clamped*32767.0f));
        le16(f,static_cast<std::uint16_t>(v));
    }
    if(!f) throw std::runtime_error("WAV write failed");
    return stats;
}
WavReadResult read_pcm16_wav(const std::string& path) {
    std::ifstream f(path,std::ios::binary);
    if(!f) throw std::runtime_error("cannot open WAV: "+path);
    char header[12]{};
    f.read(header,12);
    if(f.gcount()!=12 || std::string(header,4)!="RIFF" || std::string(header+8,4)!="WAVE")
        throw std::runtime_error("not a RIFF/WAVE file: "+path);
    bool have_fmt=false, have_data=false;
    std::uint16_t format=0,channels=0,bits=0;
    std::uint32_t sample_rate=0;
    std::vector<char> data;
    while(f) {
        char chunk[8]{};
        f.read(chunk,8);
        if(f.gcount()!=8) break;
        const std::string id(chunk,4);
        std::uint32_t size=read_le32(chunk+4);
        if(id=="fmt ") {
            if(size<16) throw std::runtime_error("short fmt chunk: "+path);
            // The size comes from the file; cap it before allocating so a
            // corrupt header fails as a runtime_error instead of bad_alloc.
            if(size>1024) throw std::runtime_error("fmt chunk too large: "+path);
            std::vector<char> fmt(size);
            f.read(fmt.data(),static_cast<std::streamsize>(size));
            if(f.gcount()!=static_cast<std::streamsize>(size)) throw std::runtime_error("truncated fmt chunk: "+path);
            format=read_le16(fmt.data());channels=read_le16(fmt.data()+2);
            sample_rate=read_le32(fmt.data()+4);bits=read_le16(fmt.data()+14);
            have_fmt=true;
            if(size&1u) f.seekg(1,std::ios::cur); // RIFF chunk padding
        } else if(id=="data") {
            if(size > (1u<<28)) throw std::runtime_error("data chunk too large: "+path);
            data.resize(size);
            f.read(data.data(),static_cast<std::streamsize>(size));
            if(f.gcount()!=static_cast<std::streamsize>(size)) throw std::runtime_error("truncated data chunk: "+path);
            have_data=true;
            if(size&1u) f.seekg(1,std::ios::cur); // RIFF chunk padding
        } else {
            f.seekg(static_cast<std::streamoff>(size)+(size&1u),std::ios::cur);
        }
    }
    if(!have_fmt || !have_data) throw std::runtime_error("missing fmt or data chunk: "+path);
    if(format!=1) throw std::runtime_error("unsupported WAV format (expected PCM): "+path);
    if(channels!=1) throw std::runtime_error("unsupported channel count (expected mono): "+path);
    if(bits!=16) throw std::runtime_error("unsupported sample width (expected 16-bit): "+path);
    if(sample_rate==0) throw std::runtime_error("zero sample rate: "+path);
    WavReadResult out;
    out.audio.sample_rate=static_cast<int>(sample_rate);
    out.audio.samples.resize(data.size()/2);
    for(std::size_t i=0;i<out.audio.samples.size();++i) {
        const auto v=static_cast<std::int16_t>(read_le16(data.data()+2*i));
        if(v==32767 || v==-32768) ++out.clipped_samples;
        out.audio.samples[i]=static_cast<float>(v)/32768.0f;
    }
    return out;
}
}
