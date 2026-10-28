#pragma once
#include <cstdint>
#include <string>

struct ID3D11Texture2D;
struct ID3D11Device;

namespace miniwp {

struct VideoFrame {
    ID3D11Texture2D* texture = nullptr;  // GPU texture (normally NV12)
    int texture_index = 0;               // Array slice index inside texture array
    int64_t pts = 0;                     // Presentation timestamp (microseconds)
    int width = 0;
    int height = 0;
};

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration_seconds = 0.0;
    std::string codec_name;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    
    // Open video file. d3d_device is used for hardware decoding.
    virtual bool Open(const char* path, ID3D11Device* d3d_device) = 0;
    
    // Decode next frame. Returns true if a new frame is available.
    // frame.texture contains the decoded texture in GPU VRAM.
    virtual bool DecodeNextFrame(VideoFrame& frame) = 0;
    
    // Seek to beginning of video for seamless looping
    virtual void SeekToStart() = 0;

    // Seek to an absolute position (seconds) inside the current stream.
    // Best effort: decoding resumes at the nearest keyframe at or before the
    // target. Used to restore the playback position after a deep suspend.
    virtual void SeekTo(double seconds) { (void)seconds; }
    
    // Get video information
    virtual VideoInfo GetInfo() const = 0;
    
    // Human-readable detail for the most recent decoder failure.
    virtual std::string GetLastError() const { return {}; }
    
    virtual void Close() = 0;
};

} // namespace miniwp
