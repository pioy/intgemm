#pragma once

namespace intgemm {
namespace callbacks {

struct Dummy {
};

template <typename OutputBufferType>
struct Write {
  OutputBufferType* addr;

  Write(OutputBufferType* addr) : addr(addr) {}
};

struct UnquantizeAndWrite {
  float unquant_mult;
  float* addr;

  UnquantizeAndWrite(float unquant_mult, float* addr) : unquant_mult(unquant_mult), addr(addr) {}
};

struct AddBiasAndWrite {
  const int* bias_addr;
  int* output_addr;

  AddBiasAndWrite(const int* bias_addr, int* output_addr) :  bias_addr(bias_addr), output_addr(output_addr) {}
};

struct UnquantizeAndAddBiasAndWrite {
  float unquant_mult;
  const float* bias_addr;
  float* output_addr;

  UnquantizeAndAddBiasAndWrite(float unquant_mult, const float* bias_addr, float* output_addr) : unquant_mult(unquant_mult), bias_addr(bias_addr), output_addr(output_addr) {}
};

}
}