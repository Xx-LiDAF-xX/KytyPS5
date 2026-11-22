#include "graphics/shader/shaderBindings.h"
#include "graphics/shader/shaderVertexMetadata.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderBindings.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace {

using namespace Libs::Graphics;

// A draw zero-initializes its vertex inputs in one memset only while these types are trivial.
static_assert(std::is_trivially_default_constructible_v<ShaderVertexDestination> &&
              std::is_trivially_default_constructible_v<ShaderVertexInputBuffer>);

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "ShaderVertexMetadataTests: failed: %s\n", text);
		std::abort();
	}
}

struct Fixture {
	std::array<uint16_t, static_cast<size_t>(DirectResourceType::Last) + 1>    offsets {};
	ShaderUserData                                                             user_data {};
	ShaderSemantic                                                             semantic {};
	ShaderMappedData                                                           mapped {};

	Fixture() {
		offsets.fill(ILLEGAL_DIRECT_OFFSET);
		offsets[static_cast<size_t>(DirectResourceType::PtrVertexBufferTable)]     = 2;
		offsets[static_cast<size_t>(DirectResourceType::PtrVertexAttribDescTable)] = 4;
		user_data.direct_resource_offset = offsets.data();
		user_data.direct_resource_count  = static_cast<uint16_t>(offsets.size());
		mapped.user_data                 = &user_data;
		mapped.input_semantics           = &semantic;
		mapped.num_input_semantics       = 1;
	}
};

void CheckRejected(const ShaderMappedData& data, const char* text) {
	ShaderVertexMetadata output;
	std::array<ShaderSemantic, 7> prior_semantics {};
	output.vertex_buffer_reg = 37;
	output.vertex_attrib_reg = 41;
	output.input_semantics   = prior_semantics;
	Check(ShaderReadVertexMetadata(data, 64, output) != nullptr, text);
	Check(output.vertex_buffer_reg == 37 && output.vertex_attrib_reg == 41 &&
	          output.input_semantics.data() == prior_semantics.data() &&
	          output.input_semantics.size() == prior_semantics.size(),
	      "metadata rejection changed the prior output");
}

void TestValidAndInvalidMetadata() {
	Fixture              fixture;
	ShaderVertexMetadata output;
	Check(ShaderReadVertexMetadata(fixture.mapped, 64, output) == nullptr,
	      "valid vertex metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == 4 &&
	          output.input_semantics.size() == 1,
	      "valid vertex metadata was decoded incorrectly");

	Fixture buffer_only;
	buffer_only.offsets[static_cast<size_t>(DirectResourceType::PtrVertexAttribDescTable)] =
	    ILLEGAL_DIRECT_OFFSET;
	buffer_only.mapped.input_semantics     = nullptr;
	buffer_only.mapped.num_input_semantics = 0;
	Check(ShaderReadVertexMetadata(buffer_only.mapped, 64, output) == nullptr,
	      "vertex-buffer-only metadata was rejected");
	Check(output.vertex_buffer_reg == 2 && output.vertex_attrib_reg == -1 &&
	          output.input_semantics.empty(),
	      "vertex-buffer-only metadata was decoded incorrectly");

	Fixture attrib_only;
	attrib_only.offsets[static_cast<size_t>(DirectResourceType::PtrVertexBufferTable)] =
	    ILLEGAL_DIRECT_OFFSET;
	CheckRejected(attrib_only.mapped, "vertex-attribute-only metadata was accepted");

	auto missing_header      = fixture.mapped;
	missing_header.user_data = nullptr;
	CheckRejected(missing_header, "missing user-data header was accepted");

	Fixture missing_offsets;
	missing_offsets.user_data.direct_resource_offset = nullptr;
	CheckRejected(missing_offsets.mapped, "missing direct-resource offsets were accepted");

	Fixture excessive_resources;
	excessive_resources.user_data.direct_resource_count =
	    static_cast<uint16_t>(excessive_resources.offsets.size() + 1);
	CheckRejected(excessive_resources.mapped, "excessive direct-resource count was accepted");

	Fixture excessive_semantics;
	excessive_semantics.mapped.num_input_semantics = ShaderVertexInputInfo::RES_MAX + 1;
	CheckRejected(excessive_semantics.mapped, "excessive vertex semantic count was accepted");

	Fixture excessive_register;
	excessive_register.offsets[static_cast<size_t>(DirectResourceType::PtrVertexBufferTable)] = 63;
	CheckRejected(excessive_register.mapped, "out-of-domain vertex table SGPR was accepted");

	Fixture excessive_attrib_register;
	excessive_attrib_register
	    .offsets[static_cast<size_t>(DirectResourceType::PtrVertexAttribDescTable)] = 63;
	CheckRejected(excessive_attrib_register.mapped,
	              "out-of-domain vertex attribute table SGPR was accepted");

	Fixture missing_semantics;
	missing_semantics.mapped.input_semantics = nullptr;
	CheckRejected(missing_semantics.mapped, "missing vertex semantic array was accepted");
}

void TestGfx10BufferFormatDecoding() {
	using namespace Libs::Graphics::ShaderRecompiler::Format;

	Check(DecodeGfx10BufferFormat(1, 0) == Prospero::BufferFormat::k8UNorm,
	      "dfmt 1, nfmt 0 should decode to k8UNorm");
	Check(DecodeGfx10BufferFormat(1, 1) == Prospero::BufferFormat::k8SNorm,
	      "dfmt 1, nfmt 1 should decode to k8SNorm");
	Check(DecodeGfx10BufferFormat(2, 0) == Prospero::BufferFormat::k16UNorm,
	      "dfmt 2, nfmt 0 should decode to k16UNorm");
	Check(DecodeGfx10BufferFormat(2, 1) == Prospero::BufferFormat::k16SNorm,
	      "dfmt 2, nfmt 1 should decode to k16SNorm");
	Check(DecodeGfx10BufferFormat(2, 7) == Prospero::BufferFormat::k16Float,
	      "dfmt 2, nfmt 7 should decode to k16Float");
	Check(DecodeGfx10BufferFormat(5, 0) == Prospero::BufferFormat::k16_16UNorm,
	      "dfmt 5, nfmt 0 should decode to k16_16UNorm");
	Check(DecodeGfx10BufferFormat(5, 7) == Prospero::BufferFormat::k16_16Float,
	      "dfmt 5, nfmt 7 should decode to k16_16Float");
	Check(DecodeGfx10BufferFormat(10, 0) == Prospero::BufferFormat::k8_8_8_8UNorm,
	      "dfmt 10, nfmt 0 should decode to k8_8_8_8UNorm");
	Check(DecodeGfx10BufferFormat(12, 0) == Prospero::BufferFormat::k16_16_16_16UNorm,
	      "dfmt 12, nfmt 0 should decode to k16_16_16_16UNorm");
	Check(DecodeGfx10BufferFormat(14, 7) == Prospero::BufferFormat::k32_32_32_32Float,
	      "dfmt 14, nfmt 7 should decode to k32_32_32_32Float");

	// HardwareFormat on ShaderBufferResource
	ShaderBufferResource res {};
	// dfmt=2 (16-bit), nfmt=0 (UNORM): (0 << 16) | (2 << 12)
	res.fields[3] = (0u << 16u) | (2u << 12u);
	Check(res.HardwareFormat() == Prospero::BufferFormat::k16UNorm,
	      "res with dfmt 2, nfmt 0 should yield HardwareFormat k16UNorm");

	// dfmt=5 (16_16), nfmt=0 (UNORM): (0 << 16) | (5 << 12)
	res.fields[3] = (0u << 16u) | (5u << 12u);
	Check(res.HardwareFormat() == Prospero::BufferFormat::k16_16UNorm,
	      "res with dfmt 5, nfmt 0 should yield HardwareFormat k16_16UNorm");

	// dfmt=12 (16_16_16_16), nfmt=0 (UNORM): (0 << 16) | (12 << 12)
	res.fields[3] = (0u << 16u) | (12u << 12u);
	Check(res.HardwareFormat() == Prospero::BufferFormat::k16_16_16_16UNorm,
	      "res with dfmt 12, nfmt 0 should yield HardwareFormat k16_16_16_16UNorm");

	// Verify destination swizzle mapping for decoded hardware formats:
	// Missing channels must default to 0.0 (X001, XY01, XYZ1) and 1.0 for W.
	const auto swizzle_for_format = [](Prospero::BufferFormat fmt) {
		const auto info     = GetFormatInfo(fmt);
		const auto channels = info.component_count;
		return Libs::Graphics::DstSel(4, channels > 1u ? 5u : 0u, channels > 2u ? 6u : 0u,
		                              channels > 3u ? 7u : 1u);
	};

	// 1-channel (k16UNorm) -> X001 (4, 0, 0, 1)
	const auto swizzle1 = swizzle_for_format(Prospero::BufferFormat::k16UNorm);
	Check(swizzle1 == Libs::Graphics::DstSel(4, 0, 0, 1),
	      "1-channel format must produce DstSel(4, 0, 0, 1)");

	// 2-channel (k16_16UNorm) -> XY01 (4, 5, 0, 1)
	const auto swizzle2 = swizzle_for_format(Prospero::BufferFormat::k16_16UNorm);
	Check(swizzle2 == Libs::Graphics::DstSel(4, 5, 0, 1),
	      "2-channel format must produce DstSel(4, 5, 0, 1)");

	// 3-channel (k32_32_32Float) -> XYZ1 (4, 5, 6, 1)
	const auto swizzle3 = swizzle_for_format(Prospero::BufferFormat::k32_32_32Float);
	Check(swizzle3 == Libs::Graphics::DstSel(4, 5, 6, 1),
	      "3-channel format must produce DstSel(4, 5, 6, 1)");

	// 4-channel (k16_16_16_16UNorm) -> XYZW (4, 5, 6, 7)
	const auto swizzle4 = swizzle_for_format(Prospero::BufferFormat::k16_16_16_16UNorm);
	Check(swizzle4 == Libs::Graphics::DstSel(4, 5, 6, 7),
	      "4-channel format must produce DstSel(4, 5, 6, 7)");
}

} // namespace

int main() {
	TestValidAndInvalidMetadata();
	TestGfx10BufferFormatDecoding();
	std::puts("ShaderVertexMetadataTests: all cases passed");
	return 0;
}
