#include <vector>
#include "app/gpu/dxr_scene.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32) && !defined(M2_MOBILE)
#define M2_DXR 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl/client.h>
#include "app/gpu/dxr_shaders_gen.h"
using Microsoft::WRL::ComPtr;
#endif

namespace app {

#if M2_DXR
namespace {

constexpr int kSlots = 3; // frames the queue may hold at once (SDL_GPU keeps up to 3)

ComPtr<ID3D12Resource> make_buffer(ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE heap_type,
                                   D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<UINT64>(size, 256);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

// Grows `buffer` (a default-heap UAV buffer) to at least `size`, doubling.
bool ensure_uav(ID3D12Device *device, ComPtr<ID3D12Resource> &buffer, UINT64 &capacity, UINT64 size,
                D3D12_RESOURCE_STATES state) {
    if (buffer && capacity >= size) return true;
    const UINT64 grown = std::max<UINT64>(size, capacity * 2);
    buffer = make_buffer(device, grown, D3D12_HEAP_TYPE_DEFAULT, state, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    capacity = buffer ? grown : 0;
    return buffer != nullptr;
}

} // namespace

struct DxrScene::Impl {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue; // SDL_GPU's direct queue (not owned)
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    ComPtr<ID3D12QueryHeap> queries;
    double tick_ms = 0;
    // the shadow pass (dxr_shadow.hlsl): root signature from its DXIL, and
    // per slot two descriptors (view z SRV, light UAV)
    ComPtr<ID3D12RootSignature> shadow_root;
    ComPtr<ID3D12PipelineState> shadow_pipe;
    // the light shafts (cs_shafts, same root signature): one float per 8x8
    // pixels, written then read on the same queue
    ComPtr<ID3D12PipelineState> shafts_pipe, shadow_large_pipe, shafts_large_pipe;
    ComPtr<ID3D12Resource> haze;
    UINT64 haze_cap = 0;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    UINT descriptor_size = 0;
    // texture RAM for the cut-out test, copied when the board's generation changes
    static constexpr UINT64 kTexramBytes = 0x80000ull * 4 * 2;
    ComPtr<ID3D12Resource> texram;
    uint64_t texram_generation = ~0ull;

    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList4> list;
        ComPtr<ID3D12Resource> upload, blas, tlas, scratch, readback, tex_upload;
        UINT64 upload_cap = 0, blas_cap = 0, tlas_cap = 0, scratch_cap = 0;
        UINT64 done = 0;     // fence value that marks this slot's last build finished
        bool timed = false;  // readback holds two timestamps not yet counted
    } slots[kSlots];
    int next = 0;

    // statistics
    uint64_t frames = 0, triangles = 0, cutouts = 0, timed_frames = 0, failures = 0, shaded = 0;
    double gpu_ms_sum = 0, gpu_ms_max = 0;
    std::vector<float> gpu_ms_all; // every timed frame: the report's p99 and long frames

    ~Impl() {
        if (fence && event) { // let the queue finish with our buffers
            for (auto &slot : slots)
                if (fence->GetCompletedValue() < slot.done) {
                    fence->SetEventOnCompletion(slot.done, event);
                    WaitForSingleObject(event, 2000);
                }
        }
        if (event) CloseHandle(event);
    }

    void collect(Slot &slot) {
        if (!slot.timed) return;
        slot.timed = false;
        void *mapped = nullptr;
        const D3D12_RANGE range{0, 16};
        if (FAILED(slot.readback->Map(0, &range, &mapped))) return;
        uint64_t stamps[2];
        std::memcpy(stamps, mapped, sizeof stamps);
        const D3D12_RANGE none{0, 0};
        slot.readback->Unmap(0, &none);
        if (stamps[1] < stamps[0]) return;
        const double ms = double(stamps[1] - stamps[0]) * tick_ms;
        gpu_ms_sum += ms;
        gpu_ms_max = std::max(gpu_ms_max, ms);
        gpu_ms_all.push_back(float(ms));
        ++timed_frames;
    }
};

DxrScene::DxrScene() = default;
DxrScene::~DxrScene() = default;

bool DxrScene::ok() const { return impl_ != nullptr; }

bool DxrScene::init(SDL_GPUDevice *dev) {
    impl_.reset();
    const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(dev);
    auto *raw_device = static_cast<ID3D12Device *>(SDL_GetPointerProperty(props, "SDL.gpu.device.d3d12.device", nullptr));
    auto *raw_queue = static_cast<ID3D12CommandQueue *>(
        SDL_GetPointerProperty(props, "SDL.gpu.device.d3d12.command_queue", nullptr));
    if (!raw_device || !raw_queue) {
        error_ = "not a Direct3D 12 device with the native handles (patch 0002)";
        return false;
    }
    auto impl = std::make_unique<Impl>();
    if (FAILED(raw_device->QueryInterface(IID_PPV_ARGS(&impl->device)))) {
        error_ = "ID3D12Device5 unavailable";
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    if (FAILED(impl->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof options)) ||
        options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        error_ = "ray tracing tier 1.1 unsupported by this GPU or driver";
        return false;
    }
    impl->queue = raw_queue;
    UINT64 frequency = 0;
    if (FAILED(raw_queue->GetTimestampFrequency(&frequency)) || !frequency) {
        error_ = "no timestamp frequency";
        return false;
    }
    impl->tick_ms = 1000.0 / double(frequency);
    if (FAILED(impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fence))) ||
        !(impl->event = CreateEventW(nullptr, FALSE, FALSE, nullptr))) {
        error_ = "fence";
        return false;
    }
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * kSlots;
    if (FAILED(impl->device->CreateQueryHeap(&qd, IID_PPV_ARGS(&impl->queries)))) {
        error_ = "timestamp query heap";
        return false;
    }
    for (auto &slot : impl->slots) {
        if (FAILED(impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(impl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(&slot.list)))) {
            error_ = "command list";
            return false;
        }
        slot.list->Close();
        slot.readback = make_buffer(impl->device.Get(), 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                    D3D12_RESOURCE_FLAG_NONE);
        if (!slot.readback) {
            error_ = "readback buffer";
            return false;
        }
    }
    if (FAILED(impl->device->CreateRootSignature(0, k_cs_shadow_dxil, sizeof k_cs_shadow_dxil,
                                                 IID_PPV_ARGS(&impl->shadow_root)))) {
        error_ = "shadow root signature";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = impl->shadow_root.Get();
    cp.CS = {k_cs_shadow_dxil, sizeof k_cs_shadow_dxil};
    if (FAILED(impl->device->CreateComputePipelineState(&cp, IID_PPV_ARGS(&impl->shadow_pipe)))) {
        error_ = "shadow compute pipeline";
        return false;
    }
    cp.CS = {k_cs_shafts_dxil, sizeof k_cs_shafts_dxil};
    if (FAILED(impl->device->CreateComputePipelineState(&cp, IID_PPV_ARGS(&impl->shafts_pipe)))) {
        error_ = "light shafts compute pipeline";
        return false;
    }
    cp.CS = {k_cs_shadow_large_dxil, sizeof k_cs_shadow_large_dxil};
    if (SUCCEEDED(impl->device->CreateComputePipelineState(&cp, IID_PPV_ARGS(&impl->shadow_large_pipe)))) {
        cp.CS = {k_cs_shafts_large_dxil, sizeof k_cs_shafts_large_dxil};
        impl->device->CreateComputePipelineState(&cp, IID_PPV_ARGS(&impl->shafts_large_pipe));
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2 * kSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(impl->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&impl->descriptors)))) {
        error_ = "descriptor heap";
        return false;
    }
    impl->descriptor_size = impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    impl_ = std::move(impl);
    error_.clear();
    return true;
}

bool DxrScene::build(const std::vector<float> &xyz, const Shade *shade) {
    if (!impl_) return false;
    Impl &d = *impl_;
    const int index = d.next;
    Impl::Slot &slot = d.slots[index];
    d.next = (d.next + 1) % kSlots;
    if (d.fence->GetCompletedValue() < slot.done) { // normally already finished
        d.fence->SetEventOnCompletion(slot.done, d.event);
        WaitForSingleObject(d.event, INFINITE);
    }
    d.collect(slot);

    const UINT triangles = UINT(xyz.size() / 9);
    const bool texram_ready = shade && shade->tex0 && shade->tex1;
    const UINT cut = texram_ready && shade->cutout_xyz && shade->cutout_records
        ? UINT(std::min(shade->cutout_xyz->size() / 9, shade->cutout_records->size() / 8)) : 0;
    ++d.frames;
    d.triangles += triangles;
    d.cutouts += cut;
    if (!triangles && !cut) return !shade; // nothing to cast a shadow: the light is left unwritten

    // upload: the one instance (64 bytes), the opaque vertices, the cut-out
    // vertices, their texture records; each part 256-byte aligned
    auto align = [](UINT64 v) { return (v + 255) & ~UINT64(255); };
    const UINT64 vertex_offset = 256;
    const UINT64 cut_offset = align(vertex_offset + UINT64(triangles) * 9 * sizeof(float));
    const UINT64 record_offset = align(cut_offset + UINT64(cut) * 9 * sizeof(float));
    const UINT64 upload_size = record_offset + UINT64(cut) * 8 * sizeof(uint32_t);
    if (!slot.upload || slot.upload_cap < upload_size) {
        const UINT64 grown = std::max<UINT64>(upload_size, slot.upload_cap * 2);
        slot.upload = make_buffer(d.device.Get(), grown, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                  D3D12_RESOURCE_FLAG_NONE);
        slot.upload_cap = slot.upload ? grown : 0;
        if (!slot.upload) return ++d.failures, false;
    }

    // geometry 0 opaque; the cut-out one non-opaque, so the ray query hands
    // its triangles to the shader's texel test
    D3D12_RAYTRACING_GEOMETRY_DESC geometries[2]{};
    UINT geometry_count = 0;
    auto add_geometry = [&](UINT count, UINT64 offset, bool opaque) {
        if (!count) return;
        D3D12_RAYTRACING_GEOMETRY_DESC &g = geometries[geometry_count++];
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = count * 3;
        g.Triangles.VertexBuffer.StartAddress = slot.upload->GetGPUVirtualAddress() + offset;
        g.Triangles.VertexBuffer.StrideInBytes = 3 * sizeof(float);
    };
    add_geometry(triangles, vertex_offset, true);
    static const bool cut_opaque = [] { // diagnostic: M2_DXR_CUTOUT_OPAQUE=1 skips the texel test
        const char *v = std::getenv("M2_DXR_CUTOUT_OPAQUE");
        return v && v[0] == '1';
    }();
    add_geometry(cut, cut_offset, cut_opaque);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
    bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottom.NumDescs = geometry_count;
    bottom.pGeometryDescs = geometries;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom_info{};
    d.device->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &bottom_info);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.NumDescs = 1;
    top.InstanceDescs = slot.upload->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO top_info{};
    d.device->GetRaytracingAccelerationStructurePrebuildInfo(&top, &top_info);

    const auto as_state = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if (!ensure_uav(d.device.Get(), slot.blas, slot.blas_cap, bottom_info.ResultDataMaxSizeInBytes, as_state) ||
        !ensure_uav(d.device.Get(), slot.tlas, slot.tlas_cap, top_info.ResultDataMaxSizeInBytes, as_state) ||
        !ensure_uav(d.device.Get(), slot.scratch, slot.scratch_cap,
                    std::max(bottom_info.ScratchDataSizeInBytes, top_info.ScratchDataSizeInBytes),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        return ++d.failures, false;

    uint8_t *mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(slot.upload->Map(0, &none, reinterpret_cast<void **>(&mapped)))) return ++d.failures, false;
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
    instance.InstanceMask = 0xff;
    instance.AccelerationStructure = slot.blas->GetGPUVirtualAddress();
    std::memcpy(mapped, &instance, sizeof instance);
    std::memcpy(mapped + vertex_offset, xyz.data(), size_t(triangles) * 9 * sizeof(float));
    if (cut) {
        std::memcpy(mapped + cut_offset, shade->cutout_xyz->data(), size_t(cut) * 9 * sizeof(float));
        std::memcpy(mapped + record_offset, shade->cutout_records->data(), size_t(cut) * 8 * sizeof(uint32_t));
    }
    slot.upload->Unmap(0, nullptr);

    // texture RAM, when it changed since the last copy (the queue runs the
    // copies in order; each slot has its own staging buffer)
    const bool tex_copy = texram_ready && shade->tex_generation != d.texram_generation;
    if (texram_ready && !d.texram) {
        d.texram = make_buffer(d.device.Get(), Impl::kTexramBytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
        if (!d.texram) return ++d.failures, false;
    }
    if (tex_copy) {
        if (!slot.tex_upload) {
            slot.tex_upload = make_buffer(d.device.Get(), Impl::kTexramBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
            if (!slot.tex_upload) return ++d.failures, false;
        }
        uint8_t *tex = nullptr;
        if (FAILED(slot.tex_upload->Map(0, &none, reinterpret_cast<void **>(&tex)))) return ++d.failures, false;
        std::memcpy(tex, shade->tex0, Impl::kTexramBytes / 2);
        std::memcpy(tex + Impl::kTexramBytes / 2, shade->tex1, Impl::kTexramBytes / 2);
        slot.tex_upload->Unmap(0, nullptr);
        d.texram_generation = shade->tex_generation;
    }

    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
        return ++d.failures, false;
    auto *list = slot.list.Get();
    list->EndQuery(d.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(2 * index));
    if (tex_copy) { // COMMON promotes to COPY_DEST; the shader reads it after the barrier below
        list->CopyBufferRegion(d.texram.Get(), 0, slot.tex_upload.Get(), 0, Impl::kTexramBytes);
        D3D12_RESOURCE_BARRIER copied{};
        copied.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copied.Transition.pResource = d.texram.Get();
        copied.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copied.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        copied.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &copied);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = bottom;
    build.DestAccelerationStructureData = slot.blas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = slot.scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = slot.blas.Get();
    list->ResourceBarrier(1, &barrier);
    barrier.UAV.pResource = slot.scratch.Get(); // the top level reuses the scratch
    list->ResourceBarrier(1, &barrier);

    build.Inputs = top;
    build.DestAccelerationStructureData = slot.tlas->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    barrier.UAV.pResource = slot.tlas.Get();
    list->ResourceBarrier(1, &barrier);

    const bool shading = shade && shade->view_z && shade->light && shade->width > 0 && shade->height > 0;
    if (shading) {
        auto *view_z = static_cast<ID3D12Resource *>(shade->view_z);
        auto *light = static_cast<ID3D12Resource *>(shade->light);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = d.descriptors->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = d.descriptors->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(2 * index) * d.descriptor_size;
        gpu.ptr += UINT64(2 * index) * d.descriptor_size;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        d.device->CreateShaderResourceView(view_z, &srv, cpu);
        cpu.ptr += d.descriptor_size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8_UNORM; // light, light shafts
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.device->CreateUnorderedAccessView(light, nullptr, &uav, cpu);

        // SDL keeps a sampled texture in "all shader resource" between passes
        const auto rest = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER to{};
        to.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to.Transition.pResource = light;
        to.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to.Transition.StateBefore = rest;
        to.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &to);

        ID3D12DescriptorHeap *heaps[] = {d.descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(d.shadow_root.Get());
        const bool large_grid = shade->large_supersampled && d.shadow_large_pipe && d.shafts_large_pipe;
        auto *shadow_pipe = large_grid ? d.shadow_large_pipe.Get() : d.shadow_pipe.Get();
        list->SetPipelineState(shadow_pipe);
        struct {
            uint32_t size[2];
            float to_screen[2], origin[2], strength, bias, sun[3], sky, focus[2], sky_radius, sun_radius;
            float shaft, shaft_dist, origin2[2], focus2[2], shaft_phase;
        } params{{uint32_t(shade->width), uint32_t(shade->height)},
                 {shade->to_screen[0], shade->to_screen[1]},
                 {shade->origin[0], shade->origin[1]},
                 shade->strength,
                 shade->bias,
                 {shade->sun[0], shade->sun[1], shade->sun[2]},
                 shade->sky,
                 {shade->focus[0], shade->focus[1]},
                 shade->sky_radius,
                 shade->sun_radius,
                 shade->shaft,
                 shade->shaft_dist,
                 {shade->origin2[0], shade->origin2[1]},
                 {shade->focus2[0], shade->focus2[1]},
                 shade->shaft_phase};
        static_assert(sizeof params == 23 * 4, "dxr_shadow.hlsl Params");
        list->SetComputeRoot32BitConstants(0, 23, &params, 0);
        list->SetComputeRootShaderResourceView(1, slot.tlas->GetGPUVirtualAddress());
        list->SetComputeRootDescriptorTable(2, gpu);
        // texture RAM and cut-out records (any valid address when there are none: never read)
        const D3D12_GPU_VIRTUAL_ADDRESS records = slot.upload->GetGPUVirtualAddress() + record_offset;
        list->SetComputeRootShaderResourceView(3, d.texram ? d.texram->GetGPUVirtualAddress() : records);
        list->SetComputeRootShaderResourceView(4, records);
        // The shaft and shadow pipelines have matching sample grids.
        const UINT shaft_stride = large_grid ? 12 : 8;
        const UINT lo_w = (UINT(shade->width) + shaft_stride - 1) / shaft_stride;
        const UINT lo_h = (UINT(shade->height) + shaft_stride - 1) / shaft_stride;
        const bool shafts = shade->shaft > 0.0f &&
                            ensure_uav(d.device.Get(), d.haze, d.haze_cap, UINT64(lo_w) * lo_h * 4,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!shafts) params.shaft = 0.0f, list->SetComputeRoot32BitConstants(0, 23, &params, 0);
        // any valid address when there are no shafts: never read then
        list->SetComputeRootUnorderedAccessView(5, d.haze ? d.haze->GetGPUVirtualAddress() : slot.scratch->GetGPUVirtualAddress());
        if (shafts) {
            list->SetPipelineState(large_grid ? d.shafts_large_pipe.Get() : d.shafts_pipe.Get());
            list->Dispatch((lo_w + 7) / 8, (lo_h + 7) / 8, 1); // cs_shafts: 8x8 groups
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = d.haze.Get();
            list->ResourceBarrier(1, &uav);
            list->SetPipelineState(shadow_pipe);
        }
        const UINT shadow_span = large_grid ? 32 : 16; // 16 threads, one or two pixels each
        list->Dispatch((UINT(shade->width) + shadow_span - 1) / shadow_span,
                       (UINT(shade->height) + shadow_span - 1) / shadow_span, 1);

        std::swap(to.Transition.StateBefore, to.Transition.StateAfter);
        list->ResourceBarrier(1, &to);
        ++d.shaded;
    }

    list->EndQuery(d.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(2 * index + 1));
    list->ResolveQueryData(d.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, UINT(2 * index), 2, slot.readback.Get(), 0);
    if (FAILED(list->Close())) return ++d.failures, false;

    ID3D12CommandList *lists[] = {list};
    d.queue->ExecuteCommandLists(1, lists);
    slot.done = ++d.fence_value;
    d.queue->Signal(d.fence.Get(), slot.done);
    slot.timed = true;
    return shade ? shading : true;
}

void DxrScene::shutdown() {
    report();
    impl_.reset();
}

void DxrScene::report() const {
    if (!impl_) return;
    const Impl &d = *impl_;
    if (!d.frames) return;
    // The slow tail matters at 120 frames/s (8.3 ms a frame): the 99th
    // percentile and the frames over 4 ms.
    double p99 = 0;
    size_t over4 = 0;
    if (!d.gpu_ms_all.empty()) {
        std::vector<float> sorted = d.gpu_ms_all;
        const size_t k = std::min(sorted.size() - 1, sorted.size() * 99 / 100);
        std::nth_element(sorted.begin(), sorted.begin() + ptrdiff_t(k), sorted.end());
        p99 = sorted[k];
        for (const float ms : d.gpu_ms_all) over4 += ms > 4.0f;
    }
    char line[320];
    std::snprintf(line, sizeof line,
                  "daytona: DXR scene: %llu frames (%llu shaded), %.0f triangles + %.0f cut-out per frame, GPU %.3f ms mean,"
                  " %.3f ms p99, %.3f ms max, %zu over 4 ms (%llu timed), %llu failures\n",
                  (unsigned long long)d.frames, (unsigned long long)d.shaded, double(d.triangles) / double(d.frames),
                  double(d.cutouts) / double(d.frames),
                  d.timed_frames ? d.gpu_ms_sum / double(d.timed_frames) : 0.0, p99, d.gpu_ms_max, over4,
                  (unsigned long long)d.timed_frames, (unsigned long long)d.failures);
    std::fputs(line, stdout);
    std::fflush(stdout);
    // M2_DXR_SCENE_REPORT=FILE: the same line appended to FILE (scripted runs
    // have no console).
    if (const char *path = std::getenv("M2_DXR_SCENE_REPORT"); path && *path)
        if (std::FILE *f = std::fopen(path, "a")) {
            std::fputs(line, f);
            std::fclose(f);
        }
}

#else // no Direct3D 12: the rendering stays as it is

struct DxrScene::Impl {};
DxrScene::DxrScene() = default;
DxrScene::~DxrScene() = default;
bool DxrScene::ok() const { return false; }
bool DxrScene::init(SDL_GPUDevice *) {
    error_ = "Direct3D 12 only";
    return false;
}
bool DxrScene::build(const std::vector<float> &, const Shade *) { return false; }
void DxrScene::report() const {}
void DxrScene::shutdown() {}

#endif

} // namespace app
