#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mf_decoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <propvarutil.h>
#include <d3d10.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

namespace miniwp {
namespace {

constexpr DWORD FourCC(char a, char b, char c, char d) {
    return static_cast<DWORD>(static_cast<unsigned char>(a)) |
           (static_cast<DWORD>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
}

std::wstring Utf8ToWide(const char* value) {
    if (!value || !*value) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                                    nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                        result.data(), count);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

std::string CodecName(const GUID& subtype) {
    const DWORD fourcc = subtype.Data1;
    if (fourcc == FourCC('H', '2', '6', '4') ||
        fourcc == FourCC('A', 'V', 'C', '1')) return "H.264";
    if (fourcc == FourCC('H', 'E', 'V', 'C') ||
        fourcc == FourCC('H', 'V', 'C', '1')) return "HEVC";
    if (fourcc == FourCC('V', 'P', '9', '0')) return "VP9";
    if (fourcc == FourCC('V', 'P', '8', '0')) return "VP8";
    if (fourcc == FourCC('A', 'V', '0', '1')) return "AV1";
    if (fourcc == FourCC('M', 'P', 'G', '2')) return "MPEG-2";
    return "Media Foundation video";
}

std::string LowerExtension(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::string result;
    result.reserve(ext.size());
    for (wchar_t c : ext) result.push_back(static_cast<char>(c));
    return result;
}

const char* CodecInstallHint(const std::wstring& path, const std::string& codec) {
    const std::string ext = LowerExtension(path);
    if (codec == "HEVC" || ext == ".hevc" || ext == ".h265") {
        return "Install HEVC Video Extensions, then try again.";
    }
    if (codec == "AV1" || ext == ".av1") {
        return "Install AV1 Video Extension, then try again.";
    }
    if (codec == "VP9" || ext == ".webm") {
        return "Install Web Media Extensions and VP9 Video Extensions, then try again.";
    }
    return "Install the Windows codec extension required by this video, then try again.";
}

std::string HResultHex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                  static_cast<unsigned long>(hr));
    return buffer;
}

void DebugLog(const std::string& message) {
    OutputDebugStringA("LiteWallpaper-MF: ");
    OutputDebugStringA(message.c_str());
    OutputDebugStringA("\n");
}

} // namespace

MFDecoder::~MFDecoder() {
    Close();
}

void MFDecoder::SetError(HRESULT hr, const char* operation) {
    m_last_error = std::string(operation) + " failed (" + HResultHex(hr) + ").";
    if (hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_INVALIDMEDIATYPE ||
        hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE) {
        m_last_error += ' ';
        m_last_error += CodecInstallHint(m_path, m_info.codec_name);
    }
    DebugLog(m_last_error);
}

bool MFDecoder::ConfigureVideoType() {
    Microsoft::WRL::ComPtr<IMFMediaType> native_type;
    if (SUCCEEDED(m_reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native_type))) {
        GUID native_subtype = GUID_NULL;
        if (SUCCEEDED(native_type->GetGUID(MF_MT_SUBTYPE, &native_subtype))) {
            m_info.codec_name = CodecName(native_subtype);
        }
    }

    Microsoft::WRL::ComPtr<IMFMediaType> output_type;
    HRESULT hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) {
        SetError(hr, "MFCreateMediaType");
        return false;
    }

    hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) {
        hr = m_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                           nullptr, output_type.Get());
    }
    if (FAILED(hr)) {
        // A small number of drivers expose only a BGRA DXGI output. The D3D11
        // video processor can consume that surface directly as well.
        HRESULT bgra_hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        if (SUCCEEDED(bgra_hr)) {
            bgra_hr = m_reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output_type.Get());
        }
        if (FAILED(bgra_hr)) {
            SetError(hr, "Selecting NV12 or BGRA video output");
            return false;
        }
    }
    return true;
}

bool MFDecoder::ReadVideoInfo() {
    Microsoft::WRL::ComPtr<IMFMediaType> native_type;
    HRESULT hr = m_reader->GetNativeMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native_type);
    if (FAILED(hr)) {
        SetError(hr, "Reading native video type");
        return false;
    }

    GUID subtype = GUID_NULL;
    if (SUCCEEDED(native_type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        m_info.codec_name = CodecName(subtype);
    }

    Microsoft::WRL::ComPtr<IMFMediaType> current_type;
    hr = m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                       &current_type);
    if (FAILED(hr)) {
        SetError(hr, "Reading configured video type");
        return false;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    if (SUCCEEDED(MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE,
                                     &width, &height))) {
        m_info.width = static_cast<int>(width);
        m_info.height = static_cast<int>(height);
    }

    UINT32 fps_numerator = 0;
    UINT32 fps_denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(current_type.Get(), MF_MT_FRAME_RATE,
                                      &fps_numerator, &fps_denominator)) &&
        fps_denominator != 0) {
        m_info.fps = static_cast<double>(fps_numerator) / fps_denominator;
    }

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(m_reader->GetPresentationAttribute(
            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration))) {
        if (duration.vt == VT_UI8) {
            m_info.duration_seconds = static_cast<double>(duration.uhVal.QuadPart) / 10000000.0;
        } else if (duration.vt == VT_I8) {
            m_info.duration_seconds = static_cast<double>(duration.hVal.QuadPart) / 10000000.0;
        }
    }
    PropVariantClear(&duration);

    if (m_info.width <= 0 || m_info.height <= 0) {
        SetError(MF_E_INVALIDMEDIATYPE, "Reading video dimensions");
        return false;
    }
    return true;
}

bool MFDecoder::Open(const char* path, ID3D11Device* d3d_device) {
    Close();
    m_last_error.clear();
    m_info = {};
    m_path = Utf8ToWide(path);
    if (m_path.empty()) {
        SetError(E_INVALIDARG, "Converting the video path to UTF-16");
        return false;
    }
    if (!d3d_device) {
        SetError(E_POINTER, "Opening Media Foundation without a D3D11 device");
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        m_com_initialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        SetError(hr, "Initializing COM for Media Foundation");
        return false;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        SetError(hr, "MFStartup");
        Close();
        return false;
    }
    m_mf_started = true;
    m_device = d3d_device;

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    hr = MFCreateDXGIDeviceManager(&m_device_reset_token, &m_device_manager);
    if (SUCCEEDED(hr)) {
        hr = m_device_manager->ResetDevice(m_device.Get(), m_device_reset_token);
    }
    if (FAILED(hr)) {
        SetError(hr, "Creating the Media Foundation D3D manager");
        Close();
        return false;
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,
                                    m_device_manager.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT32(
            MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    }
    if (FAILED(hr)) {
        SetError(hr, "Configuring Media Foundation attributes");
        Close();
        return false;
    }

    hr = MFCreateSourceReaderFromURL(m_path.c_str(), attributes.Get(), &m_reader);
    if (FAILED(hr)) {
        SetError(hr, "Opening the video with Media Foundation");
        Close();
        return false;
    }

    hr = m_reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) {
        hr = m_reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    }
    if (FAILED(hr)) {
        SetError(hr, "Selecting the video stream");
        Close();
        return false;
    }

    if (!ConfigureVideoType() || !ReadVideoInfo()) {
        Close();
        return false;
    }

    // With a D3D manager configured, decoded samples are expected as DXGI
    // surfaces. DecodeNextFrame verifies this before reporting a frame.
    DebugLog(std::string("opened ") + path + " codec=" + m_info.codec_name);
    return true;
}

bool MFDecoder::DecodeNextFrame(VideoFrame& frame) {
    frame = {};
    if (!m_reader) return false;

    bool looped = false;
    for (;;) {
        DWORD actual_stream = 0;
        DWORD flags = 0;
        LONGLONG timestamp_100ns = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        HRESULT hr = m_reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actual_stream, &flags,
            &timestamp_100ns, &sample);
        if (FAILED(hr)) {
            SetError(hr, "Reading a decoded video sample");
            return false;
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            if (!ReadVideoInfo()) return false;
        }
        if (!sample && (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            m_current_sample.Reset();
            if (looped) {
                SetError(E_FAIL, "Decoding video frames");
                return false;
            }
            looped = true;
            SeekToStart();
            if (!m_last_error.empty()) return false;
            continue;
        }
        if (!sample) continue;

        DWORD buffer_count = 0;
        hr = sample->GetBufferCount(&buffer_count);
        if (FAILED(hr)) {
            SetError(hr, "Reading Media Foundation sample buffers");
            return false;
        }

        for (DWORD i = 0; i < buffer_count; ++i) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
            Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgi_buffer;
            if (FAILED(sample->GetBufferByIndex(i, &buffer)) ||
                FAILED(buffer.As(&dxgi_buffer))) {
                continue;
            }

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            hr = dxgi_buffer->GetResource(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(texture.GetAddressOf()));
            if (FAILED(hr)) continue;

            UINT subresource = 0;
            hr = dxgi_buffer->GetSubresourceIndex(&subresource);
            if (FAILED(hr)) {
                SetError(hr, "Reading the decoded texture subresource");
                return false;
            }

            LONGLONG sample_time = timestamp_100ns;
            sample->GetSampleTime(&sample_time);
            m_current_sample = sample;
            frame.texture = texture.Get();
            frame.texture_index = static_cast<int>(subresource);
            frame.pts = sample_time / 10;
            frame.width = m_info.width;
            frame.height = m_info.height;
            m_last_error.clear();
            return true;
        }

        SetError(E_NOINTERFACE,
                 "Obtaining a DXGI surface from the decoded sample");
        return false;
    }
}

void MFDecoder::SeekToStart() {
    SeekTo(0.0);
}

void MFDecoder::SeekTo(double seconds) {
    if (!m_reader) return;
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;

    m_current_sample.Reset();
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(seconds * 10000000.0);
    const HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    if (FAILED(hr)) {
        SetError(hr, "Seeking the Media Foundation source reader");
    } else {
        m_last_error.clear();
    }
}

void MFDecoder::Close() {
    m_current_sample.Reset();
    if (m_reader) {
        m_reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    }
    m_reader.Reset();
    m_device_manager.Reset();
    m_device.Reset();

    if (m_mf_started) {
        MFShutdown();
        m_mf_started = false;
    }
    if (m_com_initialized) {
        CoUninitialize();
        m_com_initialized = false;
    }

    m_device_reset_token = 0;
    m_info = {};
    m_path.clear();
}

} // namespace miniwp
