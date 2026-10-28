#pragma once

#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <vector>

namespace miniwp {

class D3D11Presenter {
public:
    ~D3D11Presenter();

    bool Init(HWND window, int width, int height);
    bool RenderFrame(ID3D11Texture2D* texture, int array_index,
                     int scaling_mode = 0);
    HRESULT Present(UINT sync_interval = 0);
    bool Resize(int width, int height);
    void ReleaseFrameResources();
    void Cleanup();

    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    HANDLE GetWaitableObject() const { return m_frame_waitable; }

private:
    bool CreateSwapChain();
    bool CreateVideoProcessor(UINT input_width, UINT input_height);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swap_chain;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_video_device;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_video_context;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_output_view;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_input_texture;
    std::vector<Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>> m_input_views;

    HWND m_window = nullptr;
    HANDLE m_frame_waitable = nullptr;
    int m_width = 0;
    int m_height = 0;
    UINT m_input_width = 0;
    UINT m_input_height = 0;
};

} // namespace miniwp
