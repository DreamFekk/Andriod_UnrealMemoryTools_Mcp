#include "../src/VectorValue.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
int main(){
    assert(VectorValue::Width("Vector",12)==4);
    assert(VectorValue::Width("Vector",24)==8);
    assert(VectorValue::Width("Vector3f",24)==0);
    assert(VectorValue::Width("Vector3d",24)==8);
    assert(VectorValue::Width("Vector_NetQuantize100",12)==4);
    assert(VectorValue::Width("Transform",48)==0);
    assert(VectorValue::Width("Vector",8)==0);
    const float values[]{-12.5f,0.125f,8192.75f};
    auto f=VectorValue::Decode(values,4);
    assert(f[0]==-12.5 && f[1]==.125 && f[2]==8192.75);
    assert(VectorValue::Format(f,4).find("X=-12.5")!=std::string::npos);
    const double large[]{-123456789.1234567,1.0e-100,0.0};
    auto d=VectorValue::Decode(large,8);
    assert(d[0]==large[0] && d[1]==large[1]);
    const float special[]{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),-0.f};
    auto n=VectorValue::Decode(special,4);
    assert(std::isinf(n[0]) && std::isnan(n[1]) && std::signbit(n[2]));
    std::cout<<"PASS: FVector float32/float64, signed/fractional/large/nonfinite values, type/size validation\n";
}
