## What this does
Improves GPU utilization in D3D11-based Atelier games and can dramatically improve performance as a result.

A quick test on my 6900XT in early areas of Sophie 2 shows the following improvements (on Proton, using DXVK 1.10.1 and Mesa 22.0):

| Location | Before | After |
|----------|--------|-------|
| Menu (1440p) | 104.7 | **144.0** |
| Menu (4k) | 63.4 | **82.3** |
| Commercial District (1440p) | 83.3 | **144.0** |
| Commercial district (4k) | 51.0 | **72.9** |

## The issue
The engine of these games has serious issues with GPU under-utilization ever since they switched to D3D11 with Firis, due to the way data is exchanged between the CPU and GPU. It's likely that they were trying to emulate D3D9 resource management in a really bad way and never bothered to fix it, and with each new game it gets *worse*. Sophie 2 sets a sad new record with roughly 20 GPU sync points in the main menu.

Basically, what happens is as follows (in pseudo-code):
```c++
ID3D11Buffer* stagingBuffer;
D3D11_MAPPED_SUBRESOURCE mapped;
D3D11_BUFFER_DESC desc;
// ..
desc.Usage = D3D11_USAGE_STAGIG;
desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
d3d11Device->CreateBuffer(&desc, nullptr, &stagingBuffer);
d3d11Context->CopyResource(stagingBuffer, gpuResource);  // <- this is executed on the GPU
d3d11Context->Map(stagingBuffer, 0, D3D11_MAP_READ_WRITE, 0, &mapped); // <- this waits for CopyResource to complete
// ... some CPU work here to write to the mapped buffer
d3d11Context->Unmap(stagingBuffer, 0);
d3d11Context->CopySubresourceRegion(gpuResource, 0, ..., stagingBuffer, 0, ...); //< this is done on the GPU again
```

Not only does this force full CPU-GPU synchronization at the start of each frame, this also happens **multiple times every frame**, back-to-back, and with all different kinds of resources (vertex buffers, a render target, you name it). They somehow even manage to have other buffers with `D3D11_USAGE_DYNAMIC` or `D3D11_USAGE_STAGING` in that mess whcih they *could* map directly, but no, Gust prefers GPU synchronization.

The fact that there are multiple sync points back-to-back makes this especially problematic since submitting those infividual copy commands that happen between calls to `Map` is fairly costly on the driver side.

## The solution
It's actually quite simple: Instead of doing all those nasty `CopyResource` calls on the GPU, we just do them on the CPU.

However, we can't just map the GPU resources directly for the most part, so for each GPU resource that's being copied into a staging buffer, we create *another* staging buffer - but unlike the game, we keep it around, and update it each time the GPU resource itself gets updated. By the time the game calls `CopyResource`, the GPU may not be done using all those shadow resources yet, so we will still synchronize, but at worst we'll now synchronize with one single copy command from the *previous* frame, not with dozens of copy commands in the *current* frame.

## Caveats
- Memory usage as well as CPU utilization are increased.
- Not all GPU sync points are caught. There are some genuine data dependencies that can't easily be worked around this way, so there will still be situations where GPU load is low, or where the game will stutter briefly.
- I haven't tested this on Windows at all yet, or in any sort of long gameplay sessions. There may be stability issues.
