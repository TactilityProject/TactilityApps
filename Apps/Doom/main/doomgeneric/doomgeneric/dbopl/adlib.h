#pragma once
#include "dosbox.h"
namespace Adlib { struct Handler {
  virtual void WriteReg(Bitu reg, uint8_t val) = 0;
  virtual Bitu WriteAddr(Bitu port, uint8_t val) = 0;
  virtual void Generate(void* chan, Bitu samples) = 0;
  virtual void Init(Bitu rate) = 0;
  virtual ~Handler() {}
}; }
