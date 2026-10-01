#pragma once
#include "transform.hpp"
namespace bridge {
// Skyrim stores world-to-shadow UV/depth; D3D rasterization needs clip XY.
// Rebase on the world camera so drawing remains precise far from the origin.
inline bool shadow_clip_matrix(const float source[4][4],Vec3 camera,float output[4][4]) {
  float uv[4][4]{};
  const bool row_vector=std::abs(source[0][3])+std::abs(source[1][3])+std::abs(source[2][3])<1e-6f;
  const float position[]={camera.x,camera.y,camera.z};
  for(int r=0;r<4;++r){
    for(int c=0;c<3;++c)uv[r][c]=row_vector?source[c][r]:source[r][c];
    uv[r][3]=row_vector?source[3][r]:source[r][3];
    for(int c=0;c<3;++c)uv[r][3]+=uv[r][c]*position[c];
  }
  for(int c=0;c<4;++c){output[0][c]=2*uv[0][c]-uv[3][c];output[1][c]=uv[3][c]-2*uv[1][c];output[2][c]=uv[2][c];output[3][c]=uv[3][c];}
  for(int r=0;r<4;++r)for(int c=0;c<4;++c)if(!finite(output[r][c]))return false;
  return std::abs(output[3][3])>1e-6f;
}
}
