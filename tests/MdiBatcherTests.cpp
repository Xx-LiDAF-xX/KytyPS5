#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/mdiBatcher.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace Libs::Graphics;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "MdiBatcherTests:%d: %s\n", __LINE__, #condition);                \
			std::abort();                                                                          \
		}                                                                                          \
	} while (false)

void TestBatchAccumulationAndFlushing() {
	MdiBatcher batcher;
	CHECK(batcher.IsEmpty());
	CHECK(batcher.Count() == 0);

	MdiBatchKey key1 {
	    .pipeline           = reinterpret_cast<VkPipeline>(0x1000),
	    .index_buffer       = reinterpret_cast<VkBuffer>(0x2000),
	    .index_offset       = 0,
	    .index_type         = VK_INDEX_TYPE_UINT16,
	    .topology           = 4,
	    .object_id          = 1,
	    .vertex_buffer_hash = {0x1234ULL, 0x5678ULL},
	    .bindings_hash      = {0x9abcULL, 0xdef0ULL},
	};

	CHECK(batcher.CanBatch(key1) == false);

	batcher.BeginBatch(key1);
	CHECK(batcher.CanBatch(key1) == true);

	vk::DrawIndexedIndirectCommand cmd1 {
	    .indexCount    = 300,
	    .instanceCount = 1,
	    .firstIndex    = 0,
	    .vertexOffset  = 0,
	    .firstInstance = 0,
	};
	batcher.AppendIndexed(cmd1);
	CHECK(batcher.Count() == 1);
	CHECK(!batcher.IsEmpty());

	vk::DrawIndexedIndirectCommand cmd2 {
	    .indexCount    = 150,
	    .instanceCount = 1,
	    .firstIndex    = 300,
	    .vertexOffset  = 100,
	    .firstInstance = 0,
	};
	batcher.AppendIndexed(cmd2);
	CHECK(batcher.Count() == 2);

	// Incompatible key (different pipeline)
	MdiBatchKey key2 = key1;
	key2.pipeline    = reinterpret_cast<VkPipeline>(0x2000);
	CHECK(batcher.CanBatch(key2) == false);

	// Incompatible key (different index buffer)
	MdiBatchKey key3  = key1;
	key3.index_buffer = reinterpret_cast<VkBuffer>(0x3000);
	CHECK(batcher.CanBatch(key3) == false);

	// Incompatible key (different bindings lo or hi lane)
	MdiBatchKey key4      = key1;
	key4.bindings_hash.lo = 0x9999ULL;
	CHECK(batcher.CanBatch(key4) == false);

	MdiBatchKey key5      = key1;
	key5.bindings_hash.hi ^= 1ULL;
	CHECK(batcher.CanBatch(key5) == false);

	const auto commands = batcher.GetIndexedCommands();
	CHECK(commands.size() == 2);
	CHECK(commands[0].indexCount == 300);
	CHECK(commands[1].indexCount == 150);

	batcher.Reset();
	CHECK(batcher.IsEmpty());
	CHECK(batcher.Count() == 0);
}

void TestDirectNonIndexedBatching() {
	MdiBatcher batcher;

	MdiBatchKey key {
	    .pipeline           = reinterpret_cast<VkPipeline>(0x1000),
	    .index_buffer       = nullptr,
	    .topology           = 4,
	    .object_id          = 2,
	    .vertex_buffer_hash = {0xaaaaULL, 0x1111ULL},
	    .bindings_hash      = {0xbbbbULL, 0x2222ULL},
	};

	batcher.BeginBatch(key, /*indexed=*/false);
	vk::DrawIndirectCommand cmd1 {.vertexCount = 12, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0};
	vk::DrawIndirectCommand cmd2 {.vertexCount = 24, .instanceCount = 1, .firstVertex = 12, .firstInstance = 0};

	batcher.AppendDirect(cmd1);
	batcher.AppendDirect(cmd2);

	CHECK(batcher.Count() == 2);
	CHECK(batcher.IsIndexed() == false);

	const auto commands = batcher.GetDirectCommands();
	CHECK(commands.size() == 2);
	CHECK(commands[0].vertexCount == 12);
	CHECK(commands[1].vertexCount == 24);
}

void TestHardwareVariableBatchCapAndTakeBatch() {
	MdiBatcher batcher;
	MdiBatchKey key {
	    .pipeline           = reinterpret_cast<VkPipeline>(0x4000),
	    .index_buffer       = reinterpret_cast<VkBuffer>(0x5000),
	    .index_offset       = 0,
	    .index_type         = VK_INDEX_TYPE_UINT32,
	    .topology           = 4,
	    .object_id          = 0,
	    .vertex_buffer_hash = {0x1111ULL, 0x3333ULL},
	    .bindings_hash      = {0x2222ULL, 0x4444ULL},
	};

	batcher.BeginBatch(key, /*indexed=*/true);
	for (uint32_t i = 0; i < 4; ++i) {
		CHECK(batcher.CanBatch(key, /*max_commands=*/4));
		batcher.AppendIndexed({
		    .indexCount    = 36,
		    .instanceCount = 1,
		    .firstIndex    = i * 36,
		    .vertexOffset  = 0,
		    .firstInstance = 0,
		});
	}
	CHECK(!batcher.CanBatch(key, /*max_commands=*/4));
	CHECK(batcher.CanBatch(key, /*max_commands=*/8));

	std::vector<vk::DrawIndexedIndirectCommand> out_indexed;
	std::vector<vk::DrawIndirectCommand>        out_direct;
	bool                                        is_indexed = false;
	CHECK(batcher.TakeBatch(out_indexed, out_direct, is_indexed));
	CHECK(is_indexed);
	CHECK(batcher.IsEmpty());
	CHECK(!batcher.IsActive());
	CHECK(out_indexed.size() == 4);
	CHECK(out_indexed[3].firstIndex == 108);
	CHECK(!batcher.TakeBatch(out_indexed, out_direct, is_indexed));
}

void TestDynamicStateHasPendingCommit() {
	GraphicContext gfx {};
	DynamicState   state {};
	CHECK(!state.HasPendingCommit(gfx));

	// Invalidate() memset-fills dirty_state with 0xFF (including padding bits and gated bits).
	state.Invalidate();
	CHECK(state.HasPendingCommit(gfx));

	// Simulate Commit() clearing all unconditional top-level bits while gated features are false
	// and padding bits remain 1s from Invalidate().
	state.dirty_state.viewports                 = false;
	state.dirty_state.scissors                  = false;
	state.dirty_state.depth_test_enabled        = false;
	state.dirty_state.depth_write_enabled       = false;
	state.dirty_state.depth_bounds_test_enabled = false;
	state.dirty_state.depth_bias_enabled        = false;
	state.dirty_state.stencil_test_enabled      = false;
	state.dirty_state.cull_mode                 = false;
	state.dirty_state.front_face                = false;
	state.dirty_state.blend_constants           = false;
	state.dirty_state.line_width                = false;

	// Gated bits (depth_compare_op, depth_bounds, depth_bias, stencil_*, feedback_loop_aspects)
	// and unused bitfield padding bits are still 1, yet Commit() would emit zero commands!
	CHECK(!state.HasPendingCommit(gfx));

	// Enabling feedback_loop_enabled on GraphicContext activates feedback_loop_aspects commit.
	gfx.attachment_feedback_loop_enabled = true;
	CHECK(state.HasPendingCommit(gfx));
	state.dirty_state.feedback_loop_aspects = false;
	CHECK(!state.HasPendingCommit(gfx));

	// Enabling depth_test_enabled wakes the gated depth_compare_op dirty bit.
	state.SetDepthTestEnabled(true);
	CHECK(state.HasPendingCommit(gfx));
	state.dirty_state.depth_test_enabled = false;
	CHECK(state.HasPendingCommit(gfx));
	state.dirty_state.depth_compare_op = false;
	CHECK(!state.HasPendingCommit(gfx));
}

void TestPerCommandBufferStateIsolationAndHelperInvalidation() {
	// Reproduces Critical #1 & #2: two independent command buffers (render vs present_scheduler)
	// must maintain isolated BoundGraphicsState and MdiBatcher instances, and an internal helper
	// graphics pass (like BlitHelper::ReinterpretColorAsMsDepth) must invalidate BoundGraphicsState
	// so the next guest draw rebinds its pipeline.
	BoundGraphicsState render_state {};
	BoundGraphicsState present_state {};
	MdiBatcher         render_batcher {};
	MdiBatcher         present_batcher {};

	const vk::Pipeline guest_pipe {reinterpret_cast<VkPipeline>(0x7001)};
	const vk::Buffer   guest_ib {reinterpret_cast<VkBuffer>(0x8001)};
	CHECK(render_state.ShouldBindPipeline(guest_pipe));
	CHECK(!render_state.ShouldBindPipeline(guest_pipe));
	CHECK(render_state.ShouldBindIndexBuffer(guest_ib, 0, vk::IndexType::eUint16));
	CHECK(!render_state.ShouldBindIndexBuffer(guest_ib, 0, vk::IndexType::eUint16));

	MdiBatchKey key {.pipeline = static_cast<VkPipeline>(guest_pipe)};
	render_batcher.BeginBatch(key, true);
	render_batcher.AppendIndexed({.indexCount = 12, .instanceCount = 1});

	// Resetting present_scheduler's state & checking its empty batcher leaves render untouched.
	present_state.Reset();
	CHECK(present_batcher.IsEmpty());
	CHECK(!render_batcher.IsEmpty());
	CHECK(render_batcher.Count() == 1);
	CHECK(!render_state.ShouldBindPipeline(guest_pipe));

	// Internal helper pass (e.g. BlitHelper::ReinterpretColorAsMsDepth) invalidates render_state:
	// the very next guest draw with the same guest_pipe MUST rebind it.
	render_state.Reset();
	CHECK(render_state.ShouldBindPipeline(guest_pipe));
	CHECK(render_state.ShouldBindIndexBuffer(guest_ib, 0, vk::IndexType::eUint16));
}

void TestStreamSliceCacheWraparoundInvalidation() {
	// Reproduces Critical #3: if FlattenedSrt hits cache on tick T (e.g. offset 0) and uploading
	// ShaderData wraps StreamBuffer (advancing tick T -> T + 1 and overwriting offset 0),
	// ResolveStageStreamUploads must re-upload FlattenedSrt on tick T + 1 as well.
	StreamSliceCache          srt_cache {};
	StreamSliceCache          data_cache {};
	const vk::Buffer          stream_buf {reinterpret_cast<VkBuffer>(0x9000)};
	uint64_t                  tick         = 10;
	uint64_t                  next_offset  = 0;
	uint32_t                  upload_calls = 0;
	bool                      wrap_on_next = false;
	const std::array<uint32_t, 2> srt_words {0x11u, 0x22u};
	const std::array<uint32_t, 2> data_v1 {0x33u, 0x44u};
	const std::array<uint32_t, 2> data_v2 {0x55u, 0x66u};

	auto current_tick = [&]() noexcept { return tick; };
	auto upload       = [&](std::span<const uint32_t> words) -> vk::DescriptorBufferInfo {
		++upload_calls;
		if (wrap_on_next) {
			wrap_on_next = false;
			++tick;
			next_offset = 0;
		}
		const uint64_t off = next_offset;
		next_offset += 256;
		return {stream_buf, off, words.size_bytes()};
	};

	vk::DescriptorBufferInfo srt_out {};
	vk::DescriptorBufferInfo data_out {};
	ResolveStageStreamUploads(stream_buf, true, srt_words, srt_cache, srt_out, true, data_v1,
	                          data_cache, data_out, current_tick, upload);
	CHECK(upload_calls == 2);
	CHECK(srt_out.offset == 0);
	CHECK(data_out.offset == 256);

	// Identical payloads on same tick reuse both slices (0 uploads).
	ResolveStageStreamUploads(stream_buf, true, srt_words, srt_cache, srt_out, true, data_v1,
	                          data_cache, data_out, current_tick, upload);
	CHECK(upload_calls == 2);

	// Now ShaderData changes to data_v2 AND wraps StreamBuffer (advancing tick 10 -> 11 at off=0).
	// FlattenedSrt MUST NOT keep its retired tick-10 slice at off=0 (which data_v2 just overwrote)!
	wrap_on_next = true;
	ResolveStageStreamUploads(stream_buf, true, srt_words, srt_cache, srt_out, true, data_v2,
	                          data_cache, data_out, current_tick, upload);
	CHECK(tick == 11);
	CHECK(upload_calls == 4);
	CHECK(data_out.offset == 0);
	CHECK(srt_out.offset == 256);
	CHECK(srt_cache.tick == 11);
	CHECK(data_cache.tick == 11);
}

void TestReleaseMemInterruptFlushPolicy() {
	// Explicit interrupt sync selectors (1 = kOnly, 4 = kSendIntCtx) flush immediately when a
	// listener exists, whereas selector 2 (kAfterWriteConfirm) stays batched across PM4 slices
	// until MaxDrawsInBatch(), queue drain (m_submission_count == 0), block, or queue switch.
	CHECK(!ShouldFlushReleaseMemInterrupt(/*has_listener=*/false, /*selector=*/1));
	CHECK(!ShouldFlushReleaseMemInterrupt(/*has_listener=*/false, /*selector=*/2));
	CHECK(ShouldFlushReleaseMemInterrupt(/*has_listener=*/true, /*selector=*/1));
	CHECK(!ShouldFlushReleaseMemInterrupt(/*has_listener=*/true, /*selector=*/2));
	CHECK(ShouldFlushReleaseMemInterrupt(/*has_listener=*/true, /*selector=*/4));
}

void TestDescriptorSet0AndPushConstantDeduplication() {
	BoundGraphicsState state {};
	const auto         layout1 = reinterpret_cast<VkPipelineLayout>(0xa001);
	const auto         layout2 = reinterpret_cast<VkPipelineLayout>(0xa002);
	const MdiHash128   hash1 {0x1111222233334444ULL, 0x5555666677778888ULL};
	const MdiHash128   hash2 {0x9999aaaabbbbccccULL, 0xddddeeeeffff0000ULL};

	CHECK(state.ShouldBindDescriptorSet0(layout1, hash1));
	CHECK(!state.ShouldBindDescriptorSet0(layout1, hash1));
	CHECK(state.ShouldBindDescriptorSet0(layout2, hash1));
	CHECK(!state.ShouldBindDescriptorSet0(layout2, hash1));
	CHECK(state.ShouldBindDescriptorSet0(layout2, hash2));
	state.InvalidateDescriptorSet0();
	CHECK(state.ShouldBindDescriptorSet0(layout2, hash2));

	const std::array<uint32_t, 4> push_a {1u, 2u, 3u, 4u};
	const std::array<uint32_t, 6> push_a_ext {1u, 2u, 3u, 4u, 5u, 6u};
	const std::array<uint32_t, 4> push_b {1u, 2u, 9u, 4u};

	CHECK(!state.ShouldPushConstants(layout1, 0x21u, {}));
	CHECK(state.ShouldPushConstants(layout1, 0x21u, push_a));
	CHECK(!state.ShouldPushConstants(layout1, 0x21u, push_a));
	// Growing push constant range with same prefix must still push the newly needed tail dwords.
	CHECK(state.ShouldPushConstants(layout1, 0x21u, push_a_ext));
	// Shrinking to an identical prefix with same layout & stages can skip pushing.
	CHECK(!state.ShouldPushConstants(layout1, 0x21u, push_a));
	// Different pipeline layout or stage flags must re-push.
	CHECK(state.ShouldPushConstants(layout2, 0x21u, push_a));
	CHECK(state.ShouldPushConstants(layout2, 0x01u, push_a));
	// Different payload must re-push.
	CHECK(state.ShouldPushConstants(layout2, 0x01u, push_b));
	// Compute/EndRendering helper invalidation forces next graphics draw to re-push.
	state.InvalidatePushConstants();
	CHECK(state.ShouldPushConstants(layout2, 0x01u, push_b));
}

void TestActiveVertexBufferHashAndMdiMatchesScalars() {
	BoundGraphicsState state {};
	CHECK(state.MatchesActiveVertexBuffers(0, {}, {}, {}));
	CHECK(state.vertex_buffer_hash == ComputeVertexBufferHash(0, {}, {}, {}));

	const std::array<vk::Buffer, 2> bufs {
	    vk::Buffer {reinterpret_cast<VkBuffer>(0xb001)},
	    vk::Buffer {reinterpret_cast<VkBuffer>(0xb002)},
	};
	const std::array<vk::DeviceSize, 2> offs {0, 64};
	const std::array<vk::DeviceSize, 2> szs {128, 256};

	CHECK(state.ShouldBindVertexBuffers(2, bufs, offs, szs));
	CHECK(state.MatchesActiveVertexBuffers(2, bufs, offs, szs));
	CHECK(state.vertex_buffer_hash == ComputeVertexBufferHash(2, bufs, offs, szs));

	// Shrinking active count from 2 -> 1 with identical slot 0 skips vkCmdBindVertexBuffers2
	// while updating active_vertex_count and vertex_buffer_hash for 1-slot shaders!
	CHECK(!state.ShouldBindVertexBuffers(1, std::span {bufs}.first(1), std::span {offs}.first(1),
	                                     std::span {szs}.first(1)));
	CHECK(state.MatchesActiveVertexBuffers(1, std::span {bufs}.first(1), std::span {offs}.first(1),
	                                       std::span {szs}.first(1)));
	CHECK(!state.MatchesActiveVertexBuffers(2, bufs, offs, szs));
	CHECK(state.vertex_buffer_hash ==
	      ComputeVertexBufferHash(1, std::span {bufs}.first(1), std::span {offs}.first(1),
	                              std::span {szs}.first(1)));

	MdiBatcher        batcher {};
	const VkPipeline  pipe = reinterpret_cast<VkPipeline>(0xc001);
	const VkBuffer    ib   = reinterpret_cast<VkBuffer>(0xc002);
	const MdiBatchKey key {
	    .pipeline           = pipe,
	    .index_buffer       = ib,
	    .index_offset       = 0,
	    .index_type         = VK_INDEX_TYPE_UINT16,
	    .topology           = 4,
	    .object_id          = 7,
	    .vertex_buffer_hash = state.vertex_buffer_hash,
	    .bindings_hash      = {0x123ULL, 0x456ULL},
	};
	CHECK(!batcher.MatchesScalars(pipe, ib, 0, VK_INDEX_TYPE_UINT16, 4, 7, true, 16));
	batcher.BeginBatch(key, true);
	batcher.AppendIndexed({.indexCount = 18, .instanceCount = 1});
	CHECK(batcher.MatchesScalars(pipe, ib, 0, VK_INDEX_TYPE_UINT16, 4, 7, true, 16));
	CHECK(!batcher.MatchesScalars(pipe, ib, 0, VK_INDEX_TYPE_UINT16, 4, 8, true, 16));
	CHECK(!batcher.MatchesScalars(pipe, ib, 0, VK_INDEX_TYPE_UINT16, 4, 7, false, 16));
	CHECK(!batcher.MatchesScalars(pipe, ib, 0, VK_INDEX_TYPE_UINT16, 4, 7, true, 1));
}
} // namespace

int main() {
	TestBatchAccumulationAndFlushing();
	TestDirectNonIndexedBatching();
	TestHardwareVariableBatchCapAndTakeBatch();
	TestDynamicStateHasPendingCommit();
	TestPerCommandBufferStateIsolationAndHelperInvalidation();
	TestStreamSliceCacheWraparoundInvalidation();
	TestReleaseMemInterruptFlushPolicy();
	TestDescriptorSet0AndPushConstantDeduplication();
	TestActiveVertexBufferHashAndMdiMatchesScalars();
	std::printf("MdiBatcher tests passed\n");
	return 0;
}
