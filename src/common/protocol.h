#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <stdexcept>

namespace acah {
constexpr uint32_t kMagic = 0x48584341;
constexpr uint32_t kVersion = 2;
// Capacity, not a scaling target. Preserve the native size; reject larger images.
constexpr uint32_t kWidth = 2560, kHeight = 1440;
constexpr uint32_t kImageBytes = kWidth * kHeight * 4;
constexpr wchar_t kMappingName[] = L"Local\\ACAHVR_POC_v2";
constexpr wchar_t kMutexName[] = L"Local\\ACAHVR_POC_v2_Init";

// Wire fields use fixed sizes. No pointers, HANDLEs, bools, or native-size integers.
#pragma pack(push, 8)
struct Pose { float orientation[4]; float position[3]; float reserved; };
struct Fov { float left, right, up, down; };
struct Tracking {
    uint32_t serial, stereoRequested, valid, originGeneration;
    int64_t displayTime;
    Pose eyes[2];
    Fov fovs[2];
    Pose origin;
};
struct Frame {
    volatile LONG state; // 0 free, 1 writing, 2 ready, 3 reading
    uint32_t serial, width, height, stereo;
    uint32_t transformedDraws, totalDraws, cloneFailures, querySkippedDraws;
    uint32_t projectionValid, originGeneration, hudDraws; // uses the former reserved field; layout unchanged
    int64_t displayTime;
    Pose eyes[2];
    Fov fovs[2];
    float transferMilliseconds;
    uint32_t sourceFormat;
    uint8_t pixels[2][kImageBytes]; // tightly packed BGRA8; composition ignores alpha
};
struct Channel {
    uint32_t magic, version, bytes;
    volatile LONG hostPid;
    volatile LONG hostHeartbeat, gamePid, frameSerial, trackingSequence;
    Tracking tracking;
    Frame frames[2]; // one writer and one reader; overwrite unread imagery
};
#pragma pack(pop)
static_assert(sizeof(Pose) == 32 && sizeof(Fov) == 16);
static_assert(sizeof(Tracking) == 152);
static_assert(offsetof(Frame, pixels) == 160);
static_assert(offsetof(Channel, frames) == 184);

class Mapping {
    HANDLE mapping_ = nullptr;
    Channel* channel_ = nullptr;
public:
    Mapping() {
        HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
        if (!mutex) throw std::runtime_error("CreateMutex failed");
        DWORD wait = WaitForSingleObject(mutex, 1000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex); throw std::runtime_error("IPC initialization timed out");
        }
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Channel), kMappingName);
        DWORD error = GetLastError();
        if (mapping_) channel_ = static_cast<Channel*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Channel)));
        if (channel_ && error != ERROR_ALREADY_EXISTS) {
            std::memset(channel_, 0, sizeof(Channel));
            channel_->version = kVersion; channel_->bytes = sizeof(Channel);
            MemoryBarrier(); channel_->magic = kMagic;
        }
        bool valid = channel_ && channel_->magic == kMagic && channel_->version == kVersion && channel_->bytes == sizeof(Channel);
        ReleaseMutex(mutex); CloseHandle(mutex);
        if (!valid) {
            if (channel_) UnmapViewOfFile(channel_);
            if (mapping_) CloseHandle(mapping_);
            channel_ = nullptr; mapping_ = nullptr;
            throw std::runtime_error("IPC mapping creation or protocol validation failed");
        }
    }
    ~Mapping() { if (channel_) UnmapViewOfFile(channel_); if (mapping_) CloseHandle(mapping_); }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Channel* get() const { return channel_; }
};

inline bool hostAlive(Channel* c) {
    return c && static_cast<uint32_t>(GetTickCount() - static_cast<DWORD>(c->hostHeartbeat)) < 1500 && c->hostHeartbeat != 0;
}
inline bool readTracking(Channel* c, Tracking& output) {
    for (int attempt = 0; attempt != 3; ++attempt) {
        LONG before = InterlockedCompareExchange(&c->trackingSequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier(); std::memcpy(&output, &c->tracking, sizeof(output)); MemoryBarrier();
        if (before == InterlockedCompareExchange(&c->trackingSequence, 0, 0)) return true;
    }
    return false;
}
inline void writeTracking(Channel* c, const Tracking& input) {
    InterlockedIncrement(&c->trackingSequence);
    MemoryBarrier(); std::memcpy(&c->tracking, &input, sizeof(input)); MemoryBarrier();
    InterlockedIncrement(&c->trackingSequence);
    InterlockedExchange(&c->hostHeartbeat, static_cast<LONG>(GetTickCount()));
}
inline Frame* claimWrite(Channel* c) {
    for (auto& f : c->frames) if (InterlockedCompareExchange(&f.state, 1, 0) == 0) return &f;
    // Dropping an unread frame is preferable to blocking the game.
    for (auto& f : c->frames) if (InterlockedCompareExchange(&f.state, 1, 2) == 2) return &f;
    return nullptr;
}
inline void publish(Frame* f) { MemoryBarrier(); InterlockedExchange(&f->state, 2); }
inline Frame* claimLatest(Channel* c) {
    Frame* newest = nullptr;
    for (auto& f : c->frames) {
        if (InterlockedCompareExchange(&f.state, 3, 2) != 2) continue;
        if (!newest || static_cast<int32_t>(f.serial - newest->serial) > 0) {
            if (newest) InterlockedExchange(&newest->state, 0);
            newest = &f;
        } else InterlockedExchange(&f.state, 0);
    }
    return newest;
}
}
