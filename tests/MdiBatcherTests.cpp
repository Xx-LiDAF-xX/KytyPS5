#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/dynamicState.h"
#include "graphics/host_gpu/renderer/mdiBatcher.h"

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
	    .vertex_buffer_hash = 0x1234,
	    .bindings_hash      = 0x5678,
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

	// Incompatible key (different bindings)
	MdiBatchKey key4   = key1;
	key4.bindings_hash = 0x9999;
	CHECK(batcher.CanBatch(key4) == false);

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
	    .vertex_buffer_hash = 0xaaaa,
	    .bindings_hash      = 0xbbbb,
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
	    .vertex_buffer_hash = 0x1111,
	    .bindings_hash      = 0x2222,
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
} // namespace

int main() {
	TestBatchAccumulationAndFlushing();
	TestDirectNonIndexedBatching();
	TestHardwareVariableBatchCapAndTakeBatch();
	TestDynamicStateHasPendingCommit();
	std::printf("MdiBatcher tests passed\n");
	return 0;
}
