// SPDX-License-Identifier: GPL-3.0-only
#include "RenderDepth.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include <string>
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr,std::source_location where=std::source_location::current()) {
    if (FAILED(hr)) throw std::runtime_error("D3D call failed at line "+std::to_string(where.line())+": HRESULT "+std::to_string(static_cast<unsigned>(hr)));
}
void Require(bool ok) { if (!ok) throw std::runtime_error("depth regression"); }
struct Constants { float matrix[4][4]; float z,color[3]; };
int main() try {
    struct Projection { float m[4][4]{}; } projection;
    projection.m[2][3]=1; projection.m[3][2]=-5; projection.m[2][2]=1;
    Require(!Aottg::Render::ReverseProjection(projection));
    projection.m[2][2]=-.001f;
    projection.m[3][2]=5;
    Require(Aottg::Render::ReverseProjection(projection));
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> immediate,context;
    Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
    Check(device->CreateDeferredContext(0,&context));
    const char shader[]=R"(
cbuffer C:register(b0) { row_major float4x4 viewProj; float z; float3 color; };
struct P { float4 p:SV_Position; };
P VS(uint id:SV_VertexID) { P o; o.p=mul(viewProj,float4(0,0,z,1));
o.p.xy=float2(id==1?3:-1,id==2?3:-1)*o.p.w; return o; }
float4 PS(P p):SV_Target { return float4(color,1); }
)";
    ComPtr<ID3DBlob> vcode,pcode; ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    Check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vcode,nullptr));
    Check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&pcode,nullptr));
    Check(device->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,&vs));
    Check(device->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,&ps));
    D3D11_TEXTURE2D_DESC td{}; td.Width=td.Height=4; td.ArraySize=td.MipLevels=1;
    td.SampleDesc.Count=1; td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,readback,depth; ComPtr<ID3D11RenderTargetView> rtv;
    Check(device->CreateTexture2D(&td,nullptr,&target)); Check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&td,nullptr,&readback));
    td.Format=DXGI_FORMAT_D32_FLOAT; td.BindFlags=D3D11_BIND_DEPTH_STENCIL; td.Usage=D3D11_USAGE_DEFAULT; td.CPUAccessFlags=0;
    Check(device->CreateTexture2D(&td,nullptr,&depth)); ComPtr<ID3D11DepthStencilView> dsv;
    Check(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(Constants); bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb; Check(device->CreateBuffer(&bd,nullptr,&cb));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=true;
    ComPtr<ID3D11RasterizerState> raster; Check(device->CreateRasterizerState(&rd,&raster));
    D3D11_VIEWPORT viewport{0,0,4,4,0,1};
    // Check both conversions, already-matching matrices, and transparent depth writes.
    for(bool sourceReversed:{false,true}) for(bool reversed:{false,true}) {
        Constants c{}; for(int i=0;i<4;++i)c.matrix[i][i]=1;
        if(sourceReversed) { c.matrix[2][2]=-1; c.matrix[2][3]=1; }
        Aottg::Render::MatchDepthConvention(c.matrix,sourceReversed,reversed);
        Require(c.matrix[0][0]==1 && c.matrix[1][1]==1 && c.matrix[3][3]==1);
        Require(std::abs(c.matrix[2][2]*(.1f)+c.matrix[2][3]-(reversed?.9f:.1f))<1e-6f);
        D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable=true; dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc=reversed?D3D11_COMPARISON_GREATER_EQUAL:D3D11_COMPARISON_LESS_EQUAL;
        ComPtr<ID3D11DepthStencilState> opaque,smoke;
        Check(device->CreateDepthStencilState(&dd,&opaque)); dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        Check(device->CreateDepthStencilState(&dd,&smoke));
        // A sentinel immediate state must survive ExecuteCommandList(restore=true).
        immediate->OMSetDepthStencilState(smoke.Get(),17);
        auto rt=rtv.Get(); context->OMSetRenderTargets(1,&rt,dsv.Get());
        const float clear[]={0,0,0,0}; context->ClearRenderTargetView(rt,clear);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,reversed?0.f:1.f,0);
        context->RSSetViewports(1,&viewport); context->RSSetState(raster.Get());
        context->VSSetShader(vs.Get(),nullptr,0); context->PSSetShader(ps.Get(),nullptr,0);
        auto buffer=cb.Get(); context->VSSetConstantBuffers(0,1,&buffer); context->PSSetConstantBuffers(0,1,&buffer);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto draw=[&](float z,float red,float green,ID3D11DepthStencilState* state) {
            c.z=z; c.color[0]=red; c.color[1]=green;
            context->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0); context->OMSetDepthStencilState(state,0); context->Draw(3,0);
        };
        draw(.5f,1,0,opaque.Get()); // opaque HERO
        draw(.9f,0,1,opaque.Get()); // far surface cannot overwrite HERO
        draw(.1f,0,1,smoke.Get());  // smoke in front passes but must not write depth
        draw(.3f,1,0,opaque.Get()); // remains visible behind smoke
        draw(.9f,0,1,opaque.Get()); // far geometry must still fail the depth test
        ComPtr<ID3D11CommandList> commands; Check(context->FinishCommandList(false,&commands));
        immediate->ExecuteCommandList(commands.Get(),true);
        ComPtr<ID3D11DepthStencilState> restored; UINT ref=0; immediate->OMGetDepthStencilState(&restored,&ref);
        Require(restored.Get()==smoke.Get() && ref==17);
        immediate->CopyResource(readback.Get(),target.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(immediate->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        const auto pixel=static_cast<const unsigned char*>(mapped.pData)+mapped.RowPitch+4;
        Require(pixel[0]==255 && pixel[1]==0); immediate->Unmap(readback.Get(),0);
    }
    std::cout<<"Render depth check passed: forward/reverse projection conversion, GPU occlusion, smoke depth writes, immediate state restoration\n";
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
