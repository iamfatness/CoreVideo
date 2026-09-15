#pragma once

#include "iso-frame-conform.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string>
#include <wrl/client.h>

namespace corevideo::modules
{

// Adapted from CoreVideo Pro's D3DIsoFrameConformer. This transport needs planar
// I420 (Pro's writer takes NV12), with full-range black. Owned by one ISO worker;
// no immediate context is shared with the live compositor.
class D3DIsoFrameConformer
{
  public:
    explicit D3DIsoFrameConformer(bool warpForTest = false) : warpForTest_(warpForTest) {}
    bool prepare(std::string &error) { return device_ || initialize(error); }

    bool convert(const std::vector<uint8_t> &input, int sw, int sh, int dw, int dh,
                 std::vector<uint8_t> &output, std::string &error)
    {
        if (sw < 2 || sh < 2 || dw < 2 || dh < 2 || sw > 8192 || sh > 8192 || dw > 8192 ||
            dh > 8192 || ((sw | sh | dw | dh) & 1) || input.size() < size_t(sw) * sh * 3 / 2) {
            error = "invalid I420 conformance dimensions or payload";
            return false;
        }
        if (!device_ && !initialize(error))
            return false;
        const UINT inputBytes = UINT(size_t(sw) * sh * 3 / 2);
        const UINT outputBytes = UINT(size_t(dw) * dh * 3 / 2);
        if ((inputBytes + 3u) / 4u * 4u != inputCapacity_ ||
            (outputBytes + 3u) / 4u * 4u != outputCapacity_) {
            if (!allocate(inputBytes, outputBytes, error))
                return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(input_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            error = "map ISO GPU upload";
            return false;
        }
        std::memcpy(mapped.pData, input.data(), inputBytes);
        if (inputCapacity_ > inputBytes)
            std::memset(static_cast<uint8_t *>(mapped.pData) + inputBytes, 0,
                        inputCapacity_ - inputBytes);
        context_->Unmap(input_.Get(), 0);
        const auto fit = isoFitRect(sw, sh, dw, dh);
        const UINT groups = (outputCapacity_ / 4u + 255u) / 256u;
        const UINT groupsX = std::min(groups, 65535u);
        const UINT constants[12] = {UINT(sw),    UINT(sh),    UINT(dw),        UINT(dh),
                                    UINT(fit.x), UINT(fit.y), UINT(fit.width), UINT(fit.height),
                                    inputBytes,  outputBytes, groupsX * 256u,  0};
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, constants, 0, 0);
        ID3D11ShaderResourceView *srv = inputView_.Get();
        ID3D11UnorderedAccessView *uav = outputView_.Get();
        ID3D11Buffer *cb = constants_.Get();
        context_->CSSetShader(shader_.Get(), nullptr, 0);
        context_->CSSetShaderResources(0, 1, &srv);
        context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context_->CSSetConstantBuffers(0, 1, &cb);
        context_->Dispatch(groupsX, (groups + groupsX - 1) / groupsX, 1);
        srv = nullptr;
        uav = nullptr;
        context_->CSSetShaderResources(0, 1, &srv);
        context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context_->CopyResource(readback_.Get(), output_.Get());
        if (FAILED(context_->Map(readback_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            error = "map ISO GPU readback";
            return false;
        }
        output.resize(outputBytes);
        std::memcpy(output.data(), mapped.pData, outputBytes);
        context_->Unmap(readback_.Get(), 0);
        return true;
    }

  private:
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    bool initialize(std::string &error)
    {
        D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(
                nullptr, warpForTest_ ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                requested, 1, D3D11_SDK_VERSION, &device_, &level, &context_))) {
            error = "create ISO GPU conformance device";
            return false;
        }
        // Four output bytes per invocation avoid sub-word UAV write races. All
        // box bounds, rounding, chroma alignment, and bar levels match the oracle.
        static constexpr char source[] = R"(
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
cbuffer Parameters : register(b0) { uint4 dims; uint4 fit; uint4 sizes; };
uint byteAt(uint i) { return (src.Load(i & ~3u) >> ((i & 3u) * 8u)) & 255u; }
uint box(uint offset, uint pw, uint ph, uint x, uint y, uint rw, uint rh) {
  uint x0=x*pw/rw, x1=min(pw,max(x0+1,(x+1)*pw/rw));
  uint y0=y*ph/rh, y1=min(ph,max(y0+1,(y+1)*ph/rh));
  uint total=0, count=(x1-x0)*(y1-y0);
  for(uint sy=y0;sy<y1;++sy) for(uint sx=x0;sx<x1;++sx) total+=byteAt(offset+sy*pw+sx);
  return (total+count/2)/count;
}
uint pixel(uint i) {
  if(i>=sizes.y) return 0;
  uint luma=dims.z*dims.w;
  if(i<luma) {
    uint x=i%dims.z,y=i/dims.z;
    if(x<fit.x || x>=fit.x+fit.z || y<fit.y || y>=fit.y+fit.w) return 0;
    return box(0,dims.x,dims.y,x-fit.x,y-fit.y,fit.z,fit.w);
  }
  uint cl=luma/4,j=i-luma,plane=j/cl,p=j%cl,x=p%(dims.z/2),y=p/(dims.z/2);
  if(x<fit.x/2 || x>=(fit.x+fit.z)/2 || y<fit.y/2 || y>=(fit.y+fit.w)/2) return 128;
  uint sl=dims.x*dims.y;
  return box(sl+plane*(sl/4),dims.x/2,dims.y/2,x-fit.x/2,y-fit.y/2,fit.z/2,fit.w/2);
}
[numthreads(256,1,1)] void main(uint3 tid : SV_DispatchThreadID) {
  uint at=(tid.x+tid.y*sizes.z)*4;
  if(at>=sizes.y) return;
  dst.Store(at,pixel(at)|(pixel(at+1)<<8)|(pixel(at+2)<<16)|(pixel(at+3)<<24));
})";
        Ptr<ID3DBlob> code, diagnostics;
        const HRESULT compiled = D3DCompile(
            source, sizeof(source) - 1, "IsoBoxConform", nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &diagnostics);
        if (FAILED(compiled) ||
            FAILED(device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                nullptr, &shader_))) {
            error = "compile ISO GPU box filter";
            device_.Reset();
            context_.Reset();
            return false;
        }
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 48;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &constants_))) {
            error = "create ISO GPU constants";
            device_.Reset();
            context_.Reset();
            return false;
        }
        return true;
    }
    bool allocate(UINT inBytes, UINT outBytes, std::string &error)
    {
        inputView_.Reset();
        outputView_.Reset();
        input_.Reset();
        output_.Reset();
        readback_.Reset();
        inputCapacity_ = outputCapacity_ = 0;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = (inBytes + 3u) / 4u * 4u;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        HRESULT hr = device_->CreateBuffer(&desc, nullptr, &input_);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        srv.BufferEx.NumElements = desc.ByteWidth / 4;
        srv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (SUCCEEDED(hr))
            hr = device_->CreateShaderResourceView(input_.Get(), &srv, &inputView_);
        desc.ByteWidth = (outBytes + 3u) / 4u * 4u;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.CPUAccessFlags = 0;
        if (SUCCEEDED(hr))
            hr = device_->CreateBuffer(&desc, nullptr, &output_);
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = desc.ByteWidth / 4;
        uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (SUCCEEDED(hr))
            hr = device_->CreateUnorderedAccessView(output_.Get(), &uav, &outputView_);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (SUCCEEDED(hr))
            hr = device_->CreateBuffer(&desc, nullptr, &readback_);
        if (FAILED(hr)) {
            error = "allocate ISO GPU conformance buffers";
            return false;
        }
        inputCapacity_ = (inBytes + 3u) / 4u * 4u;
        outputCapacity_ = (outBytes + 3u) / 4u * 4u;
        return true;
    }
    bool warpForTest_ = false;
    UINT inputCapacity_ = 0, outputCapacity_ = 0;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    Ptr<ID3D11ComputeShader> shader_;
    Ptr<ID3D11Buffer> input_, output_, readback_, constants_;
    Ptr<ID3D11ShaderResourceView> inputView_;
    Ptr<ID3D11UnorderedAccessView> outputView_;
};
} // namespace corevideo::modules
