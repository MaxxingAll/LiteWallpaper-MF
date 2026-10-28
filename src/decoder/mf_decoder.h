#pragma once

#include "video_decoder.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <string>

namespace miniwp {

class MFDecoder final : public IVideoDecoder {
public:
    MFDecoder() = default;
    ~MFDecoder() override;

    bool Open(const char* path, ID3D11Device* d3d_device) override;
    bool DecodeNextFrame(VideoFrame& frame) override;
    void SeekToStart() override;
    void SeekTo(double seconds) override;
    VideoInfo GetInfo() const override { return m_info; }
    std::string GetLastError() const override { return m_last_error; }
    void Close() override;

private:
    bool ConfigureVideoType();
    bool ReadVideoInfo();
    void SetError(HRESULT hr, const char* operation);

    Microsoft::WRL::ComPtr<IMFSourceReader> m_reader;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_device_manager;
    Microsoft::WRL::ComPtr<IMFSample> m_current_sample;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;

    VideoInfo m_info;
    std::string m_last_error;
    std::wstring m_path;
    UINT m_device_reset_token = 0;
    bool m_com_initialized = false;
    bool m_mf_started = false;
};

} // namespace miniwp
