// afk_overlay_d2d.h -- the AFK screen's HUD drawn with Direct2D / DirectWrite straight onto the swap-chain back buffer:
// starfield (dimmed inside the lens circle), camera-lens vignette, bottom gradient, clock, auto-fitted quote, hint line.
// Same layout numbers and fade stages as the GDI+ renderer (afk_helpers.h), but GPU-composited at the display refresh rate.
#pragma once
#include "afk_backend.h"

class AfkOverlayD2D{
 public:
  AfkOverlayD2D();
  ~AfkOverlayD2D();
  AfkOverlayD2D(const AfkOverlayD2D&)=delete; AfkOverlayD2D& operator=(const AfkOverlayD2D&)=delete;

  // d3dDevice: ID3D11Device*, backBuffer: ID3D11Texture2D* (BGRA). videoFade (0..1) is how visible the video is, so the
  // vignette fades in with it. Returns false when Direct2D / DirectWrite are unusable -- the video then plays without HUD.
  bool draw(void* d3dDevice,void* backBuffer,unsigned W,unsigned H,bool haveVideo,float videoFade,const AfkFrameParams& p);
  void releaseTargets();     // drop references to back-buffer textures (before ResizeBuffers / engine shutdown)
  void releaseAll();         // drop everything, including the Direct2D device
  bool usable() const;
 private:
  struct Impl;
  Impl* m;
};
