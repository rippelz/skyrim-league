// SPDX-License-Identifier: GPL-3.0-or-later
// Scene target integration and state preservation adapted from SkyCraft (MIT).
#include "DirectRender.hpp"
#include "shared/shadow_projection.hpp"
#include <RE/B/BSShadowDirectionalLight.h>
#include <RE/M/Main.h>
#include <RE/N/NiCamera.h>
#include <RE/R/Renderer.h>
#include <RE/S/Sky.h>
#include <RE/S/Sun.h>
#include <RE/N/NiDirectionalLight.h>
#include <RE/T/TESWeather.h>
#include <RE/T/TESObjectLIGH.h>
#include <RE/B/bhkWorld.h>
#include <RE/B/bhkPickData.h>
#include <RE/H/hkpCollidable.h>
namespace RE { class hkpRigidBody; }
#include <RE/H/hkpWorld.h>
#pragma push_macro("InterlockedCompareExchange")
#undef InterlockedCompareExchange
#include <RE/T/TESObjectCELL.h>
#include <RE/P/PlayerCharacter.h>
#pragma pop_macro("InterlockedCompareExchange")
#include <d3d11.h>
#include <d3dcompiler.h>
#include <fstream>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <cstring>
namespace direct_render {
namespace {
template<class T> void release(T*& p) { if(p) p->Release();p=nullptr; }
struct LocalLight {RE::NiPoint3 position;float radius;float color[3];};
struct Pose { bool active{},car_visible{},ball_visible{},boosting{};RE::NiTransform car{},ball{};std::vector<LocalLight> lights;float sun_visibility[2]{1,1},sky_visibility[2]{1,1};float brightness=1.0f;bool cast_shadows=true; };
std::mutex pose_mutex;Pose pose;
struct Vertex { float position[3],normal[3],uv[2],color[4]; };
struct Mesh { ID3D11Buffer* vertices{};ID3D11ShaderResourceView* texture{},*normal{},*paint_mask{};UINT count{};bool ball{};float roughness=.65f,metallic=.02f,normal_alpha{},style{}; };
std::vector<Mesh> meshes;
ID3D11VertexShader* shadow_vs{};ID3D11RasterizerState* shadow_raster{},*shadow_reverse_raster{};
ID3D11VertexShader* vs{};ID3D11PixelShader* ps{};ID3D11InputLayout* layout{};
ID3D11Buffer* constants{};ID3D11SamplerState* sampler{};ID3D11RasterizerState* raster{};ID3D11BlendState* blend{};
ID3D11DepthStencilState* depth_normal{},*depth_reverse{};
ID3D11PixelShader* boost_ps{};ID3D11Buffer* boost_vertices{};ID3D11ShaderResourceView* boost_texture{};
ID3D11BlendState* boost_blend{};ID3D11DepthStencilState* boost_depth_normal{},*boost_depth_reverse{};
UINT boost_count{};
bool initialized{},failed{};std::atomic<bool> ready{false};
constexpr char shader[]=R"(
cbuffer Object : register(b0) { row_major float4x4 viewProj;row_major float4x4 model;float4 sunlight;float4 sunColor;float4 ambient[6];float4 fogRange;float4 fogNear;float4 fogFar;float4 material;float4 renderParams;float4 paintColor;float4 surface;float4 localPosition[8];float4 localColor[8]; };
Texture2D paintMask : register(t3);
Texture2D diffuse : register(t0);Texture2D normalMap : register(t1);SamplerState samp : register(s0);
struct Input { float3 position:POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 color:COLOR0; };
struct Output { float4 position:SV_POSITION;float3 normal:NORMAL;float2 uv:TEXCOORD0;float4 color:COLOR0;float distance:TEXCOORD1;float3 relative:TEXCOORD2; };
Output VS(Input i) { Output o;float4 rel=mul(model,float4(i.position,1));o.position=mul(viewProj,rel);o.distance=length(rel.xyz);o.relative=rel.xyz;o.normal=mul((float3x3)model,i.normal);o.uv=i.uv;o.color=i.color;return o; }
float4 ShadowVS(Input i):SV_POSITION {return mul(viewProj,mul(model,float4(i.position,1)));}
float3 environment(float3 n) {
 return ambient[n.x>=0?0:1].rgb*n.x*n.x+ambient[n.y>=0?2:3].rgb*n.y*n.y+ambient[n.z>=0?4:5].rgb*n.z*n.z;
}
float4 PS(Output i):SV_TARGET {
 float3 base=diffuse.Sample(samp,i.uv).rgb*i.color.rgb;
 if(paintColor.w>.5) base*=lerp(float3(1,1,1),paintColor.rgb,saturate(paintMask.Sample(samp,i.uv).r));
 float ao=clamp(i.color.a,.3,1);
 if(surface.x>2.5 && surface.x<3.5)base*=.22; // OEM+ rubber/rim base tint.
 float3 geometric=normalize(i.normal);
 float4 tex=normalMap.Sample(samp,i.uv);float2 xy=lerp(tex.rg,tex.ag,material.z)*2-1;
 float3 tangentNormal=float3(xy,sqrt(saturate(1-dot(xy,xy))));
 float3 dp1=ddx(i.relative),dp2=ddy(i.relative);float2 duv1=ddx(i.uv),duv2=ddy(i.uv);
 float determinant=duv1.x*duv2.y-duv1.y*duv2.x;
 float3 n=geometric;
 if(abs(determinant)>1e-7) {
  float3 tangent=(dp1*duv2.y-dp2*duv1.y)/determinant;
  tangent=normalize(tangent-geometric*dot(tangent,geometric));
  float3 uvBitangent=(dp2*duv1.x-dp1*duv2.x)/determinant;
  float handedness=dot(cross(geometric,tangent),uvBitangent)<0?-1:1;
  float3 bitangent=normalize(cross(geometric,tangent))*handedness;
  n=normalize(tangent*tangentNormal.x+bitangent*tangentNormal.y+geometric*tangentNormal.z);
 }
 float3 v=normalize(-i.relative),l=normalize(sunlight.xyz),h=normalize(v+l);
 float nl=saturate(dot(n,l)),nv=max(saturate(dot(n,v)),.001),nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
 float rough=clamp(material.x,.12,.95),metal=saturate(material.y),a=rough*rough,a2=a*a;
 float den=nh*nh*(a2-1)+1;float distribution=a2/(3.14159265*den*den);
 float k=(rough+1)*(rough+1)/8;float geometry=nv/(nv*(1-k)+k)*nl/(nl*(1-k)+k);
 float dielectric=surface.x>3.5 && surface.x<4.5?.07:.04;
 float3 f0=lerp(float3(dielectric,dielectric,dielectric),base,metal),fresnel=f0+(1-f0)*pow(1-vh,5);
 float3 spec=distribution*geometry*fresnel/max(4*nv*max(nl,.001),.001);
 float sunVisible=material.w;
 // Skyrim's screen-space mask was generated from native world depth, not
 // this mesh depth. Sampling it falsely shadows the mesh with its ground shadow.
 float3 lit=base*(1-f0)*(1-metal)*environment(n)*sunlight.w*ao*.65
   +base*(1-fresnel)*(1-metal)*sunColor.rgb*nl*sunVisible+spec*sunColor.rgb*nl*sunVisible;
 for(int j=0;j<8;++j) {
  float3 delta=localPosition[j].xyz-i.relative;float distance=length(delta),radius=localPosition[j].w;
  if(radius<=0 || distance>=radius)continue;
  float3 pl=delta/max(distance,.001),ph=normalize(pl+v);float pnl=saturate(dot(n,pl)),pnh=saturate(dot(n,ph)),pvh=saturate(dot(v,ph));
  float pd=pnh*pnh*(a2-1)+1,pD=a2/(3.14159265*pd*pd);
  float pG=nv/(nv*(1-k)+k)*pnl/(pnl*(1-k)+k);
  float3 pF=f0+(1-f0)*pow(1-pvh,5);
  float attenuation=pow(saturate(1-distance/radius),2);
  lit+=(base*(1-pF)*(1-metal)+pD*pG*pF/max(4*nv*max(pnl,.001),.001))*localColor[j].rgb*pnl*attenuation;
 }
 float3 reflection=reflect(-v,n);float3 envF=f0+(1-f0)*pow(1-nv,5);
 lit+=environment(reflection)*envF*(1-.6*rough)*sunlight.w*ao*.2;
 lit*=renderParams.z;
 float fog=pow(saturate(i.distance*fogRange.x-fogRange.y),max(fogRange.z,.01))*fogRange.w;
 lit=lerp(lit,lerp(fogNear.rgb,fogFar.rgb,fog),fog);return float4(lit,1);
}
float4 BoostPS(Output i):SV_TARGET {float4 flame=diffuse.Sample(samp,i.uv);return float4(flame.rgb*3,flame.a);}
)";
struct Constants { float viewProj[4][4],model[4][4],sunlight[4],sunColor[4],ambient[6][4],fogRange[4],fogNear[4],fogFar[4],material[4],renderParams[4],paintColor[4],surface[4],localPosition[8][4],localColor[8][4]; };
		struct StateBackup
		{
			ID3D11RenderTargetView*   rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   dsv{};
			ID3D11BlendState*         blend{};
			float                     factor[4]{};
			UINT                      mask{};
			ID3D11RasterizerState*    raster{};
			ID3D11DepthStencilState*  depth{};
			UINT                      stencil{};
			D3D11_VIEWPORT            vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      vpCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_PRIMITIVE_TOPOLOGY  topo{};
			ID3D11InputLayout*        layout{};
			ID3D11Buffer*             vb{};
			UINT                      stride{}, offset{};
			ID3D11VertexShader*       vs{};
			ID3D11PixelShader*        ps{};
			ID3D11Buffer*             vsCb[2]{};
			ID3D11Buffer*             psCb[2]{};
			ID3D11ShaderResourceView* srv[4]{};
			ID3D11SamplerState*       samplers[3]{};
			// Binding a depth buffer or render target unbinds it wherever it's bound for reading;
			// put every stage's resources back so Skyrim's renderer finds what it left.
			static constexpr UINT     kSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
			ID3D11ShaderResourceView* vsAll[kSlots]{};
			ID3D11ShaderResourceView* psAll[kSlots]{};
			ID3D11ShaderResourceView* csAll[kSlots]{};

			void Save(ID3D11DeviceContext* a_c)
			{
				a_c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
				a_c->OMGetBlendState(&blend, factor, &mask);
				a_c->RSGetState(&raster);
				a_c->OMGetDepthStencilState(&depth, &stencil);
				a_c->RSGetViewports(&vpCount, vps);
				a_c->IAGetPrimitiveTopology(&topo);
				a_c->IAGetInputLayout(&layout);
				a_c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
				a_c->VSGetShader(&vs, nullptr, nullptr);
				a_c->PSGetShader(&ps, nullptr, nullptr);
				a_c->VSGetConstantBuffers(0, 2, vsCb);
				a_c->PSGetConstantBuffers(0, 2, psCb);
				a_c->PSGetShaderResources(0, 4, srv);
				a_c->PSGetSamplers(0, 3, samplers);
				a_c->VSGetShaderResources(0, kSlots, vsAll);
				a_c->PSGetShaderResources(0, kSlots, psAll);
				a_c->CSGetShaderResources(0, kSlots, csAll);
			}

			void Restore(ID3D11DeviceContext* a_c)
			{
				a_c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
				a_c->OMSetBlendState(blend, factor, mask);
				a_c->RSSetState(raster);
				a_c->OMSetDepthStencilState(depth, stencil);
				a_c->RSSetViewports(vpCount, vps);
				a_c->IASetPrimitiveTopology(topo);
				a_c->IASetInputLayout(layout);
				a_c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
				a_c->VSSetShader(vs, nullptr, 0);
				a_c->PSSetShader(ps, nullptr, 0);
				a_c->VSSetConstantBuffers(0, 2, vsCb);
				a_c->PSSetConstantBuffers(0, 2, psCb);
				a_c->PSSetShaderResources(0, 4, srv);
				a_c->PSSetSamplers(0, 3, samplers);
				a_c->VSSetShaderResources(0, kSlots, vsAll);
				a_c->PSSetShaderResources(0, kSlots, psAll);
				a_c->CSSetShaderResources(0, kSlots, csAll);
				for (auto* list : { vsAll, psAll, csAll }) {
					for (UINT k = 0; k < kSlots; ++k) {
						if (list[k]) {
							list[k]->Release();
						}
					}
				}
				for (auto*& r : rtv) {
					release(r);
				}
				release(dsv);
				release(blend);
				release(raster);
				release(depth);
				release(layout);
				release(vb);
				release(vs);
				release(ps);
				for (auto*& b : vsCb) {
					release(b);
				}
				for (auto*& b : psCb) {
					release(b);
				}
				for (auto*& s : srv) {
					release(s);
				}
				for (auto*& s : samplers) {
					release(s);
				}
			}
		};

void check(HRESULT hr,const char* what) { if(FAILED(hr)) throw std::runtime_error(std::string(what)+" HRESULT "+std::to_string(static_cast<unsigned>(hr))); }
template<class T> T read(std::ifstream& f) { T x{};f.read(reinterpret_cast<char*>(&x),sizeof x);if(!f) throw std::runtime_error("Truncated mesh");return x; }
ID3D11ShaderResourceView* texture(ID3D11Device* d,std::string name,bool srgb=true) {
  std::ifstream f("Data/"+name,std::ios::binary);if(!f)throw std::runtime_error("Missing texture "+name);
  auto magic=read<std::uint32_t>(f);std::uint32_t h[31]{};f.read(reinterpret_cast<char*>(h),sizeof h);
  if(magic!=0x20534444 || h[0]!=124 || h[2]>8192 || h[3]>8192)throw std::runtime_error("Invalid DDS");
  auto fourcc=h[20];DXGI_FORMAT format;UINT block;
  if(fourcc==0x31545844) {format=srgb?DXGI_FORMAT_BC1_UNORM_SRGB:DXGI_FORMAT_BC1_UNORM;block=8;}
  else if(fourcc==0x35545844) {format=srgb?DXGI_FORMAT_BC3_UNORM_SRGB:DXGI_FORMAT_BC3_UNORM;block=16;}
  else if(fourcc==0 && h[21]==32 && h[22]==0xff && h[23]==0xff00 && h[24]==0xff0000 && h[25]==0xff000000) {format=srgb?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;block=0;}
  else throw std::runtime_error("Unsupported DDS format");
  UINT pitch=block?((h[3]+3)/4)*block:h[3]*4,bytes=pitch*(block?((h[2]+3)/4):h[2]);std::vector<char> pixels(bytes);f.read(pixels.data(),bytes);if(!f)throw std::runtime_error("Truncated DDS pixels");
  D3D11_TEXTURE2D_DESC td{};td.Width=h[3];td.Height=h[2];td.MipLevels=1;td.ArraySize=1;td.Format=format;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{pixels.data(),pitch,bytes};ID3D11Texture2D* tex{};check(d->CreateTexture2D(&td,&data,&tex),"DDS texture");
  ID3D11ShaderResourceView* srv{};auto result=d->CreateShaderResourceView(tex,nullptr,&srv);release(tex);check(result,"DDS view");return srv;
}
void load(ID3D11Device* d,const char* filename,bool ball) {
  std::ifstream f(filename,std::ios::binary);if(!f)throw std::runtime_error(std::string("Missing direct mesh ")+filename);
  if(read<std::uint32_t>(f)!=0x314d4252)throw std::runtime_error("Wrong mesh version");
  auto count=read<std::uint32_t>(f);if(count>32)throw std::runtime_error("Too many meshes");
  for(unsigned m=0;m<count;++m) {
    auto len=read<std::uint32_t>(f);if(len>256)throw std::runtime_error("Bad texture path");std::string path(len,' ');f.read(path.data(),len);
    auto n=read<std::uint32_t>(f);if(n==0 || n>500000 || n%3)throw std::runtime_error("Bad triangle count");
    std::vector<Vertex> v(n);f.read(reinterpret_cast<char*>(v.data()),n*sizeof(Vertex));if(!f)throw std::runtime_error("Truncated vertices");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=n*sizeof(Vertex);bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{v.data(),0,0};Mesh mesh{};mesh.count=n;mesh.ball=ball;mesh.texture=texture(d,path);
    spdlog::info("Mesh colour map loaded: {} vertices {} sRGB true",path,n);
    auto normal=path;auto suffix=normal.rfind("_d.dds");
    if(suffix!=std::string::npos)normal.replace(suffix,6,"_n.dds");else normal="textures/rocketbridge/flat_n.dds";
    if(path.find("body_grain_d")!=std::string::npos){normal="textures/rocketbridge/body_grain_bodynormalmap.dds";mesh.normal_alpha=1;mesh.roughness=.32f;mesh.metallic=.02f;mesh.style=1;mesh.paint_mask=texture(d,"textures/rocketbridge/body_grain_blankskin.dds",false);}
    else if(path.find("chassis")!=std::string::npos){mesh.normal_alpha=1;mesh.roughness=.65f;mesh.style=2;}
    else if(path.find("wheel")!=std::string::npos){mesh.normal_alpha=1;mesh.roughness=.88f;mesh.metallic=0;mesh.style=3;}
    else if(ball){mesh.roughness=.58f;mesh.metallic=0;mesh.style=5;}
    else {mesh.roughness=.14f;mesh.metallic=0;mesh.style=4;}
    if(!std::filesystem::exists("Data/"+normal))normal="textures/rocketbridge/flat_n.dds";
    mesh.normal=texture(d,normal,false);
    spdlog::info("Mesh normal map loaded: {} paint mask {}",normal,mesh.paint_mask!=nullptr);
    check(d->CreateBuffer(&bd,&data,&mesh.vertices),"Mesh vertex buffer");meshes.push_back(mesh);
  }
}
void initialize(ID3D11Device* d) {
  ID3DBlob *v{},*p{},*errors{};
  auto compile=[&](const char* entry,const char* profile,ID3DBlob** blob) {
    const auto hr=D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,blob,&errors);
    if(FAILED(hr)) {std::string message=errors?std::string(static_cast<char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Shader compile failed";release(errors);throw std::runtime_error(message);}release(errors);
  };
  compile("VS","vs_5_0",&v);compile("PS","ps_5_0",&p);
  check(d->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs),"Vertex shader");check(d->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps),"Pixel shader");
  D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0}};
  check(d->CreateInputLayout(elements,4,v->GetBufferPointer(),v->GetBufferSize(),&layout),"Mesh layout");release(v);release(p);
  compile("ShadowVS","vs_5_0",&v);check(d->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&shadow_vs),"Shadow vertex shader");release(v);
  compile("BoostPS","ps_5_0",&p);check(d->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&boost_ps),"Boost shader");release(p);
  D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(Constants);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(d->CreateBuffer(&bd,nullptr,&constants),"Constants");
  D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sd.MaxLOD=D3D11_FLOAT32_MAX;check(d->CreateSamplerState(&sd,&sampler),"Sampler");
  D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=true;check(d->CreateRasterizerState(&rd,&raster),"Rasterizer");rd.DepthBias=2;rd.SlopeScaledDepthBias=1.5f;check(d->CreateRasterizerState(&rd,&shadow_raster),"Shadow rasterizer");rd.DepthBias=-2;rd.SlopeScaledDepthBias=-1.5f;check(d->CreateRasterizerState(&rd,&shadow_reverse_raster),"Reverse shadow rasterizer");
  D3D11_BLEND_DESC bl{};bl.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;check(d->CreateBlendState(&bl,&blend),"Blend");
  D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=true;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;check(d->CreateDepthStencilState(&dd,&depth_normal),"Depth");dd.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;check(d->CreateDepthStencilState(&dd,&depth_reverse),"Reverse depth");
  dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;check(d->CreateDepthStencilState(&dd,&boost_depth_reverse),"Boost reverse depth");dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;check(d->CreateDepthStencilState(&dd,&boost_depth_normal),"Boost depth");
  auto& b=bl.RenderTarget[0];b.BlendEnable=true;b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_ONE;b.BlendOp=D3D11_BLEND_OP_ADD;b.SrcBlendAlpha=D3D11_BLEND_ZERO;b.DestBlendAlpha=D3D11_BLEND_ONE;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;check(d->CreateBlendState(&bl,&boost_blend),"Boost blend");
  std::vector<Vertex> flame;
  auto quad=[&](float x,bool vertical) {
    Vertex corners[4]{};
    const float u[4]={0,1,1,0},v[4]={0,0,1,1};
    for(int i=0;i<4;++i) {auto& a=corners[i];const float across=(u[i]-.5f)*(v[i]?18.0f:8.0f);a.position[0]=x+(vertical?0:across);a.position[1]=-43-v[i]*110;a.position[2]=7+(vertical?across:0);a.normal[2]=1;a.uv[0]=u[i];a.uv[1]=v[i];for(float& channel:a.color)channel=1;}
    for(int i:{0,1,2,0,2,3})flame.push_back(corners[i]);
  };
  for(float x:{-18.0f,18.0f}) {quad(x,false);quad(x,true);}
  D3D11_BUFFER_DESC fb{};fb.ByteWidth=flame.size()*sizeof(Vertex);fb.Usage=D3D11_USAGE_IMMUTABLE;fb.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA fd{flame.data(),0,0};check(d->CreateBuffer(&fb,&fd,&boost_vertices),"Boost mesh");boost_count=flame.size();boost_texture=texture(d,"textures/rocketbridge/boost_flame.dds");
  load(d,"Data/SKSE/Plugins/RocketBridge-Car.rmesh",false);load(d,"Data/SKSE/Plugins/RocketBridge-Ball.rmesh",true);
  initialized=true;ready.store(true);spdlog::info("Direct mesh renderer initialized: {} real asset batches",meshes.size());
}
void draw() {
  Pose current;{std::lock_guard lock(pose_mutex);current=pose;}if(!current.active || failed)return;
  auto* renderer=RE::BSGraphics::Renderer::GetSingleton();auto* camera=RE::Main::WorldRootCamera();if(!renderer || !camera)return;
  auto& rd=renderer->GetRuntimeData();auto* ctx=reinterpret_cast<ID3D11DeviceContext*>(rd.context);auto* device=reinterpret_cast<ID3D11Device*>(rd.forwarder);if(!ctx || !device)return;
  try {
    if(!initialized)initialize(device);
    const auto& scene=rd.renderTargets[RE::RENDER_TARGETS::kMAIN];const auto& depth=renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
    auto* rtv=reinterpret_cast<ID3D11RenderTargetView*>(scene.RTV);auto* dsv=reinterpret_cast<ID3D11DepthStencilView*>(depth.views[0]);auto* tex=reinterpret_cast<ID3D11Texture2D*>(scene.texture);if(!rtv || !dsv || !tex)return;
    D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);const auto cam=camera->world.translate;const auto& w2c=camera->GetRuntimeData().worldToCam;
    Constants c{};for(int r=0;r<4;++r) {for(int col=0;col<3;++col)c.viewProj[r][col]=w2c[r][col];c.viewProj[r][3]=float(double(w2c[r][3])+double(w2c[r][0])*cam.x+double(w2c[r][1])*cam.y+double(w2c[r][2])*cam.z);}
    const auto clip_near=camera->GetRuntimeData2().viewFrustum.fNear;float clip[4]{};
    for(int r=0;r<4;++r) {clip[r]=c.viewProj[r][3];for(int col=0;col<3;++col)clip[r]+=c.viewProj[r][col]*camera->world.rotate.entry[col][0]*clip_near;}
    const bool reversed=clip[3]!=0 && clip[2]/clip[3]>.5f;
    c.sunlight[0]=.3f;c.sunlight[1]=-.4f;c.sunlight[2]=.85f;c.sunlight[3]=1;
    c.sunColor[0]=.9f;c.sunColor[1]=.85f;c.sunColor[2]=.75f;
    for(auto& a:c.ambient) {a[0]=.35f;a[1]=.35f;a[2]=.35f;}
    auto color=[](float* out,const RE::NiColor& in) {out[0]=in.red;out[1]=in.green;out[2]=in.blue;};
    if(auto* sky=RE::Sky::GetSingleton()) {
      for(int axis=0;axis<3;++axis) {color(c.ambient[axis*2],sky->directionalAmbientColors[axis][1]);color(c.ambient[axis*2+1],sky->directionalAmbientColors[axis][0]);}
      if(sky->sun && sky->sun->light) {
        auto* light=sky->sun->light.get();const auto dir=light->GetWorldDirection();const auto len=dir.Length();
        if(len>1e-4f) {c.sunlight[0]=-dir.x/len;c.sunlight[1]=-dir.y/len;c.sunlight[2]=-dir.z/len;}
        const auto& ld=light->GetLightRuntimeData();const auto fade=ld.fade>0 && ld.fade<16?ld.fade:1;
        color(c.sunColor,ld.diffuse);for(int i=0;i<3;++i)c.sunColor[i]*=fade;
      }
      if(sky->fogFar>sky->fogNear+1 && sky->fogClamp>0) {
        c.fogRange[0]=1/(sky->fogFar-sky->fogNear);c.fogRange[1]=sky->fogNear*c.fogRange[0];c.fogRange[2]=sky->fogPower>0?sky->fogPower:1;c.fogRange[3]=std::min(sky->fogClamp,1.0f);
        color(c.fogNear,sky->skyColor[RE::TESWeather::ColorTypes::kFogNear]);color(c.fogFar,sky->skyColor[RE::TESWeather::ColorTypes::kFogFar]);
      }
    }
    for(std::size_t i=0;i<std::min<std::size_t>(current.lights.size(),8);++i){const auto& l=current.lights[i];
      c.localPosition[i][0]=l.position.x-cam.x;c.localPosition[i][1]=l.position.y-cam.y;c.localPosition[i][2]=l.position.z-cam.z;c.localPosition[i][3]=l.radius;
      for(int channel=0;channel<3;++channel)c.localColor[i][channel]=l.color[channel];}
    c.renderParams[2]=current.brightness;
    c.renderParams[3]=0; // Native depth shadow mask does not match direct mesh depth.
    StateBackup backup;backup.Save(ctx);
    ID3D11GeometryShader* gs{};ID3D11HullShader* hs{};ID3D11DomainShader* ds{};ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
    ctx->GSSetShader(nullptr,nullptr,0);ctx->HSSetShader(nullptr,nullptr,0);ctx->DSSetShader(nullptr,nullptr,0);
    ctx->OMSetRenderTargets(1,&rtv,dsv);ctx->OMSetBlendState(blend,nullptr,0xffffffff);ctx->OMSetDepthStencilState(reversed?depth_reverse:depth_normal,0);ctx->RSSetState(raster);
    D3D11_VIEWPORT viewport{0,0,float(td.Width),float(td.Height),0,1};ctx->RSSetViewports(1,&viewport);ctx->IASetInputLayout(layout);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs,nullptr,0);ctx->PSSetShader(ps,nullptr,0);ctx->VSSetConstantBuffers(0,1,&constants);ctx->PSSetConstantBuffers(0,1,&constants);ctx->PSSetSamplers(0,1,&sampler);
    for(auto& mesh:meshes) {
      if(!(mesh.ball?current.ball_visible:current.car_visible))continue;
      const auto& object=mesh.ball?current.ball:current.car;
      for(int r=0;r<3;++r) {for(int col=0;col<3;++col)c.model[r][col]=object.rotate.entry[r][col]*object.scale;}
      c.model[0][3]=object.translate.x-cam.x;c.model[1][3]=object.translate.y-cam.y;c.model[2][3]=object.translate.z-cam.z;c.model[3][3]=1;
      c.material[0]=mesh.roughness;c.material[1]=mesh.metallic;c.material[2]=mesh.normal_alpha;c.material[3]=current.sun_visibility[mesh.ball?1:0];c.sunlight[3]=current.sky_visibility[mesh.ball?1:0];
      // The extracted Fennec material's default orange team paint is linear RGB.
      c.paintColor[0]=.84337f;c.paintColor[1]=.196786f;c.paintColor[2]=0;c.paintColor[3]=mesh.paint_mask?1.0f:0.0f;c.surface[0]=mesh.style;
      ctx->PSSetShaderResources(3,1,&mesh.paint_mask);
      ctx->UpdateSubresource(constants,0,nullptr,&c,0,0);UINT stride=sizeof(Vertex),offset=0;ctx->IASetVertexBuffers(0,1,&mesh.vertices,&stride,&offset);ID3D11ShaderResourceView* textures[]={mesh.texture,mesh.normal};ctx->PSSetShaderResources(0,2,textures);ctx->Draw(mesh.count,0);
    }
    if(current.boosting && current.car_visible) {
      const auto& object=current.car;
      const float pulse=1+.12f*std::sin(float(GetTickCount64()%10000)*.075f);
      for(int r=0;r<3;++r)for(int col=0;col<3;++col)c.model[r][col]=object.rotate.entry[r][col]*object.scale*(col==1?pulse:1);
      c.model[0][3]=object.translate.x-cam.x;c.model[1][3]=object.translate.y-cam.y;c.model[2][3]=object.translate.z-cam.z;
      ctx->UpdateSubresource(constants,0,nullptr,&c,0,0);ctx->PSSetShader(boost_ps,nullptr,0);ctx->OMSetBlendState(boost_blend,nullptr,0xffffffff);ctx->OMSetDepthStencilState(reversed?boost_depth_reverse:boost_depth_normal,0);
      UINT stride=sizeof(Vertex),offset=0;ctx->IASetVertexBuffers(0,1,&boost_vertices,&stride,&offset);ctx->PSSetShaderResources(0,1,&boost_texture);ctx->Draw(boost_count,0);
    }
    ctx->GSSetShader(gs,nullptr,0);ctx->HSSetShader(hs,nullptr,0);ctx->DSSetShader(ds,nullptr,0);release(gs);release(hs);release(ds);backup.Restore(ctx);
    static std::uint64_t light_log{};if(GetTickCount64()-light_log>10000){light_log=GetTickCount64();spdlog::info("Bridge lighting: sun {} {} {} ambient-up {} {} {} sun visibility {} {} sky {} {} brightness {} shadow mask {}",c.sunColor[0],c.sunColor[1],c.sunColor[2],c.ambient[4][0],c.ambient[4][1],c.ambient[4][2],current.sun_visibility[0],current.sun_visibility[1],current.sky_visibility[0],current.sky_visibility[1],current.brightness,c.renderParams[3]);}
    static bool logged=false;if(!logged) {logged=true;spdlog::info("Direct mesh draw: scene {}x{} format {} reversed depth {}",td.Width,td.Height,static_cast<unsigned>(td.Format),reversed);}
  } catch(const std::exception& e) {failed=true;ready.store(false);spdlog::error("Direct mesh renderer disabled: {}",e.what());}
}
// Append the actual streamed meshes to the engine's completed sun cascades.
// Leave existing world depth intact; Skyrim then shades its own surfaces normally.
void draw_shadows(RE::BSShadowLight* light) {
  static bool disabled{};if(disabled || !initialized || !light)return;
  Pose current;{std::lock_guard lock(pose_mutex);current=pose;}
  if(!current.active || !current.cast_shadows)return;
  auto* renderer=RE::BSGraphics::Renderer::GetSingleton();auto* camera=RE::Main::WorldRootCamera();if(!renderer || !camera)return;
  auto& runtime=renderer->GetRuntimeData();auto* ctx=reinterpret_cast<ID3D11DeviceContext*>(runtime.context);auto* device=reinterpret_cast<ID3D11Device*>(runtime.forwarder);if(!ctx || !device)return;
  auto& descriptors=light->GetRuntimeData().shadowmapDescriptors;if(descriptors.size()>16)return;
  StateBackup backup;backup.Save(ctx);
  ID3D11GeometryShader* gs{};ID3D11HullShader* hs{};ID3D11DomainShader* ds{};
  ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
  ctx->GSSetShader(nullptr,nullptr,0);ctx->HSSetShader(nullptr,nullptr,0);ctx->DSSetShader(nullptr,nullptr,0);
  unsigned drawn{};
  try {
    const auto cam=camera->world.translate;
    for(const auto& cascade:descriptors){
      auto target=static_cast<unsigned>(cascade.renderTarget);
      if(target>=RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL)target=RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS;
      if(target==RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN || target==RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY)continue;
      const auto& native=renderer->GetDepthStencilData().depthStencils[target];
      auto* texture=reinterpret_cast<ID3D11Texture2D*>(native.texture);auto* native_view=reinterpret_cast<ID3D11DepthStencilView*>(native.views[0]);
      if(!texture || !native_view)continue;
      D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
      if(desc.SampleDesc.Count!=1 || cascade.shadowmapIndex>=desc.ArraySize)continue;
      Constants c{};if(!bridge::shadow_clip_matrix(cascade.lightTransform.m,{cam.x,cam.y,cam.z},c.viewProj))continue;
      RE::NiPoint3 direction{0,0,-1};if(auto* sky=RE::Sky::GetSingleton();sky && sky->sun && sky->sun->light)direction=sky->sun->light->GetWorldDirection();
      const bool reversed=c.viewProj[2][0]*direction.x+c.viewProj[2][1]*direction.y+c.viewProj[2][2]*direction.z<0;
      D3D11_DEPTH_STENCIL_VIEW_DESC vd{};native_view->GetDesc(&vd);
      vd.Flags=0;
      if(desc.ArraySize>1){vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY;vd.Texture2DArray.MipSlice=0;vd.Texture2DArray.FirstArraySlice=cascade.shadowmapIndex;vd.Texture2DArray.ArraySize=1;}
      else{vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;vd.Texture2D.MipSlice=0;}
      ID3D11DepthStencilView* depth{};if(FAILED(device->CreateDepthStencilView(texture,&vd,&depth)))continue;
      ctx->OMSetRenderTargets(0,nullptr,depth);release(depth);
      ctx->OMSetDepthStencilState(reversed?depth_reverse:depth_normal,0);ctx->OMSetBlendState(blend,nullptr,0xffffffff);
      ctx->RSSetState(reversed?shadow_reverse_raster:shadow_raster);
      D3D11_VIEWPORT viewport{0,0,float(desc.Width),float(desc.Height),0,1};ctx->RSSetViewports(1,&viewport);
      ctx->IASetInputLayout(layout);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetShader(shadow_vs,nullptr,0);ctx->PSSetShader(nullptr,nullptr,0);ctx->VSSetConstantBuffers(0,1,&constants);
      for(const auto& mesh:meshes){
        if(!(mesh.ball?current.ball_visible:current.car_visible))continue;
        const auto& object=mesh.ball?current.ball:current.car;
        for(int r=0;r<3;++r)for(int col=0;col<3;++col)c.model[r][col]=object.rotate.entry[r][col]*object.scale;
        c.model[0][3]=object.translate.x-cam.x;c.model[1][3]=object.translate.y-cam.y;c.model[2][3]=object.translate.z-cam.z;c.model[3][3]=1;
        // The extracted Fennec material's default orange team paint is linear RGB.
      c.paintColor[0]=.84337f;c.paintColor[1]=.196786f;c.paintColor[2]=0;c.paintColor[3]=mesh.paint_mask?1.0f:0.0f;c.surface[0]=mesh.style;
      ctx->PSSetShaderResources(3,1,&mesh.paint_mask);
      ctx->UpdateSubresource(constants,0,nullptr,&c,0,0);UINT stride=sizeof(Vertex),offset=0;ctx->IASetVertexBuffers(0,1,&mesh.vertices,&stride,&offset);ctx->Draw(mesh.count,0);
      }
      ++drawn;
    }
  }catch(const std::exception& e){disabled=true;spdlog::error("Mesh shadow casting disabled: {}",e.what());}
  ctx->GSSetShader(gs,nullptr,0);ctx->HSSetShader(hs,nullptr,0);ctx->DSSetShader(ds,nullptr,0);release(gs);release(hs);release(ds);backup.Restore(ctx);
  static std::uint64_t last_log{};if(drawn && GetTickCount64()-last_log>10000){last_log=GetTickCount64();spdlog::info("Real car/ball shadows appended to {} native sun cascades",drawn);}
}
struct ShadowHook {
  static void thunk(RE::BSShadowLight* light,std::uint32_t& index){original(light,index);draw_shadows(light);}
  static inline REL::Relocation<decltype(thunk)> original;
};
struct Hook { static void thunk(bool arg) {original(arg);draw();}static inline REL::Relocation<decltype(thunk)> original; };
}
void publish(bool active,const RE::NiTransform& car,const RE::NiTransform& ball,bool car_visible,bool ball_visible,bool boosting) {
  // Collect on Skyrim's main thread, then copy values to the render thread.
  static std::vector<LocalLight> lights;static std::uint64_t last_lights{};
  static float sun_visibility[2]{1,1},sky_visibility[2]{1,1};static float brightness=1.0f;static bool cast_shadows=true;
  const auto now=GetTickCount64();
  if(active && now-last_lights>200){last_lights=now;lights.clear();
    cast_shadows=GetPrivateProfileIntA("Bridge","CastShadows",1,".\\Data\\SKSE\\Plugins\\SkyrimRocketBridge.ini")!=0;
    char setting[64]{};GetPrivateProfileStringA("Bridge","RenderBrightness","1.0",setting,sizeof setting,".\\Data\\SKSE\\Plugins\\SkyrimRocketBridge.ini");
    try{const float value=std::stof(setting);if(std::isfinite(value))brightness=std::clamp(value,.02f,3.0f);}catch(...){}

    auto* player=RE::PlayerCharacter::GetSingleton();auto* cell=player?player->GetParentCell():nullptr;
    // Test real world geometry for sunlight and overhead shelter on the game thread.
    auto* world=cell?cell->GetbhkWorld():nullptr;auto* native=world?world->GetWorld1():nullptr;
    if(native){
      const float scale=RE::bhkWorld::GetWorldScale();RE::BSReadLockGuard world_lock(world->worldLock);
      auto visible=[&](RE::NiPoint3 start,RE::NiPoint3 direction){
        const auto end=start+direction*12000;
        for(int attempt=0;attempt<12;++attempt){
          RE::bhkPickData pick{};pick.rayInput.from=RE::hkVector4(start*scale);pick.rayInput.to=RE::hkVector4(end*scale);pick.rayInput.enableShapeCollectionFilter=false;pick.rayInput.filterInfo={};native->CastRay(pick.rayInput,pick.rayOutput);
          if(!pick.rayOutput.HasHit())return 1.0f;
          const auto layer=pick.rayOutput.rootCollidable->GetCollisionLayer();
          if(layer==RE::COL_LAYER::kStatic || layer==RE::COL_LAYER::kAnimStatic || layer==RE::COL_LAYER::kTerrain || layer==RE::COL_LAYER::kGround || layer==RE::COL_LAYER::kProps)return 0.0f;
          const auto remaining=end-start;const auto distance=remaining.Length();if(distance<2)return 1.0f;start=start+remaining*std::min(1.0f,pick.rayOutput.hitFraction+2/distance);
        }return 0.0f;
      };
      RE::NiPoint3 sun_direction{.3f,-.4f,.85f};
      if(auto* sky=RE::Sky::GetSingleton();sky && sky->sun && sky->sun->light){sun_direction=sky->sun->light->GetWorldDirection()*-1;const float length=sun_direction.Length();if(length>.001f)sun_direction=sun_direction/length;}
      for(int object=0;object<2;++object){auto origin=(object?ball:car).translate;origin.z+=object?20:30;
        const float sun=visible(origin,sun_direction);float ambient=0;
        for(const auto direction:{RE::NiPoint3{0,0,1},RE::NiPoint3{.707f,0,.707f},RE::NiPoint3{-.707f,0,.707f},RE::NiPoint3{0,.707f,.707f},RE::NiPoint3{0,-.707f,.707f}})ambient+=visible(origin,direction)/5;
        sun_visibility[object]=sun;sky_visibility[object]=.2f+.8f*ambient;
      }
    }
    if(cell)cell->ForEachReference([&](RE::TESObjectREFR* ref){
      if(!ref || ref->IsDisabled() || !ref->Is3DLoaded())return RE::BSContainer::ForEachResult::kContinue;
      auto* base=ref->GetBaseObject();auto* light=base && base->GetFormType()==RE::FormType::Light?static_cast<RE::TESObjectLIGH*>(base):nullptr;
      if(!light || light->data.flags.any(RE::TES_LIGHT_FLAGS::kNegative,RE::TES_LIGHT_FLAGS::kOffByDefault))return RE::BSContainer::ForEachResult::kContinue;
      const auto position=ref->GetPosition();const auto radius=float(light->data.radius);
      if(radius>0 && (position-car.translate).Length()<radius+200){const auto color=light->data.color;
        lights.push_back({position,radius,{color.red/255.0f,color.green/255.0f,color.blue/255.0f}});}
      return RE::BSContainer::ForEachResult::kContinue;});
    std::sort(lights.begin(),lights.end(),[&](const auto& a,const auto& b){return (a.position-car.translate).Length()<(b.position-car.translate).Length();});if(lights.size()>8)lights.resize(8);
  }
  std::lock_guard lock(pose_mutex);pose={active,car_visible,ball_visible,boosting,car,ball,lights,{sun_visibility[0],sun_visibility[1]},{sky_visibility[0],sky_visibility[1]},brightness,cast_shadows};
}
bool available() { return ready.load(); }
void install() {
  const auto target=REL::ID(107142).address();const auto text=REL::Module::get().segment(REL::Segment::textx);const auto* code=reinterpret_cast<const std::uint8_t*>(text.address());unsigned count=0;
  for(std::size_t i=0;i+5<=text.size();++i) {if(code[i]!=0xe8)continue;std::int32_t offset;std::memcpy(&offset,code+i+1,4);if(text.address()+i+5+static_cast<std::intptr_t>(offset)==target) {Hook::original=SKSE::GetTrampoline().write_call<5>(text.address()+i,Hook::thunk);++count;}}
  REL::Relocation<std::uintptr_t> sun{RE::VTABLE_BSShadowDirectionalLight[0]};
  ShadowHook::original=sun.write_vfunc(0x0A,ShadowHook::thunk);
  spdlog::info("Native sun shadow hook installed; actual car and ball geometry");
  spdlog::info("Direct mesh renderer RenderWorld hooks {}",count);
}
}
