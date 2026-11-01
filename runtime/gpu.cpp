#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string.h>
#include "gpu.h"

using Microsoft::WRL::ComPtr;
namespace {
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGISwapChain> swapchain;
ComPtr<ID3D11RenderTargetView> target;
ComPtr<ID3D11Texture2D> texture;
ComPtr<ID3D11Texture2D> readback;
ComPtr<ID3D11ShaderResourceView> texture_view;
ComPtr<ID3D11VertexShader> vertex_shader;
ComPtr<ID3D11PixelShader> pixel_shader;
ComPtr<ID3D11SamplerState> sampler;
ComPtr<ID3D11Buffer> constants;
ComPtr<ID3D11RasterizerState> rasterizer;
int client_w=0,client_h=0,source_w=0,source_h=0;
bool failed=false;

bool resize(int w,int h) {
    if (target && w==client_w && h==client_h) return true;
    context->OMSetRenderTargets(0,nullptr,nullptr);
    target.Reset();
    readback.Reset();
    if (FAILED(swapchain->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0))) return false;
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(swapchain->GetBuffer(0,IID_PPV_ARGS(&buffer)))) return false;
    if (FAILED(device->CreateRenderTargetView(buffer.Get(),nullptr,&target))) return false;
    client_w=w; client_h=h; return true;
}

bool initialize(HWND hwnd,int w,int h) {
    DXGI_SWAP_CHAIN_DESC desc={};
    desc.BufferDesc.Width=w; desc.BufferDesc.Height=h;
    desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.OutputWindow=hwnd; desc.Windowed=TRUE;
    desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&desc,
        &swapchain,&device,&level,&context))) return false;
    ComPtr<IDXGIDevice> dxgi_device; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter))
        && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);
    static const char shader[]=R"(
        cbuffer Parameters : register(b0) { float4 crop; };
        struct Varying { float4 position:SV_POSITION; float2 uv:TEXCOORD0; };
        Varying vs(uint id:SV_VertexID) {
            float2 p=float2((id<<1)&2,id&2);
            Varying v;
            v.position=float4(p*float2(2,-2)+float2(-1,1),0,1);
            v.uv=float2(p.x,crop.x+p.y*crop.y);
            return v;
        }
        Texture2D image:register(t0); SamplerState point_sampler:register(s0);
        float4 ps(Varying v):SV_TARGET { return float4(image.Sample(point_sampler,v.uv).rgb,1); }
    )";
    ComPtr<ID3DBlob> vs,ps,errors;
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errors))) return false;
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&errors))) return false;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex_shader))
        || FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel_shader))) return false;
    D3D11_SAMPLER_DESC sample={};
    sample.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sample.MaxLOD=D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sample,&sampler))) return false;
    D3D11_BUFFER_DESC constant_desc={};
    constant_desc.ByteWidth=16; constant_desc.Usage=D3D11_USAGE_DEFAULT;
    constant_desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&constant_desc,nullptr,&constants))) return false;
    D3D11_RASTERIZER_DESC raster={};
    raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
    if (FAILED(device->CreateRasterizerState(&raster,&rasterizer))) return false;
    return resize(w,h);
}

bool source_texture(int w,int h) {
    if (texture && source_w==w && source_h==h) return true;
    texture_view.Reset(); texture.Reset();
    D3D11_TEXTURE2D_DESC desc={};
    desc.Width=w; desc.Height=h; desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DYNAMIC; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture))
        || FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&texture_view))) return false;
    source_w=w; source_h=h; return true;
}
} // namespace

void gpu_shutdown() {
    if (context) context->ClearState();
    texture_view.Reset(); texture.Reset(); target.Reset(); readback.Reset();
    constants.Reset(); sampler.Reset(); rasterizer.Reset();
    vertex_shader.Reset(); pixel_shader.Reset();
    swapchain.Reset(); context.Reset(); device.Reset();
    client_w=client_h=source_w=source_h=0;
}

bool gpu_present(HWND hwnd,const uint32_t *pixels,int w,int h,
                 const RECT &dest,const RECT &client,int top,int visible_height,bool capture_for_test) {
    if (failed) return false;
    bool ok=device ? resize(client.right,client.bottom) : initialize(hwnd,client.right,client.bottom);
    if (!ok || !source_texture(w,h)) { gpu_shutdown(); failed=true; return false; }
    D3D11_MAPPED_SUBRESOURCE map={};
    if (FAILED(context->Map(texture.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map))) {
        gpu_shutdown(); failed=true; return false;
    }
    for (int y=0;y<h;++y)
        memcpy((uint8_t *)map.pData+y*map.RowPitch,pixels+y*w,w*sizeof(uint32_t));
    context->Unmap(texture.Get(),0);
    const float black[4]={0,0,0,1};
    context->ClearRenderTargetView(target.Get(),black);
    float crop[4]={float(top)/h,float(visible_height)/h,0,0};
    context->UpdateSubresource(constants.Get(),0,nullptr,crop,0,0);
    D3D11_VIEWPORT viewport={float(dest.left),float(dest.top),float(dest.right-dest.left),
        float(dest.bottom-dest.top),0,1};
    context->RSSetViewports(1,&viewport); context->RSSetState(rasterizer.Get());
    ID3D11RenderTargetView *rt=target.Get(); context->OMSetRenderTargets(1,&rt,nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_shader.Get(),nullptr,0);
    context->PSSetShader(pixel_shader.Get(),nullptr,0);
    ID3D11Buffer *cb=constants.Get(); context->VSSetConstantBuffers(0,1,&cb);
    ID3D11SamplerState *ss=sampler.Get(); context->PSSetSamplers(0,1,&ss);
    ID3D11ShaderResourceView *srv=texture_view.Get(); context->PSSetShaderResources(0,1,&srv);
    context->Draw(3,0);
    if (capture_for_test) {
        // Capture before Present: DISCARD swapchain contents are undefined
        // afterwards. This path is never enabled during normal gameplay.
        ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(swapchain->GetBuffer(0,IID_PPV_ARGS(&buffer)))) return false;
        if (!readback) {
            D3D11_TEXTURE2D_DESC desc; buffer->GetDesc(&desc);
            desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=desc.MiscFlags=0;
            desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&desc,nullptr,&readback))) return false;
        }
        context->CopyResource(readback.Get(),buffer.Get());
    }
    // Main loop already paces at the NES rate; do not introduce a second wait.
    if (FAILED(swapchain->Present(0,0))) { gpu_shutdown(); failed=true; return false; }
    return true;
}

bool gpu_readback(uint32_t *pixels,int w,int h) {
    if (!readback || w!=client_w || h!=client_h) return false;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&map))) return false;
    for (int y=0;y<h;++y) memcpy(pixels+y*w,(uint8_t *)map.pData+y*map.RowPitch,w*4);
    context->Unmap(readback.Get(),0);
    return true;
}
