// SPDX-License-Identifier: GPL-3.0-only
#include "Render.h"
#include "RenderDepth.h"
#include "CallDiscovery.h"
#include "SkyrimWorld.h"
#include <SKSE/SKSE.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <WICTextureLoader.h>
#include <mutex>
#include <atomic>
#include <SKSEMenuFramework.h>

namespace Aottg::Render {
    using Microsoft::WRL::ComPtr;
    namespace {
        std::uintptr_t renderCall=0;
        std::mutex mutex;
        std::shared_ptr<const Frame> frame;
        std::vector<Material> materials;
        std::filesystem::path assetPath;
        bool assets=false, attempted=false;
        std::atomic<bool> ready=false;
        std::atomic<unsigned> draws=0;
        std::string failure;
        ComPtr<ID3D11Device1> device;
        ComPtr<ID3D11DeviceContext1> context, immediate;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11Buffer> vertices, constants;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11SamplerState> hudSampler, hudFontSampler, savedHudSampler;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11DepthStencilState> depth[2], smokeDepth[2];
        ComPtr<ID3D11BlendState> blend[2];
        std::vector<ComPtr<ID3D11ShaderResourceView>> textures;
        unsigned capacity=0;
        std::atomic<bool> hudVisible{true};
        LibAottg::HudPackage damagePackage, killPackage, heroPackage;
        struct HudTexture { ComPtr<ID3D11ShaderResourceView> srv; unsigned width,height; bool font=false; };
        std::map<std::string,HudTexture> hudTextures;
        std::unique_ptr<SKSEMenuFramework::Model::HudElement> hudElement;
        struct Constants { float matrix[4][4]; DirectX::XMFLOAT4 color, params, viewport; };
        constexpr const char* shader=R"(
cbuffer Frame : register(b0) { row_major float4x4 viewProj; float4 tint; float4 options; float4 viewport; };
Texture2D tex : register(t0); SamplerState smp : register(s0);
struct V { float3 p:POSITION; float3 n:NORMAL; float2 uv:TEXCOORD0; };
struct P { float4 p:SV_Position; float3 n:NORMAL; float2 uv:TEXCOORD0; };
P VS(V v) { P p; p.p=mul(viewProj,float4(v.p,1)); p.n=v.n; p.uv=v.uv; return p; }
[maxvertexcount(4)] void GS(line P v[2], inout TriangleStream<P> stream) {
    float4 a=v[0].p, b=v[1].p;
    float2 delta=(b.xy/b.w-a.xy/a.w)*viewport.xy*0.5; float lengthPixels=length(delta);
    if (lengthPixels<0.01) return;
    float2 perpendicular=float2(-delta.y,delta.x)/lengthPixels;
    float2 offset=perpendicular*options.w/viewport.xy;
    P p; p.n=v[0].n; p.uv=v[0].uv; p.p=a; p.p.xy+=offset*a.w; stream.Append(p);
    p.p=a; p.p.xy-=offset*a.w; stream.Append(p);
    p.n=v[1].n; p.uv=v[1].uv; p.p=b; p.p.xy+=offset*b.w; stream.Append(p);
    p.p=b; p.p.xy-=offset*b.w; stream.Append(p); stream.RestartStrip();
}
float4 PS(P p):SV_Target { float4 c=tex.Sample(smp,p.uv)*tint; if(options.x==1 && c.a<options.y) discard;
    float light=options.z>0 ? 1 : 0.65+0.35*saturate(dot(normalize(p.n),normalize(float3(0.3,0.2,1))));
    return float4(c.rgb*light,c.a); }
)";
        void Check(HRESULT hr,const char* operation) { if (FAILED(hr)) throw std::runtime_error(std::format("{}: HRESULT {:08X}",operation,static_cast<unsigned>(hr))); }
        void Init() {
            auto renderer=RE::BSGraphics::Renderer::GetSingleton(); if (!renderer || !renderer->GetRuntimeData().forwarder) return;
            attempted=true;
            auto dev=reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder);
            auto ctx=reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
            Check(dev->QueryInterface(IID_PPV_ARGS(&device)),"D3D11.1 device");
            Check(ctx->QueryInterface(IID_PPV_ARGS(&immediate)),"D3D11.1 context");
            // Record our explicit GPU state separately. Reverse-Z mods intercept
            // the game's immediate-context setters and translate vanilla states;
            // feeding already-reversed states through those hooks reverses twice.
            Check(device->CreateDeferredContext1(0,&context),"HERO deferred context");
            ComPtr<ID3DBlob> vcode,pcode,error;
            Check(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vcode,&error),"vertex shader");
            ComPtr<ID3DBlob> gcode;
            Check(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"GS","gs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&gcode,&error),"rope geometry shader");
            Check(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&pcode,&error),"pixel shader");
            Check(device->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,&vs),"vertex shader creation");
            Check(device->CreateGeometryShader(gcode->GetBufferPointer(),gcode->GetBufferSize(),nullptr,&gs),"rope geometry shader creation");
            Check(device->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,&ps),"pixel shader creation");
            D3D11_INPUT_ELEMENT_DESC elements[]={
                {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
                {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
                {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
            Check(device->CreateInputLayout(elements,3,vcode->GetBufferPointer(),vcode->GetBufferSize(),&layout),"input layout");
            D3D11_BUFFER_DESC cb{}; cb.ByteWidth=sizeof(Constants); cb.Usage=D3D11_USAGE_DEFAULT; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            Check(device->CreateBuffer(&cb,nullptr,&constants),"constant buffer");
            D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP; sd.MaxLOD=D3D11_FLOAT32_MAX;
            Check(device->CreateSamplerState(&sd,&sampler),"sampler");
            sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
            Check(device->CreateSamplerState(&sd,&hudSampler),"HUD point sampler");
            sd.Filter=D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
            Check(device->CreateSamplerState(&sd,&hudFontSampler),"HUD font sampler");
            D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=true;
            Check(device->CreateRasterizerState(&rd,&raster),"rasterizer");
            for (int reversed=0;reversed<2;++reversed) {
                D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable=true; dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
                dd.DepthFunc=reversed ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
                Check(device->CreateDepthStencilState(&dd,&depth[reversed]),"depth state");
                dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
                Check(device->CreateDepthStencilState(&dd,&smokeDepth[reversed]),"smoke depth state");
            }
            for (int transparent=0;transparent<2;++transparent) {
                D3D11_BLEND_DESC bd{}; auto& t=bd.RenderTarget[0]; t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
                t.BlendEnable=transparent!=0; t.SrcBlend=D3D11_BLEND_SRC_ALPHA; t.DestBlend=D3D11_BLEND_INV_SRC_ALPHA; t.BlendOp=D3D11_BLEND_OP_ADD;
                t.SrcBlendAlpha=D3D11_BLEND_ONE; t.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; t.BlendOpAlpha=D3D11_BLEND_OP_ADD;
                Check(device->CreateBlendState(&bd,&blend[transparent]),"blend state");
            }
            // White texture supplies materials without a captured image and ropes.
            D3D11_TEXTURE2D_DESC td{}; td.Width=td.Height=td.MipLevels=td.ArraySize=1; td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_IMMUTABLE; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            unsigned white=0xFFFFFFFF; D3D11_SUBRESOURCE_DATA init{&white,4,0}; ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> whiteSrv;
            Check(device->CreateTexture2D(&td,&init,&texture),"white texture"); Check(device->CreateShaderResourceView(texture.Get(),nullptr,&whiteSrv),"white texture view");
            for (const auto& m:materials) {
                ComPtr<ID3D11ShaderResourceView> srv=whiteSrv;
                if (!m.texture.empty()) {
                    srv.Reset();
                    Check(DirectX::CreateWICTextureFromFileEx(device.Get(),(assetPath/m.texture).c_str(),0,D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,0,DirectX::WIC_LOADER_FORCE_SRGB,nullptr,&srv),"hero texture");
                }
                textures.push_back(std::move(srv));
            }
            textures.push_back(whiteSrv);
            ComPtr<ID3D11ShaderResourceView> smokeTexture;
            Check(DirectX::CreateWICTextureFromFileEx(device.Get(),(assetPath/"smoke.png").c_str(),0,D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,0,DirectX::WIC_LOADER_FORCE_SRGB,nullptr,&smokeTexture),"smoke texture");
            textures.push_back(smokeTexture);
            const auto uiPath=assetPath.parent_path()/"ui";
            damagePackage.Load(uiPath/"aottg-damage-score/hud.bin"); killPackage.Load(uiPath/"aottg-kill-info/hud.bin"); heroPackage.Load(uiPath/"aottg-hero-hud/hud.bin");
            auto loadHud=[&](const LibAottg::HudPackage& package,const std::string& folder) {
                for(const auto& name:package.Textures()) {
                    HudTexture t; t.font=package.IsFontTexture(name); ComPtr<ID3D11Resource> resource;
                    Check(DirectX::CreateWICTextureFromFileEx(device.Get(),(uiPath/folder/name).c_str(),0,D3D11_USAGE_IMMUTABLE,D3D11_BIND_SHADER_RESOURCE,0,0,DirectX::WIC_LOADER_IGNORE_SRGB,&resource,&t.srv),"HUD texture");
                    ComPtr<ID3D11Texture2D> tex; Check(resource.As(&tex),"HUD texture dimensions"); D3D11_TEXTURE2D_DESC d{};tex->GetDesc(&d);t.width=d.Width;t.height=d.Height;hudTextures[folder+"/"+name]=std::move(t);
                }
            };
            loadHud(damagePackage,"aottg-damage-score");loadHud(killPackage,"aottg-kill-info");loadHud(heroPackage,"aottg-hero-hud");
            ready=true;
            SKSE::log::info("HERO HUD assets ready; textures={}; score=AoTTG; stylish=AoTTG; killInfo=AoTTG; build={}",hudTextures.size(),LIBAOTTG_BUILD_ID);
            SKSE::log::info("HERO renderer ready; materials={}; depth=Skyrim MAIN; target=HDR; build={}",materials.size(),LIBAOTTG_BUILD_ID);
        }
        struct RecordingGuard {
            bool submitted=false;
            ~RecordingGuard() {
                if (!submitted) { ComPtr<ID3D11CommandList> discarded; context->FinishCommandList(false,&discarded); }
            }
            void Submit() {
                ComPtr<ID3D11CommandList> commands;
                Check(context->FinishCommandList(false,&commands),"HERO command list");
                immediate->ExecuteCommandList(commands.Get(),true);
                submitted=true;
            }
        };
        void Draw() {
            std::shared_ptr<const Frame> f;
            { std::scoped_lock lock(mutex); if (!assets) return; f=frame; }
            if (!attempted) Init(); if (!ready || !f) return;
            auto renderer=RE::BSGraphics::Renderer::GetSingleton(); auto camera=RE::Main::WorldRootCamera();
            if (!renderer || !camera) return;
            auto& scene=renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
            auto& ds=renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
            if (!scene.RTV || !scene.texture || !ds.views[0]) return;
            RecordingGuard guard;
            auto rtv=reinterpret_cast<ID3D11RenderTargetView*>(scene.RTV); auto dsv=reinterpret_cast<ID3D11DepthStencilView*>(ds.views[0]);
            context->OMSetRenderTargets(1,&rtv,dsv);
            D3D11_TEXTURE2D_DESC desc{}; reinterpret_cast<ID3D11Texture2D*>(scene.texture)->GetDesc(&desc);
            const auto& graphics=RE::BSGraphics::State::GetSingleton()->GetRuntimeData();
            const float width=float(desc.Width)*graphics.dynamicResolutionWidthRatio;
            const float height=float(desc.Height)*graphics.dynamicResolutionHeightRatio;
            D3D11_VIEWPORT vp{0,0,width,height,0,1}; context->RSSetViewports(1,&vp);
            const auto& matrix=camera->GetRuntimeData().worldToCam; const auto origin=camera->world.translate;
            Constants c{};
            c.viewport={width,height,0,0};
            for (int r=0;r<4;++r) {
                for (int col=0;col<3;++col) c.matrix[r][col]=matrix[r][col];
                c.matrix[r][3]=float(double(matrix[r][3])+double(matrix[r][0])*origin.x+double(matrix[r][1])*origin.y+double(matrix[r][2])*origin.z);
            }
            // Detect reversed Z from projection at two forward distances (works across INIs).
            const auto fwd=RE::NiPoint3(camera->world.rotate.entry[0][0],camera->world.rotate.entry[1][0],camera->world.rotate.entry[2][0]);
            auto z=[&](float d) { auto p=origin+fwd*d; float clipZ=matrix[2][0]*p.x+matrix[2][1]*p.y+matrix[2][2]*p.z+matrix[2][3]; float w=matrix[3][0]*p.x+matrix[3][1]*p.y+matrix[3][2]*p.z+matrix[3][3]; return clipZ/w; };
            const bool cameraReversed=z(100)>z(1000);
            // NiCamera::worldToCam stays vanilla in Bottled Shaders. The
            // published GPU projection is reversed instead; use that convention
            // with the matching MAIN depth target, keeping our world camera XY/W.
            auto shadow=RE::BSGraphics::RendererShadowState::GetSingleton();
            const auto published=shadow->GetRuntimeData().cameraData.getEye().projMat;
            const bool reversed=ReverseProjection(published);
            MatchDepthConvention(c.matrix,cameraReversed,reversed);
            context->OMSetDepthStencilState(depth[reversed?1:0].Get(),0);
            static int reportedConvention=-1;
            if (reportedConvention!=int(reversed)) {
                reportedConvention=int(reversed);
                D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc{}; dsv->GetDesc(&depthDesc);
                SKSE::log::info("HERO depth convention; reversed={}; cameraReversed={}; format={}; comparison={}; context=deferred; viewport={}x{}; target={}x{}; cameraDepth100={}; cameraDepth1000={}; build={}",reversed,cameraReversed,unsigned(depthDesc.Format),reversed?"GREATER_EQUAL":"LESS_EQUAL",width,height,desc.Width,desc.Height,z(100),z(1000),LIBAOTTG_BUILD_ID);
            }
            context->RSSetState(raster.Get()); context->IASetInputLayout(layout.Get()); context->VSSetShader(vs.Get(),nullptr,0); context->PSSetShader(ps.Get(),nullptr,0);
            context->GSSetShader(nullptr,nullptr,0); context->HSSetShader(nullptr,nullptr,0); context->DSSetShader(nullptr,nullptr,0);
            auto cb=constants.Get(); context->VSSetConstantBuffers(0,1,&cb); context->GSSetConstantBuffers(0,1,&cb); context->PSSetConstantBuffers(0,1,&cb); auto smp=sampler.Get(); context->PSSetSamplers(0,1,&smp);
            auto draw=[&](const std::vector<Vertex>& source,int material,bool rope,DirectX::XMFLOAT4 tint=DirectX::XMFLOAT4(1,1,1,1)) {
                if (source.empty()) return;
                if (source.size()>capacity) {
                    vertices.Reset(); capacity=unsigned(source.size());
                    D3D11_BUFFER_DESC b{}; b.ByteWidth=capacity*sizeof(Vertex); b.Usage=D3D11_USAGE_DYNAMIC; b.BindFlags=D3D11_BIND_VERTEX_BUFFER; b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
                    Check(device->CreateBuffer(&b,nullptr,&vertices),"dynamic hero vertices");
                }
                D3D11_MAPPED_SUBRESOURCE mapped{}; Check(context->Map(vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"vertex upload");
                auto dest=static_cast<Vertex*>(mapped.pData);
                for (size_t i=0;i<source.size();++i) { dest[i]=source[i]; dest[i].position.x-=origin.x; dest[i].position.y-=origin.y; dest[i].position.z-=origin.z; }
                context->Unmap(vertices.Get(),0);
                unsigned stride=sizeof(Vertex),offset=0; auto vb=vertices.Get(); context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
                context->IASetPrimitiveTopology(rope ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                const Material m=material>=0 ? materials[material] : material==-2 ? Material{tint,2,0,{}} : Material{{0.07f,0.07f,0.07f,1},0,0,{}};
                c.color=m.color; c.params={float(m.alphaMode),m.cutoff,(rope || material==-2)?1.0f:0.0f,rope?f->ropeThicknessMultiplier:0};
                context->UpdateSubresource(constants.Get(),0,nullptr,&c,0,0);
                auto srv=textures[material>=0 ? material : materials.size()+(material==-2?1:0)].Get(); context->PSSetShaderResources(0,1,&srv);
                context->OMSetBlendState(blend[m.alphaMode==2?1:0].Get(),nullptr,0xFFFFFFFF);
                context->GSSetShader(rope?gs.Get():nullptr,nullptr,0);
                context->Draw(unsigned(source.size()),0);
            };
            for (const auto& m:f->meshes) draw(m.vertices,m.material,false);
            draw(f->ropes,-1,true);
            context->OMSetDepthStencilState(smokeDepth[reversed?1:0].Get(),0);
            if (!f->smoke.empty()) {
                static bool reported=false;
                if (!reported) { reported=true; SKSE::log::info("HERO smoke world draw; particles={}; depthTest=true; depthWrite=false; build={}",f->smoke.size(),LIBAOTTG_BUILD_ID); }
            }
            auto particles=f->smoke;
            std::sort(particles.begin(),particles.end(),[&](const auto& a,const auto& b) { return (ToSkyrim(a.Position)-origin).SqrLength()>(ToSkyrim(b.Position)-origin).SqrLength(); });
            const auto& rotation=camera->world.rotate;
            RE::NiPoint3 right{rotation.entry[0][1],rotation.entry[1][1],rotation.entry[2][1]}, up{rotation.entry[0][2],rotation.entry[1][2],rotation.entry[2][2]};
            for (const auto& particle:particles) {
                const auto center=ToSkyrim(particle.Position);
                auto r=(right*std::cos(particle.Rotation)+up*std::sin(particle.Rotation))*(particle.Size*.5f);
                auto u=(up*std::cos(particle.Rotation)-right*std::sin(particle.Rotation))*(particle.Size*.5f);
                std::vector<Vertex> quad;
                for (int index:{0,1,2,0,2,3}) {
                    const float x=index==0 || index==3 ? -1.f:1.f, y=index<2 ? -1.f:1.f;
                    auto point=center+r*x+u*y;
                    quad.push_back({{point.x,point.y,point.z},{0,0,1},{(x+1)*.5f,(1-y)*.5f}});
                }
                draw(quad,-2,false,{particle.Color.X,particle.Color.Y,particle.Color.Z,particle.Alpha});
            }
            guard.Submit();
            if (++draws==1) SKSE::log::info("HERO first world draw; meshes={}; build={}",f->meshes.size(),LIBAOTTG_BUILD_ID);
        }
        // Menu Framework submits HUD images after world postprocessing, preserving
        // the original font/sprite colors and keeping the overlay independent of depth.
        // These callbacks run when ImGui executes the draw list, after its backend
        // binds its own sampler. Restore that exact state for subsequent UI draws.
        void BeginHudPointFiltering(const ImGuiMCP::ImDrawList*,const ImGuiMCP::ImDrawCmd*) {
            immediate->PSGetSamplers(0,1,savedHudSampler.ReleaseAndGetAddressOf());
            auto state=hudSampler.Get();
            immediate->PSSetSamplers(0,1,&state);
        }
        void EndHudPointFiltering(const ImGuiMCP::ImDrawList*,const ImGuiMCP::ImDrawCmd*) {
            auto state=savedHudSampler.Get();
            immediate->PSSetSamplers(0,1,&state);
            savedHudSampler.Reset();
        }
        void HudLinearFiltering(const ImGuiMCP::ImDrawList*,const ImGuiMCP::ImDrawCmd*) {
            auto state=savedHudSampler.Get(); immediate->PSSetSamplers(0,1,&state);
        }
        void HudPointFiltering(const ImGuiMCP::ImDrawList*,const ImGuiMCP::ImDrawCmd*) {
            auto state=hudSampler.Get(); immediate->PSSetSamplers(0,1,&state);
        }
        void HudFontFiltering(const ImGuiMCP::ImDrawList*,const ImGuiMCP::ImDrawCmd*) {
            auto state=hudFontSampler.Get(); immediate->PSSetSamplers(0,1,&state);
        }
        void __stdcall DrawHud() {
            static bool callbackReported=false, visibleReported=false;
            if(!callbackReported) { callbackReported=true; SKSE::log::info("HERO HUD callback active; build={}",LIBAOTTG_BUILD_ID); }
            std::shared_ptr<const Frame> f;
            { std::scoped_lock lock(mutex); f=frame; }
            if (!ready || !f || !hudVisible) return;
            ImGuiMCP::ImDrawList* filteredList=nullptr;
            try {
                auto size=ImGuiMCP::GetIO()->DisplaySize;
                if(size.x<=0 || size.y<=0) return;
                auto list=ImGuiMCP::GetBackgroundDrawList();
                ImGuiMCP::ImDrawListManager::AddCallback(list,BeginHudPointFiltering,nullptr);
                filteredList=list;
                int mode=2, currentFilter=-1; float offsetX=0,offsetY=0;
                auto draw=[&](const std::vector<LibAottg::DamageHudQuad>& quads,const std::string& folder) {
                    for(const auto& q:quads) {
                        if(!visibleReported && q.Color[3]>0 && q.X<size.x && q.X+q.Width>0 && q.Y<size.y && q.Y+q.Height>0) {
                            visibleReported=true;
                            SKSE::log::info("HERO first visible HUD submission; package={}; quads={}; score={}; hits={}; kills={}; build={}",folder,quads.size(),f->damage.ScoreText,f->damage.Hits,f->kills.Entries.size(),LIBAOTTG_BUILD_ID);
                        }
                        const auto& t=hudTextures.at(folder+"/"+q.Texture);
                        // Bitmap text stays crisp when enlarged, but retains coverage
                        // when reduced. Sprites retain the framework's linear sampler.
                        int filter=mode==1?1:(mode==2 && t.font?2:0);
                        if(filter!=currentFilter) {
                            ImGuiMCP::ImDrawListManager::AddCallback(list,filter==1?HudPointFiltering:filter==2?HudFontFiltering:HudLinearFiltering,nullptr);
                            currentFilter=filter;
                        }
                        unsigned tint=0;
                        for(int i=0;i<4;++i) tint|=unsigned(std::clamp(q.Color[i],0.f,1.f)*255.f+.5f)<<(8*i);
                        if(q.Rotation==0) {
                            ImGuiMCP::ImDrawListManager::AddImage(list,t.srv.Get(),{q.X+offsetX,q.Y+offsetY},{q.X+q.Width+offsetX,q.Y+q.Height+offsetY},
                                {q.SourceX/t.width,q.SourceY/t.height},{(q.SourceX+q.SourceWidth)/t.width,(q.SourceY+q.SourceHeight)/t.height},tint);
                        } else {
                            float cx=q.X+q.Width*.5f+offsetX,cy=q.Y+q.Height*.5f+offsetY,c=std::cos(q.Rotation),s=std::sin(q.Rotation);
                            auto point=[&](float x,float y){return ImGuiMCP::ImVec2{cx+x*c-y*s,cy+x*s+y*c};};
                            float u=q.SourceX/t.width,v=q.SourceY/t.height,du=q.SourceWidth/t.width,dv=q.SourceHeight/t.height;
                            ImGuiMCP::ImDrawListManager::AddImageQuad(list,t.srv.Get(),point(-q.Width*.5f,-q.Height*.5f),point(q.Width*.5f,-q.Height*.5f),point(q.Width*.5f,q.Height*.5f),point(-q.Width*.5f,q.Height*.5f),{u,v},{u+du,v},{u+du,v+dv},{u,v+dv},tint);
                        }
                    }
                };
                if(f->hudComparison) {
                    LibAottg::HeroDamageHud sample; sample.ShowDamage(1234); sample.ShowDamage(5678); sample.Advance(.4f); sample.StylishX=0; sample.StylishY=0;
                    LibAottg::HeroKillInfo feed; feed.Show(false,"Levi",true,"Titan",1234); feed.Show(true,"Titan",false,"Mikasa",5678); feed.Advance(.5f);
                    const char* labels[]={"LINEAR","POINT: ALL","POINT: ENLARGED TEXT"};
                    for(mode=0;mode<3;++mode) {
                        float left=120+mode*((size.x-120)/3);
                        ImGuiMCP::ImDrawListManager::AddCallback(list,HudLinearFiltering,nullptr); currentFilter=0;
                        ImGuiMCP::ImDrawListManager::AddRectFilled(list,{left-8,24},{left+340,590},0xE0202020,0,0);
                        ImGuiMCP::ImDrawListManager::AddText(list,{left,30},0xFFFFFFFF,labels[mode]);
                        auto preview=[&](auto quads,const std::string& folder,float top) {
                            float minX=std::numeric_limits<float>::max(),minY=minX;
                            for(const auto& q:quads){ minX=std::min(minX,q.X); minY=std::min(minY,q.Y); }
                            offsetX=left-minX; offsetY=top-minY; draw(quads,folder);
                        };
                        // Keep original widget sizes: only translate the samples.
                        sample.ScoreScale=0; preview(damagePackage.Compose(sample,size.x,size.y),"aottg-damage-score",80);
                        preview(killPackage.Compose(feed,size.x,size.y),"aottg-kill-info",300);
                        sample.ScoreScale=100; auto score=damagePackage.Compose(sample,size.x,size.y);
                        std::erase_if(score,[](const auto& q){return q.Texture!="hitscore48_0.png";});
                        preview(score,"aottg-damage-score",450);
                    }
                } else {
                if(f->resourceHud)draw(heroPackage.Compose(f->heroHud,size.x,size.y),"aottg-hero-hud");
                if(f->damageHud)draw(damagePackage.Compose(f->damage,size.x,size.y),"aottg-damage-score");
                if(f->killInfo)draw(killPackage.Compose(f->kills,size.x,size.y),"aottg-kill-info");
                }
            } catch(const std::exception& e) { SKSE::log::error("HERO HUD rendering failed: {}",e.what()); }
            if(filteredList) ImGuiMCP::ImDrawListManager::AddCallback(filteredList,EndHudPointFiltering,nullptr);
        }
        struct Hook {
            static void Thunk(bool value) { original(value); try { Draw(); } catch (const std::exception& e) { ready=false; {std::scoped_lock lock(mutex);failure=e.what();} SKSE::log::error("HERO rendering disabled: {}",e.what()); } }
            static inline REL::Relocation<decltype(Thunk)> original;
        };
    }
    bool ValidateHooks() {
        const auto owner=REL::ID(36559).address(), target=REL::ID(107142).address();
        const auto sites=Hooks::FunctionCalls(owner,target);
        if (sites.size()!=1) { failure="World render hook requires exactly one validated Address Library call"; SKSE::log::error("{}; candidates={}",failure,sites.size()); return false; }
        renderCall=sites.front();
        SKSE::log::info("HERO render hook discovered; offset={:X}; build={}",renderCall-owner,LIBAOTTG_BUILD_ID);
        return true;
    }
    bool Install() {
        const auto site=renderCall, target=REL::ID(107142).address();
        if (!site) return false;
        const auto code=reinterpret_cast<const unsigned char*>(site); std::int32_t displacement; std::memcpy(&displacement,code+1,4);
        if (code[0]!=0xE8 || site+5+static_cast<std::intptr_t>(displacement)!=target) { failure="World render call does not match supported runtime"; SKSE::log::error("{}",failure); return false; }
        Hook::original=SKSE::GetTrampoline().write_call<5>(site,Hook::Thunk); return true;
    }
    void SetHudVisible(bool visible) { hudVisible=visible; }
    void SetAssets(const Character& ch,const std::filesystem::path& dir) { std::scoped_lock lock(mutex); materials=ch.materials; assetPath=dir; assets=true; if(!hudElement) hudElement=std::make_unique<SKSEMenuFramework::Model::HudElement>(DrawHud); }
    void Prepare() { if (!attempted) Init(); }
    void Publish(std::shared_ptr<const Frame> f) { std::scoped_lock lock(mutex); frame=std::move(f); }
    bool Ready() { return ready; }
    unsigned Draws() { return draws; }
    std::string Failure() { std::scoped_lock lock(mutex); return failure; }
}
