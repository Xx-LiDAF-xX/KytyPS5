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
	    .pipeline            = reinterpret_cast<VkPipeline>(0x1000),
	    .index_address       = 0x20000,
	    .index_type_and_size = 2,
	    .topology            = 4,
	};

	CHECK(batcher.CanBatch(key1) == false); // Initially empty, requires BeginBatch

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
	MdiBatchKey key3 = key1;
	key3.index_address = 0x30000;
	CHECK(batcher.CanBatch(key3) == false);

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
	    .pipeline      = reinterpret_cast<VkPipeline>(0x1000),
	    .index_address = 0,
	    .topology      = 4,
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
} // namespace

int main() {
	TestBatchAccumulationAndFlushing();
	TestDirectNonIndexedBatching();
	std::printf("MdiBatcher tests passed\n");
	return 0;
}
