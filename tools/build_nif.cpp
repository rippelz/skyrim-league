// SPDX-License-Identifier: GPL-3.0-or-later
#include <NifFile.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace nifly;
using Json=nlohmann::json;
int main(int argc,char** argv) {
  try {
    if(argc!=3) throw std::runtime_error("usage: build_nif scene.json output.nif");
    std::ifstream input(argv[1]);Json scene;input>>scene;
    NifFile nif;nif.Create(NiVersion::getSSE());unsigned count=0;
    for (const auto& mesh:scene.at("meshes")) {
      std::vector<Vector3> vertices,normals;std::vector<Vector2> uv;
      std::vector<Triangle> triangles;std::vector<Color4> colors;
      for(const auto& p:mesh.at("vertices"))vertices.emplace_back(p[0].get<float>(),p[1].get<float>(),p[2].get<float>());
      if(vertices.size()>65535)throw std::runtime_error("NIF shape vertex limit exceeded");
      for(const auto& p:mesh.at("normals"))normals.emplace_back(p[0].get<float>(),p[1].get<float>(),p[2].get<float>());
      for(const auto& p:mesh.at("uvs"))uv.emplace_back(p[0].get<float>(),p[1].get<float>());
      for(const auto& p:mesh.at("colors"))colors.emplace_back(p[0].get<float>(),p[1].get<float>(),p[2].get<float>(),p[3].get<float>());
      for(const auto& t:mesh.at("triangles"))triangles.emplace_back(t[0].get<uint16_t>(),t[1].get<uint16_t>(),t[2].get<uint16_t>());
      auto* shape=nif.CreateShapeFromData(mesh.at("name").get<std::string>(),&vertices,&triangles,&uv,&normals);
      // SSE writes float32 positions. Declare that precision in VertexDesc too;
      // otherwise the runtime decodes those bytes as float16 geometry.
      auto* tri=dynamic_cast<BSTriShape*>(shape);
      if (!tri) throw std::runtime_error("Expected SSE triangle shape");
      tri->SetFullPrecision(true);
      shape->flags=14;nif.SetColorsForShape(shape,colors);
      auto* shader=dynamic_cast<BSLightingShaderProperty*>(nif.GetShader(shape));
      if(!shader)throw std::runtime_error("Missing lighting shader");
      shader->shaderFlags1=0x80000301U;shader->shaderFlags2=0x89U;
      shader->SetVertexColors(true);shader->SetDoubleSided(true);shader->glossiness=30;shader->specularStrength=.3f;
      auto diffuse=mesh.at("texture").get<std::string>();auto normal=mesh.at("normal").get<std::string>();
      nif.SetTextureSlot(shape,diffuse,0);nif.SetTextureSlot(shape,normal,1);++count;
    }
    // Visual geometry only: Rocket League remains the physics authority.
    if(nif.Save(argv[2])!=0)throw std::runtime_error("NIF save failed");
    NifFile check;
    if(check.Load(argv[2])!=0||check.GetShapes().size()!=count)throw std::runtime_error("NIF reload failed");
    for(auto* shape:check.GetShapes()) {
      auto* tri=dynamic_cast<BSTriShape*>(shape);
      if(!tri || !tri->IsFullPrecision())throw std::runtime_error("SSE position precision missing");
    }
    std::cout<<argv[2]<<": "<<count<<" shapes validated\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
