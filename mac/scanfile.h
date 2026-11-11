// scanfile.h -- reads a file for the strings check (core/str_scan.h): the whole file up to 2*READ_HALF bytes, else its first and last READ_HALF bytes with 8 zero
// bytes between them (as the Windows app reads it). Shared by mac/sentinel-strings.cpp and the Mac app (mac/core.cpp).
#pragma once
#include "str_scan.h"
#include <filesystem>
#include <fstream>

inline bool readCapped(const std::filesystem::path& p,std::vector<uint8_t>& buf){
  std::ifstream f(p,std::ios::binary); if(!f) return false;
  f.seekg(0,std::ios::end); const std::streamoff size=f.tellg(); if(size<0) return false;
  auto take=[&](std::streamoff off,size_t len){ const size_t k=buf.size(); buf.resize(k+len); f.clear(); f.seekg(off); f.read((char*)buf.data()+k,(std::streamsize)len); buf.resize(k+(size_t)f.gcount()); };
  const size_t half=strscan::READ_HALF;
  if((size_t)size<=2*half) take(0,(size_t)size);
  else{ take(0,half); buf.insert(buf.end(),8,0); take(size-(std::streamoff)half,half); }
  return size==0||!buf.empty();
}
