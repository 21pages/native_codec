#include "win.h"
#include <fstream>
bool dumpTexture(ID3D11Device *device, ID3D11Texture2D *texture,
                 const string &filename) {
  const char *dir = "texture";
  DWORD attrib = GetFileAttributesA(dir);
  if (attrib == INVALID_FILE_ATTRIBUTES ||
      !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
    if (!CreateDirectoryA(dir, NULL)) {
      std::cout << "Failed to create directory: " << dir << std::endl;
      return false;
    } else {
      std::cout << "Directory created: " << dir << std::endl;
    }
  } else {
    // already exists
  }

  D3D11_TEXTURE2D_DESC desc = {};
  ComPtr<ID3D11DeviceContext> deviceContext;
  HRESULT hr;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.BindFlags = 0;
  ComPtr<ID3D11Texture2D> stagingTexture;
  hr = device->CreateTexture2D(&desc, nullptr, stagingTexture.GetAddressOf());
  IF_FAILED_THROW(hr);
  device->GetImmediateContext(deviceContext.ReleaseAndGetAddressOf());
  deviceContext->CopyResource(stagingTexture.Get(), texture);

  D3D11_MAPPED_SUBRESOURCE mappedResource = {};
  deviceContext->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0,
                     &mappedResource);
  string path = string(dir) + "/" + filename;
  std::ofstream file(path, std::ios::binary | std::ios::app);
  int bpp = 32;
  if (desc.Format == DXGI_FORMAT_NV12) {
    bpp = 12;
  }
  int rowPitch = desc.Width * bpp / 8;
  char *p = (char *)mappedResource.pData;
  // for (int i = 0; i < desc.Height; i++) {
  //   file.write(p, rowPitch);
  //   p += mappedResource.RowPitch;
  // }

  file.close();
  return true;
}