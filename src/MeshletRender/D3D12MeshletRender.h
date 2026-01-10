#pragma once

#include "DXSample.h"
#include "Model.h"
#include "StepTimer.h"
#include "SimpleCamera.h"

using namespace DirectX;


using Microsoft::WRL::ComPtr;

class D3D12MeshletRender : public DXSample
{
public:
    D3D12MeshletRender(UINT width, UINT height, std::wstring name);

    virtual void OnInit();
    virtual void OnUpdate();
    virtual void OnRender();
    virtual void OnDestroy();
    virtual void OnKeyDown(UINT8 key);
    virtual void OnKeyUp(UINT8 key);
    void OnMouseDown(UINT8 button, int x, int y) override;
    void OnMouseUp(UINT8 button, int x, int y) override;
    void OnMouseMove(int x, int y) override;


private:
    static const UINT FrameCount = 2;

    _declspec(align(256u)) struct SceneConstantBuffer
    {
        XMFLOAT4X4 World;
        XMFLOAT4X4 WorldView;
        XMFLOAT4X4 WorldViewProj;
        uint32_t   DrawMeshlets;
    };

    // Pipeline objects.
    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Device2> m_device;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12Resource> m_depthStencil;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[FrameCount];
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12Resource> m_constantBuffer;
    UINT m_rtvDescriptorSize;
    UINT m_dsvDescriptorSize;

    ComPtr<ID3D12GraphicsCommandList6> m_commandList;
    SceneConstantBuffer m_constantBufferData;
    UINT8* m_cbvDataBegin;

    StepTimer m_timer;
    SimpleCamera m_camera;
    Model m_model;

    // Synchronization objects.
    UINT m_frameIndex;
    UINT m_frameCounter;
    HANDLE m_fenceEvent;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValues[FrameCount];

    void LoadPipeline();
    void LoadAssets();
    void PopulateCommandList();
    void MoveToNextFrame();
    void WaitForGpu();

private:
    static const wchar_t* c_meshFilename;
    static const wchar_t* c_meshShaderFilename;
    static const wchar_t* c_pixelShaderFilename;

    struct AlbedoBinding
    {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Heap; // shader-visible SRV heap (1 descriptor)
        Microsoft::WRL::ComPtr<ID3D12Resource> Tex; // default heap texture
        Microsoft::WRL::ComPtr<ID3D12Resource> Upload; // upload heap staging
    };

    AlbedoBinding m_albedo;
    UINT m_cbvSrvUavInc = 0;

    void InitAlbedoResources();
    void BindAlbedoTexture();
};
