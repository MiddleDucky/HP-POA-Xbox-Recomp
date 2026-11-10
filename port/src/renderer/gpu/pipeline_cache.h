#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

// Shaders and pipelines kept on disk between runs, and the threads that build new ones away from the render thread.
// Every mission is a new process and the title's shaders are made from draw time state, so what this PC has built
// before is all there is to keep.
namespace gpu::cache {

// shadercache\ beside the exe; root: a hash of the root signature every pipeline is made with
void Open(ID3D12Device* dev, uint64_t root);

// compiled bytecode by source hash; check, a second hash of the source, turns a key collision into a miss. base:
// set when it came from the shipped base_shaders.bin (built elsewhere, so this driver may still refuse it)
Microsoft::WRL::ComPtr<ID3DBlob> FindShader(uint64_t key, uint64_t check, bool* base = nullptr);
void AddShader(uint64_t key, uint64_t check, ID3DBlob* code);

// the driver's compiled pipeline by a hash of everything its description holds; null when the library lacks it
Microsoft::WRL::ComPtr<ID3D12PipelineState> LoadPipeline(uint64_t name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d);
void StorePipeline(uint64_t name, ID3D12PipelineState* pso);
// each frame: the library is written a few seconds after the last new pipeline
void SaveWhenIdle();

// runs job on a build thread
void Post(std::function<void()> job);
// xboxrecomp: runs job on a build thread once no Post job waits (pipelines made ahead of need)
void PostLow(std::function<void()> job);

// xboxrecomp: pipelines_list.bin, every pipeline the title has made, described without anything of this PC's GPU (its
// key and its shaders' source checks), so another PC can make them all before they are needed. base_pipelines.bin
// beside it is the list a release ships, read only. record: the caller's fixed-size description
void OpenPipelineList(size_t record);
void RecordPipeline(uint64_t id, const void* record);
// the descriptions of both files, each once
std::vector<std::vector<uint8_t>> RecordedPipelines();
size_t RecordedPipelineCount();

}  // namespace gpu::cache
