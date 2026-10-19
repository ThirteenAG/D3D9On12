# D3D9On12

This fork maintains the D3D9-to-D3D12 mapping layer with correctness and CPU
performance improvements, reproducible dependency patches, and runtime regression
probes. It is an independent fork of Microsoft's MIT-licensed project.

Release archives contain `d3d9on12.dll`, its matching `dxbcSigner.dll`, dependency
licenses, and build metadata. Win32 targets 32-bit applications; x64 targets 64-bit
applications. An application or loader must select D3D9On12 through
`Direct3DCreate9On12` or `Direct3DCreate9On12Ex` and load the application-local
mapping layer. Copying these files alone does not redirect ordinary D3D9 calls.
Keep Windows' system copies unchanged.

The release configuration includes sampler validation, SYSTEMMEM buffer upload
corrections, query lifetime fixes, redundant constant-write filtering,
asynchronous translation-layer recording, grouped per-draw recording, root-CB0
constant bindings, and resource allocation improvements.
Added optional interfaces use the `D3D9On12` prefix; see
[the interface header](include/D3D9On12Extensions.h).

D3D9On12 is a mapping layer, which maps graphics commands from D3D9 to D3D12. D3D9On12 is **not** an implementation of the D3D9 **API**, but is instead an implementation of the D3D9 usermode **DDI** (device driver interface). That means it is not a binary named d3d9.dll, but is named d3d9on12.dll.

When an application creates a D3D9 device, they may choose for it to be a D3D9On12 device, rather than a native D3D9 device (see [Direct3DCreate9On12](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/TranslationLayerResourceInterop.md#function-direct3dcreate9on12) and [Direct3DCreate9On12Ex](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/TranslationLayerResourceInterop.md#function-direct3dcreate9on12ex)). When this happens, d3d9on12.dll is loaded by the D3D9 runtime and initialized. When the application calls rendering commands, D3D9 will validate those commands, and then will convert those commands to the D3D9 DDI and send it to D3D9On12, just like any D3D9 driver. D3D9On12 will take these commands and convert them into D3D12 API calls, which are further validated by the D3D12 runtime, optionally including the D3D12 debug layer, which are then converted to the D3D12 DDI and sent to the D3D12 driver.

Note that D3D9On12 is an *enlightened* D3D9 driver, and there are several places where it receives additional information compared to a traditional D3D9 driver, either to enable it to provide API-level information to D3D12 rather than driver-level information (as is the case for shaders), or to enable interop scenarios. When a D3D9 device is created with D3D9On12, the device will expose an `IDirect3DDevice9On12` interface which enables applications to submit work to both the D3D9 API and the D3D12 API with lightweight sharing and synchronization.

For more details about D3D9On12, see:
* [D3D Translation Layer spec](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/TranslationLayerResourceInterop.md)
* [D3D9On12 blog posts](https://devblogs.microsoft.com/directx/coming-to-directx-12-d3d9on12-and-d3d11on12-resource-interop-apis/)

Make sure that you visit the [DirectX Landing Page](https://devblogs.microsoft.com/directx/landing-page/) for more resources for DirectX developers.

## How does it work?

The primary entrypoint to D3D9On12 is a custom version of the normal D3D9 driver [OpenAdapter](https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_openadapter) entrypoint, named `OpenAdapter_Private`, where D3D9 provides additional information to the mapping layer. In response to this, like a normal driver, D3D9On12 returns an adapter object, which exposes DDIs to create a device. The device is created like normal, but in addition to the normal DDI tables, D3D9on12 also exposes a set of DDIs defined by the `D3D9ON12_PRIVATE_DDI_TABLE` which can be retrieved by calling `GetPrivateDDITable`.

The device object internally uses an instance of the D3D12TranslationLayer immediate context to record commands. Similarly, most D3D9On12 objects are backed by an implementation from the D3D12TranslationLayer library. The code in this repository is largely an adaptor from the D3D9 DDI to the D3D12TranslationLayer library, where the real heavy lifting of converting to the D3D12 domain is done.

## Building

Use CMake with Visual Studio and the Windows SDK/WDK. Dependencies are pinned and
verified before compilation. A clean build downloads D3D12TranslationLayer and
DirectX-Headers; restore the shader signer with NuGet before building.

```powershell
cmake -S . -B build -A Win32 -DUSE_PIX=OFF -DD3D9ON12_BUILD_TESTS=ON
nuget install Microsoft.Direct3D.DxbcSigner -Version 1.0.0 -OutputDirectory build/packages
cmake --build build --config Release --parallel
./tests/run_probes.ps1 -Build build
./scripts/Package-Release.ps1 -Build build -Output packages
```

Use `-A x64` in a separate build directory for 64-bit applications.

## Why open source?

The D3D9On12 mapping layer is included as an operating system component of Windows 10. Over the years and Windows 10 releases, it has grown in functionality, to the point where it is a complete and relatively performant implementation of a D3D9 driver. We are choosing to release the source to this component for two primary reasons:
1. To enable the community to contribute bugfixes and further performance improvements, which will improve the stability and performance of Windows 10. See [CONTRIBUTING](CONTRIBUTING.md).
2. To serve as an example of how to use the D3D12TranslationLayer library.

### What can you do with this?

There are minor differences between binaries built out of this repository and the versions that are included in the OS. To that end, shipping applications should not attempt to override the OS version of D3D9On12 with versions that they have built. We will not guarantee that newer versions of Windows will continue to support older versions of D3D9On12, since it is an OS component which may be revised together with D3D9. However, developers are welcome to override the Windows version of D3D9On12 for *local testing and experimentation*.

### Compatibility

When possible, we will attempt to maintain compatibility between D3D9 and D3D9On12. One should expect that the tip of D3D9On12's `master` branch should work nicely with the latest release of Windows 10. Support for configurations other than that are not guaranteed to work.

## Data Collection

The software may collect information about you and your use of the software and send it to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may turn off the telemetry as described in the repository. There are also some features in the software that may enable you and Microsoft to collect data from users of your applications. If you use these features, you must comply with applicable law, including providing appropriate notices to users of your applications together with a copy of Microsoft's privacy statement. Our privacy statement is located at https://go.microsoft.com/fwlink/?LinkID=824704. You can learn more about data collection and use in the help documentation and our privacy statement. Your use of the software operates as your consent to these practices.

Note however that no data collection is performed when using your private builds.
