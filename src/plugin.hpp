#pragma once
#include "abi.hpp"
#include "dsp.hpp"
#include <atomic>
#include <array>
#include <cstdint>

namespace sv {
class Editor;
class Plugin {
public:
    explicit Plugin(vst_host_callback_t host) noexcept;
    ~Plugin();
    vst_effect_t effect{};
    vst_host_callback_t host=nullptr;
    float parameter(int i) const noexcept;
    void setParameter(int i, float normalized) noexcept;
    void editParameter(int i, float normalized) noexcept;
    void beginEdit(int i) noexcept;
    void endEdit(int i) noexcept;
    void choosePreset(int i, bool notify) noexcept;
    int currentPreset() const noexcept { return program_.load(std::memory_order_relaxed); }
    float sampleRate() const noexcept { return rate_.load(std::memory_order_relaxed); }
    float latencyMs() const noexcept { return 1000.0f*reportedLatency_.load(std::memory_order_relaxed)/rate_.load(std::memory_order_relaxed); }
    MeterState meterSnapshot() const noexcept;
    intptr_t dispatch(int opcode, int index, intptr_t value, void* ptr, float opt);
    template<class T> void render(const T* const* inputs, T** outputs, int frames, bool accumulating=false) noexcept;
private:
    std::array<std::atomic<float>,ParamCount> parameters_{};
    std::atomic<int> program_{0};
    std::atomic<float> rate_{48000};
    std::atomic<int> reportedLatency_{0};
    std::atomic<bool> resetRequested_{true};
    std::array<std::atomic<float>,6> meters_{};
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> invalid_{0};
    Leveler dsp_;
    void updateLatency() noexcept;
    struct Chunk { uint32_t magic, version, count, program; float values[ParamCount]; } chunk_{};
    static_assert(sizeof(Chunk)==16+ParamCount*4);
    vst_rect_t rect_{0,0,720,900};
    Editor* editor_=nullptr;
    vst_speaker_arrangement_t arrangement_{};
};
}
