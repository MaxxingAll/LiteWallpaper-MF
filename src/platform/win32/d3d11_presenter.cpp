#include "d3d11_presenter.h"

#include <d3d10.h>

#include <algorithm>

namespace miniwp {

D3D11Presenter::~D3D11Presenter() {
    Cleanup();
}

bool D3D11Presenter::Init(HWND window, int width, int height) {
    Cleanup();
    m_window = window;
    m_width = (std::max)(1, width);
    m_height = (std::max)(1, height);

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_11_0;
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                       D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
        static_cast<UINT>(_countof(levels)), D3D11_SDK_VERSION, &m_device,
        &selected, &m_context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &levels[1], 1,
            D3D11_SDK_VERSION, &m_device, &selected, &m_context);
    }
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }
    if (FAILED(m_device.As(&m_video_device)) ||
        FAILED(m_context.As(&m_video_context))) {
        return false;
    }
    return CreateSwapChain();
}

bool D3D11Presenter::CreateSwapChain() {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(m_device.As(&dxgi_device)) ||
        FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 description = {};
    description.Width = static_cast<UINT>(m_width);
    description.Height = static_cast<UINT>(m_height);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HRESULT hr = factory->CreateSwapChainForHwnd(
        m_device.Get(), m_window, &description, nullptr, nullptr, &m_swap_chain);
    if (FAILED(hr)) return false;
    factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);

    Microsoft::WRL::ComPtr<IDXGISwapChain2> swap_chain2;
    if (SUCCEEDED(m_swap_chain.As(&swap_chain2))) {
        swap_chain2->SetMaximumFrameLatency(1);
        m_frame_waitable = swap_chain2->GetFrameLatencyWaitableObject();
    }
    return true;
}

bool D3D11Presenter::CreateVideoProcessor(UINT input_width, UINT input_height) {
    ReleaseFrameResources();
    m_output_view.Reset();
    m_processor.Reset();
    m_enumerator.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {30, 1};
    content.InputWidth = input_width;
    content.InputHeight = input_height;
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = static_cast<UINT>(m_width);
    content.OutputHeight = static_cast<UINT>(m_height);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(m_video_device->CreateVideoProcessorEnumerator(
            &content, &m_enumerator)) ||
        FAILED(m_video_device->CreateVideoProcessor(m_enumerator.Get(), 0,
                                                     &m_processor))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_description = {};
    output_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_description.Texture2D.MipSlice = 0;
    if (FAILED(m_video_device->CreateVideoProcessorOutputView(
            back_buffer.Get(), m_enumerator.Get(), &output_description,
            &m_output_view))) {
        return false;
    }

    m_input_width = input_width;
    m_input_height = input_height;
    return true;
}

bool D3D11Presenter::RenderFrame(ID3D11Texture2D* texture, int array_index,
                                 int scaling_mode) {
    if (!texture || !m_video_context || !m_swap_chain) return false;

    D3D11_TEXTURE2D_DESC texture_description = {};
    texture->GetDesc(&texture_description);
    if (texture_description.Width == 0 || texture_description.Height == 0) {
        return false;
    }
    if (!m_processor || !m_output_view ||
        m_input_width != texture_description.Width ||
        m_input_height != texture_description.Height) {
        if (!CreateVideoProcessor(texture_description.Width,
                                  texture_description.Height)) {
            return false;
        }
    }

    if (array_index < 0) return false;
    const UINT slice = static_cast<UINT>(array_index);
    if (slice >= texture_description.ArraySize) return false;
    if (m_input_texture.Get() != texture) {
        ReleaseFrameResources();
        m_input_texture = texture;
        m_input_views.resize(texture_description.ArraySize);
    }
    if (!m_input_views[slice]) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_description = {};
        input_description.FourCC = 0;
        input_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_description.Texture2D.MipSlice = 0;
        input_description.Texture2D.ArraySlice = slice;
        if (FAILED(m_video_device->CreateVideoProcessorInputView(
                texture, m_enumerator.Get(), &input_description,
                &m_input_views[slice]))) {
            return false;
        }
    }

    RECT source = {0, 0, static_cast<LONG>(texture_description.Width),
                   static_cast<LONG>(texture_description.Height)};
    RECT destination = {0, 0, m_width, m_height};
    const double video_aspect = static_cast<double>(texture_description.Width) /
                                texture_description.Height;
    const double output_aspect = static_cast<double>(m_width) / m_height;

    if (scaling_mode == 0) {
        if (output_aspect > video_aspect) {
            const LONG height = (std::max)(
                1L, static_cast<LONG>(texture_description.Width / output_aspect));
            source.top = (static_cast<LONG>(texture_description.Height) - height) / 2;
            source.bottom = source.top + height;
        } else {
            const LONG width = (std::max)(
                1L, static_cast<LONG>(texture_description.Height * output_aspect));
            source.left = (static_cast<LONG>(texture_description.Width) - width) / 2;
            source.right = source.left + width;
        }
    } else if (scaling_mode == 1) {
        if (output_aspect > video_aspect) {
            const LONG width = (std::max)(
                1L, static_cast<LONG>(m_height * video_aspect));
            destination.left = (m_width - width) / 2;
            destination.right = destination.left + width;
        } else {
            const LONG height = (std::max)(
                1L, static_cast<LONG>(m_width / video_aspect));
            destination.top = (m_height - height) / 2;
            destination.bottom = destination.top + height;
        }
    }

    D3D11_VIDEO_COLOR background = {};
    background.RGBA.A = 1.0f;
    const RECT output_rect = {0, 0, m_width, m_height};
    m_video_context->VideoProcessorSetOutputBackgroundColor(
        m_processor.Get(), FALSE, &background);
    m_video_context->VideoProcessorSetOutputTargetRect(
        m_processor.Get(), TRUE, &output_rect);
    m_video_context->VideoProcessorSetStreamFrameFormat(
        m_processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color = {};
    const bool ycbcr_input = texture_description.Format == DXGI_FORMAT_NV12 ||
                             texture_description.Format == DXGI_FORMAT_P010 ||
                             texture_description.Format == DXGI_FORMAT_P016;
    if (ycbcr_input) {
        input_color.YCbCr_Matrix = texture_description.Height >= 720 ? 1 : 0;
        input_color.Nominal_Range = 1; // studio range (16-235)
    } else {
        input_color.Nominal_Range = 2; // full range RGB
    }
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color = {};
    output_color.Nominal_Range = 2; // full range RGB
    m_video_context->VideoProcessorSetStreamColorSpace(
        m_processor.Get(), 0, &input_color);
    m_video_context->VideoProcessorSetOutputColorSpace(
        m_processor.Get(), &output_color);
    m_video_context->VideoProcessorSetStreamAutoProcessingMode(
        m_processor.Get(), 0, FALSE);
    m_video_context->VideoProcessorSetStreamSourceRect(
        m_processor.Get(), 0, TRUE, &source);
    m_video_context->VideoProcessorSetStreamDestRect(
        m_processor.Get(), 0, TRUE, &destination);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = m_input_views[slice].Get();
    return SUCCEEDED(m_video_context->VideoProcessorBlt(
        m_processor.Get(), m_output_view.Get(), 0, 1, &stream));
}

HRESULT D3D11Presenter::Present(UINT sync_interval) {
    return m_swap_chain ? m_swap_chain->Present(sync_interval, 0) : E_POINTER;
}

bool D3D11Presenter::Resize(int width, int height) {
    width = (std::max)(1, width);
    height = (std::max)(1, height);
    if (!m_swap_chain || (width == m_width && height == m_height)) return false;
    ReleaseFrameResources();
    m_output_view.Reset();
    m_enumerator.Reset();
    m_processor.Reset();
    m_input_width = 0;
    m_input_height = 0;
    const HRESULT hr = m_swap_chain->ResizeBuffers(
        0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) return false;
    m_width = width;
    m_height = height;
    return true;
}

void D3D11Presenter::ReleaseFrameResources() {
    m_input_views.clear();
    m_input_texture.Reset();
}

void D3D11Presenter::Cleanup() {
    if (m_context) m_context->ClearState();
    if (m_frame_waitable) CloseHandle(m_frame_waitable);
    m_frame_waitable = nullptr;
    m_output_view.Reset();
    ReleaseFrameResources();
    m_processor.Reset();
    m_enumerator.Reset();
    m_video_context.Reset();
    m_video_device.Reset();
    m_swap_chain.Reset();
    m_context.Reset();
    m_device.Reset();
    m_window = nullptr;
    m_width = 0;
    m_height = 0;
    m_input_width = 0;
    m_input_height = 0;
}

} // namespace miniwp
