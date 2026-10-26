#pragma once
#include <cstdint>
#include <map>
#include <vector>

// Minimal MPEG-TS muxer: one program, H.264 video (PID 0x100, carries PCR) and ADTS AAC (PID 0x101).
// Timestamps are 90 kHz.
class TsMuxer {
public:
    void set_audio(bool present);
    void psi(std::vector<uint8_t>& out);
    void video(std::vector<uint8_t>& out, const std::vector<uint8_t>& au, int64_t pts, int64_t dts, bool key);
    void audio(std::vector<uint8_t>& out, const std::vector<uint8_t>& adtsFrame, int64_t pts);

private:
    void section(std::vector<uint8_t>& out, uint16_t pid, std::vector<uint8_t> sec);
    void packetize(std::vector<uint8_t>& out, uint16_t pid, const std::vector<uint8_t>& pes,
                   int64_t pcr, bool randomAccess);

    std::map<uint16_t, uint8_t> cc_;
    bool    audio_ = false;
    uint8_t version_ = 0;
};
