#pragma once
#include <cstdint>
#include <vector>

struct AudioInfo {
    bool present = false;
    bool adts = false;               // samples already carry ADTS headers
    int  objectType = 2, freqIndex = 3, channels = 2;
};

struct SessionInfo {
    std::vector<uint8_t> seqHeader;  // H.264 SPS/PPS from the video media type
    AudioInfo audio;
};

enum class ItemKind : uint8_t { Session, Video, Audio };

// One unit handed from the game's threads to the output worker.
struct Item {
    ItemKind kind = ItemKind::Video;
    uint32_t session = 0;
    int64_t  pts = 0, dts = 0;       // 100ns, as given to the MP4 sink
    int64_t  wall = 0;               // qpc_100ns() when tapped
    bool     key = false;
    std::vector<uint8_t> data;
    SessionInfo info;                // ItemKind::Session only
};

void output_start();
bool output_active();                // false -> nobody is listening, skip copying samples
void output_push(Item&& it);
bool output_take_idr_request();      // true once per new viewer / overflow

// Live mode: one long-running session whose description changes when our encoders (re)start.
void     live_update_video(const std::vector<uint8_t>& seqHeader);
void     live_update_audio(const AudioInfo& audio);
uint32_t live_session();
