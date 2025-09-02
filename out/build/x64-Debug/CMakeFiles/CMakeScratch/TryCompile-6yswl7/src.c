
      #include <winsdkver.h>
      #include <sdkddkver.h>
      #include <d3d12.h>
      ID3D12Device1 *device;
      #if WDK_NTDDI_VERSION > 0x0A000008
      int main(int argc, char **argv) { return 0; }
      #endif
