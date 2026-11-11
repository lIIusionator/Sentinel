// com_ptr.h -- minimal owning COM pointer (no WRL dependency, so older MinGW toolchains build it too).
#pragma once

template<class T> struct Com{
  T* p=nullptr;
  Com(){}
  ~Com(){ reset(); }
  Com(const Com&)=delete; Com& operator=(const Com&)=delete;
  void reset(){ if(p){ p->Release(); p=nullptr; } }
  T** put(){ reset(); return &p; }                  // for out-parameters
  T* get() const { return p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p!=nullptr; }
  void attach(T* x){ reset(); p=x; }                // takes over one reference
  T* detach(){ T* t=p; p=nullptr; return t; }
};
