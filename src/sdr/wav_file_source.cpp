#include "sdr/wav_file_source.h"

#include "util/log.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

uint32_t rd32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
uint16_t rd16(const unsigned char* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
uint64_t rd64(const unsigned char* p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

} // namespace

WavFileSource::~WavFileSource()
{
    stop();
}

bool WavFileSource::start(int, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;

    std::ifstream f(path_, std::ios::binary);
    if (!f)
    {
        err = "Cannot open file: " + path_;
        return false;
    }

    unsigned char hdr[12];
    f.read((char*)hdr, 12);
    if (!f || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
    {
        err = "Not a RIFF/WAVE file";
        return false;
    }

    bool haveFmt = false, haveData = false;
    wavCenterFreq_ = 0.0;
    while (f)
    {
        unsigned char ch[8];
        f.read((char*)ch, 8);
        if (!f)
            break;
        uint32_t sz = rd32(ch + 4);

        if (std::memcmp(ch, "fmt ", 4) == 0)
        {
            std::vector<unsigned char> fmt(sz);
            f.read((char*)fmt.data(), sz);
            if (sz >= 16)
            {
                channels_ = rd16(fmt.data() + 2);
                sampleRate_ = (double)rd32(fmt.data() + 4);
                bits_ = rd16(fmt.data() + 14);
            }
            haveFmt = true;
            if (sz & 1) f.seekg(1, std::ios::cur);
        }
        else if (std::memcmp(ch, "data", 4) == 0)
        {
            dataOffset_ = (uint64_t)f.tellg();
            dataBytes_ = sz;
            haveData = true;
            break;
        }
        else if (std::memcmp(ch, "auxi", 4) == 0 && sz >= 16)
        {
            std::vector<unsigned char> aux(std::min<uint32_t>(sz, 128u));
            f.read((char*)aux.data(), aux.size());
            wavCenterFreq_ = (double)rd64(aux.data() + 8);
            if (sz > aux.size())
                f.seekg(sz - aux.size(), std::ios::cur);
            if (sz & 1) f.seekg(1, std::ios::cur);
        }
        else
        {
            f.seekg(sz + (sz & 1), std::ios::cur);
        }
    }

    // If auxi didn't provide center freq, extract from filename (SDR++ convention)
    if (wavCenterFreq_ == 0.0)
    {
        std::string name = path_;
        auto pos = name.rfind('\\');
        if (pos == std::string::npos) pos = name.rfind('/');
        if (pos != std::string::npos) name = name.substr(pos + 1);
        for (size_t i = 0; i + 1 < name.size(); ++i)
        {
            if (std::tolower((unsigned char)name[i]) != 'h' ||
                std::tolower((unsigned char)name[i + 1]) != 'z')
                continue;
            size_t j = i;
            while (j > 0 && name[j - 1] >= '0' && name[j - 1] <= '9')
                --j;
            std::string digits = name.substr(j, i - j);
            if (!digits.empty())
            {
                wavCenterFreq_ = std::stod(digits);
                break;
            }
        }
    }

    if (!haveFmt || !haveData)
    {
        err = "Missing fmt/data chunk";
        return false;
    }
    if (channels_ < 1 || channels_ > 2 || (bits_ != 8 && bits_ != 16))
    {
        err = "Unsupported WAV (need 8/16-bit, 1/2 ch)";
        return false;
    }

    logWrite("[wav] opened %dch %d-bit %g Hz",
             channels_, bits_, sampleRate_);

    f.clear();
    f.seekg(0, std::ios::end);
    uint64_t fileSize = (uint64_t)f.tellg();
    if (fileSize > dataOffset_)
    {
        uint64_t physical = fileSize - dataOffset_;
        if (dataBytes_ == 0 || dataBytes_ > physical)
            dataBytes_ = physical;
    }

    const int frameBytes = channels_ * (bits_ / 8);
    totalFrames_ = dataBytes_ / (uint64_t)frameBytes;

    seekPending_.store(false);
    seekTarget_.store(0);
    currentFrame_.store(0);

    cb_ = std::move(cb);
    progress_.store(0.0);
    running_.store(true);
    thread_ = std::thread([this]() { playLoop(); });
    return true;
}

void WavFileSource::stop()
{
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
    cb_ = nullptr;
}

void WavFileSource::seekToFrame(uint64_t frame)
{
    if (frame >= totalFrames_)
        frame = totalFrames_ > 0 ? totalFrames_ - 1 : 0;
    seekTarget_.store(frame);
    seekPending_.store(true);
}

void WavFileSource::playLoop()
{
    std::ifstream f(path_, std::ios::binary);
    if (!f)
    {
        running_.store(false);
        return;
    }

    const int frameBytes = channels_ * (bits_ / 8);
    const int kFrames = 32768;

    std::vector<unsigned char> raw((size_t)kFrames * frameBytes);
    std::vector<float> iq((size_t)kFrames * 2);

    f.seekg((std::streamoff)dataOffset_, std::ios::beg);
    uint64_t framePos = 0;

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    uint64_t played = 0;

    while (running_.load())
    {
        if (seekPending_.load())
        {
            seekPending_.store(false);
            uint64_t target = seekTarget_.load();
            f.clear();
            f.seekg((std::streamoff)(dataOffset_ + target * frameBytes), std::ios::beg);
            framePos = target;
            t0 = clock::now();
            played = 0;
            currentFrame_.store(target);
            progress_.store(totalFrames_ ? (double)target / (double)totalFrames_ : 0.0);
            if (onSeek_)
                onSeek_();
        }

        uint64_t remaining = totalFrames_ - framePos;
        if (remaining == 0)
        {
            if (loop_.load())
            {
                f.clear();
                f.seekg((std::streamoff)dataOffset_, std::ios::beg);
                framePos = 0;
                t0 = clock::now();
                played = 0;
                continue;
            }
            break;
        }

        int want = (int)std::min<uint64_t>(kFrames, remaining);
        f.read((char*)raw.data(), (std::streamsize)want * frameBytes);
        std::streamsize got = f.gcount();
        int frames = (int)(got / frameBytes);
        if (frames <= 0)
            break;

        for (int i = 0; i < frames; ++i)
        {
            const unsigned char* p = raw.data() + (size_t)i * frameBytes;
            float vi, vq;
            if (bits_ == 8)
            {
                vi = ((int)p[0] - 128) * (1.0f / 128.0f);
                vq = (channels_ == 2) ? ((int)p[1] - 128) * (1.0f / 128.0f) : 0.0f;
            }
            else
            {
                int16_t s0 = (int16_t)rd16(p);
                int16_t s1 = (channels_ == 2) ? (int16_t)rd16(p + 2) : 0;
                vi = s0 * (1.0f / 32768.0f);
                vq = (channels_ == 2) ? s1 * (1.0f / 32768.0f) : 0.0f;
            }
            iq[(size_t)i * 2] = vi;
            iq[(size_t)i * 2 + 1] = vq;
        }

        if (cb_)
            cb_(iq.data(), frames);

        framePos += frames;
        played += frames;
        currentFrame_.store(framePos);
        progress_.store(totalFrames_ ? (double)framePos / (double)totalFrames_ : 0.0);

        auto target = t0 + std::chrono::duration_cast<clock::duration>(
                               std::chrono::duration<double>((double)played / sampleRate_));
        std::this_thread::sleep_until(target);
    }

    running_.store(false);
}
