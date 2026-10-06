#include "GpuAssetStreamer.h"
#include "GuestVirtualMemory.h"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void WriteWords(const std::filesystem::path& path, std::span<const std::uint32_t> words) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size_bytes()))) {
        throw std::runtime_error("Cannot write test fixture");
    }
}
int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) throw std::invalid_argument("Usage: gpu_asset_streamer_demo decode.spv requests.spv fixture-directory [--validation]");
        const bool validation = argc == 5 && std::string_view(argv[4]) == "--validation";
        if (argc == 5 && !validation) throw std::invalid_argument("Unknown argument");
        GuestVirtualMemory nativePageProbe(1);
        const auto guestBytes = nativePageProbe.PageSize() * 2 + 128;
        GuestVirtualMemory guest(guestBytes);
        Require(reinterpret_cast<std::uintptr_t>(guest.BaseAddress()) % guest.PageSize() == 0, "Native page alignment");
        std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}, read{};
        const auto offset = guest.PageSize() - 2;
        guest.Write(offset, bytes); guest.Read(offset, read);
        Require(read == bytes, "Guest memory read/write");
        for (std::size_t page = 0; page < 2; ++page) {
            const auto use = guest.Utilization(page);
            Require(use.reads == 1 && use.writes == 1, "Per-page utilization");
        }
        bool rejected = false;
        try { guest.Write(guestBytes - 1, bytes); } catch (const std::out_of_range&) { rejected = true; }
        Require(rejected, "Guest range overflow accepted");
        std::vector<std::jthread> counterWorkers;
        for (unsigned index = 0; index < 8; ++index) {
            counterWorkers.emplace_back([&, index] {
                const std::array<std::byte, 1> value{static_cast<std::byte>(index)};
                std::array<std::byte, 1> observed{};
                // Separate bytes avoid a data race while sharing the same page counters.
                for (unsigned repeat = 0; repeat < 1000; ++repeat) {
                    guest.Write(guest.PageSize() * 2 + index, value);
                    guest.Read(guest.PageSize() * 2 + index, observed);
                }
            });
        }
        counterWorkers.clear();
        const auto concurrentUse = guest.Utilization(2);
        Require(concurrentUse.reads == 8000 && concurrentUse.writes == 8000, "Concurrent page counters lost updates");
        const std::filesystem::path directory(argv[3]);
        std::filesystem::create_directories(directory);
        const auto first = directory / "fixture-a.rle1";
        const auto second = directory / "fixture-b.rle1";
        const auto invalid = directory / "fixture-invalid.rle1";
        const std::uint32_t a[]{0x31454c52u, 257, 3, 0,
            0, 5, 0x11223344u, 5, 200, 0xaabbccddu, 205, 52, 0x55667788u};
        const std::uint32_t b[]{0x31454c52u, 262145, 2, 0,
            0, 262144, 0x13579bdfu, 262144, 1, 0x2468ace0u};
        const std::uint32_t bad[]{0x31454c52u, 257, 1, 0, 1, 257, 42};
        WriteWords(first, a); WriteWords(second, b); WriteWords(invalid, bad);
        const std::vector<std::vector<std::uint32_t>> corrupt{
            {0x31454c52u, 257, 1, 0, 0, 0, 42}, // Empty run.
            {0x31454c52u, 257, 1, 0, 0, 258, 42}, // Output overflow.
            {0x31454c52u, 257, 1, 0, 0, 256, 42}, // Missing final word.
            {0x31454c52u, 257, 2, 0, 0, 257, 42}, // Truncated run table.
            {0x31454c52u, 257, 1, 1, 0, 257, 42}, // Invalid reserved field.
            {0x31454c52u, 257, 2, 0, 0, 200, 42, 199, 58, 43}, // Overlapping runs.
            {0x31454c52u, 257, 1, 0} // No records.
        };
        std::shared_ptr<StreamingDevice> device;
        std::vector<StreamedAsset> retained;
        {
            GpuAssetStreamer streamer(argv[1], argv[2], validation);
            device = streamer.Device();
            std::cout << "GPU: " << device->properties.deviceName << '\n';
            // ID 1 is not requested by the GPU, so its nonexistent file must not be opened.
            auto valid = streamer.StreamGpuDemandAsync({first, directory / "not-requested.missing", second}, 2);
            auto malformed = streamer.StreamGpuDemandAsync({invalid}, 1);
            auto subsequent = streamer.StreamGpuDemandAsync({first}, 1);
            auto assets = valid.get();
            Require(assets.size() == 2 && assets[0].id == 0 && assets[1].id == 2, "GPU request selection");
            Require(assets[0].data->Size() == 257 * 4 && assets[1].data->Size() == 262145 * 4, "Decoded sizes");
            const auto* decodedA = reinterpret_cast<const std::uint32_t*>(assets[0].data->Bytes().data());
            for (std::uint32_t word = 0; word < 257; ++word) {
                const auto expected = word < 5 ? 0x11223344u : (word < 205 ? 0xaabbccddu : 0x55667788u);
                Require(decodedA[word] == expected, "GPU decode A mismatch");
            }
            const auto* decodedB = reinterpret_cast<const std::uint32_t*>(assets[1].data->Bytes().data());
            for (std::uint32_t word = 0; word < 262145; ++word) {
                Require(decodedB[word] == (word < 262144 ? 0x13579bdfu : 0x2468ace0u), "GPU decode B mismatch");
            }
            rejected = false;
            try { (void)malformed.get(); } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected, "Malformed RLE table accepted");
            Require(subsequent.get().size() == 1, "Worker did not recover after invalid file");
            for (std::size_t index = 0; index < corrupt.size(); ++index) {
                const auto path = directory / ("fixture-corrupt-" + std::to_string(index) + ".rle1");
                WriteWords(path, corrupt[index]);
                rejected = false;
                try { (void)streamer.StreamGpuDemandAsync({path}).get(); }
                catch (const std::runtime_error&) { rejected = true; }
                Require(rejected, "Corrupt RLE variant was accepted");
            }
            std::cout << "Output memory flags: " << assets[0].data->Properties()
                      << "; page size: " << guest.PageSize() << '\n';
            retained = std::move(assets);
        }
        Require(reinterpret_cast<const std::uint32_t*>(retained[0].data->Bytes().data())[0] == 0x11223344u,
                "Asset lifetime did not survive service shutdown");
        retained.clear();
        Require(device->validationErrors.load() == 0, "Vulkan validation errors");
        {
            GpuAssetStreamer twoDimensional(argv[1], argv[2], validation, {3, 0});
            auto decoded = twoDimensional.StreamGpuDemandAsync({second}).get();
            const auto* words = reinterpret_cast<const std::uint32_t*>(decoded[0].data->Bytes().data());
            for (std::uint32_t word = 0; word < 262145; ++word) {
                Require(words[word] == (word < 262144 ? 0x13579bdfu : 0x2468ace0u), "2D GPU decode mismatch");
            }
            Require(twoDimensional.Device()->validationErrors.load() == 0, "2D Vulkan validation errors");
        }
        std::cout << "PASS: concurrent counters, malformed table variants, retained asset lifetime and 2D GPU decode\n";
        bool hasNoncoherent = false;
        for (std::uint32_t index = 0; index < device->memory.memoryTypeCount; ++index) {
            const auto flags = device->memory.memoryTypes[index].propertyFlags;
            hasNoncoherent = hasNoncoherent || ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                                              !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        }
        if (hasNoncoherent) {
            try {
                GpuAssetStreamer noncoherent(argv[1], argv[2], validation, {3, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT});
                auto decoded = noncoherent.StreamGpuDemandAsync({first}).get();
                Require(!(decoded[0].data->Properties() & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), "Noncoherent policy ignored");
                const auto* words = reinterpret_cast<const std::uint32_t*>(decoded[0].data->Bytes().data());
                for (std::uint32_t word = 0; word < 257; ++word) {
                    Require(words[word] == (word < 5 ? 0x11223344u : (word < 205 ? 0xaabbccddu : 0x55667788u)),
                            "Noncoherent decode mismatch");
                }
                Require(noncoherent.Device()->validationErrors.load() == 0, "Noncoherent Vulkan validation errors");
                std::cout << "PASS: real noncoherent mapped memory flush/invalidate\n";
            } catch (const std::runtime_error& error) {
                if (std::string_view(error.what()) != "No compatible HOST_VISIBLE storage memory") throw;
                std::cout << "SKIP: noncoherent memory type incompatible with storage buffers\n";
            }
        } else {
            std::cout << "SKIP: device exposes no HOST_VISIBLE noncoherent memory type\n";
        }
        std::cout << "PASS: native guest mapping/counters, GPU requests, direct mapped file reads, "
                     "GPU RLE decode, invalid input rejection and worker recovery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
