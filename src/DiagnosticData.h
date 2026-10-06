#pragma once

#include "Civ6FixCore.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace civ6fix {

struct RawReadResult {
    bool succeeded;
    std::size_t transferred;
    std::uint32_t error;
};

template <typename Reader, typename T>
std::uint32_t ReadExactValue(Reader&& reader, std::uintptr_t address, T& value) {
    if (!IsAddressAdditionSafe(address, sizeof(T) - 1)) return 487;
    T candidate{};
    const auto result = reader(address, &candidate, sizeof(candidate));
    if (!result.succeeded) return result.error == 0 ? 299 : result.error;
    if (result.transferred != sizeof(candidate)) return 299; // ERROR_PARTIAL_COPY
    value = candidate;
    return 0;
}

// Only typed, bounded observations belong here. Never add paths, addresses,
// process/account identifiers or arbitrary platform/log text to this structure.
enum class ReadQuality {
    NotSampled, Value, NotCreated, PointerReadFailed, ValueReadFailed,
    InvalidAddress, Unavailable,
};

struct FieldRead {
    ReadQuality quality = ReadQuality::NotSampled;
    std::uint32_t error = 0;
};

struct MonitorSample {
    bool readable = false;  // True only when ALL three fields were read in this poll.
    std::uint64_t skippedInvalidUnlocks = 0;
    int discoveryState = -1;
    int ssoState = -1;
    FieldRead counter;
    FieldRead discovery;
    FieldRead sso;

    MonitorSample() = default;
    MonitorSample(bool complete, std::uint64_t skipped, int discoveryValue, int ssoValue)
        : readable(complete), skippedInvalidUnlocks(skipped),
          discoveryState(discoveryValue), ssoState(ssoValue) {
        const FieldRead field{complete ? ReadQuality::Value : ReadQuality::Unavailable, 0};
        counter = discovery = sso = field;
    }
};

inline bool CompleteSample(const MonitorSample& sample) noexcept {
    return sample.readable && sample.counter.quality == ReadQuality::Value &&
           sample.discovery.quality == ReadQuality::Value && sample.sso.quality == ReadQuality::Value;
}

// Reader returns 0 only for an exact read. Failed/partial reads must never be
// mistaken for a numeric state, even if a reader touched the destination.
template <typename Reader>
FieldRead ReadServiceField(Reader&& reader, std::uintptr_t pointerAddress,
                          std::uintptr_t stateOffset, int& value) {
    value = -1;
    std::uintptr_t pointer = 0;
    std::uint32_t error = reader(pointerAddress, pointer);
    if (error != 0) return {ReadQuality::PointerReadFailed, error};
    if (pointer == 0) return {ReadQuality::NotCreated, 0};
    if (!IsAddressAdditionSafe(pointer, stateOffset) ||
        !IsAddressAdditionSafe(pointer + stateOffset, sizeof(int) - 1)) {
        return {ReadQuality::InvalidAddress, 487}; // ERROR_INVALID_ADDRESS
    }
    int candidate = -1;
    error = reader(pointer + stateOffset, candidate);
    if (error != 0) return {ReadQuality::ValueReadFailed, error};
    value = candidate;
    return {ReadQuality::Value, 0};
}

enum class VectorProbeState { NotAttempted, ReadFailed, NotCreated, Available, InvalidLayout };
struct VectorProbe {
    VectorProbeState state = VectorProbeState::NotAttempted;
    std::uint32_t error = 0;
    std::uintptr_t begin = 0; // Local validation only; NEVER copied to SessionDiagnostics.
};

template <typename Reader>
VectorProbe ReadLockVector(Reader&& reader, std::uintptr_t address,
                           std::uint32_t index) {
    std::uintptr_t begin = 0, end = 0;
    if (!IsAddressAdditionSafe(address, sizeof(std::uintptr_t))) {
        return {VectorProbeState::ReadFailed, 487, 0};
    }
    auto error = reader(address, begin);
    if (error == 0) error = reader(address + sizeof(std::uintptr_t), end);
    if (error != 0) return {VectorProbeState::ReadFailed, error, 0};
    if (begin == 0 && end == 0) return {VectorProbeState::NotCreated, 0, 0};
    if (!IsPointerVectorIndexAvailable(begin, end, index)) {
        return {VectorProbeState::InvalidLayout, 13, 0}; // ERROR_INVALID_DATA
    }
    return {VectorProbeState::Available, 0, begin};
}

template <typename Reader>
MonitorSample SampleMonitor(Reader&& reader, std::uintptr_t counterAddress,
                             std::uintptr_t discoveryAddress, std::uintptr_t ssoAddress) {
    MonitorSample sample;
    std::uint64_t counter = 0;
    const auto error = reader(counterAddress, counter);
    sample.counter = {error == 0 ? ReadQuality::Value : ReadQuality::ValueReadFailed, error};
    if (error == 0) sample.skippedInvalidUnlocks = counter;
    sample.discovery = ReadServiceField(reader, discoveryAddress, 0x88U, sample.discoveryState);
    sample.sso = ReadServiceField(reader, ssoAddress, 0x3D8U, sample.ssoState);
    sample.readable = true;
    sample.readable = CompleteSample(sample);
    return sample;
}

struct FieldStatistics {
    std::uint64_t values = 0;
    std::uint64_t absent = 0;
    std::uint64_t failures = 0;
    std::uint64_t lastValueAtMs = 0;
    std::uint32_t lastError = 0;
};

struct DiagnosticEvent {
    std::uint64_t atMs = 0; // Relative to monitor start; never a wall-clock timestamp.
    MonitorSample sample;
};

struct SessionDiagnostics {
    bool available = false;
    std::uint64_t elapsedMs = 0;
    std::array<std::uint64_t, 8> phaseMs{};
    int lastActivePhase = 0;
    std::uint32_t monitorTimeoutMs = 0;
    std::uint32_t monitorPollMs = 0;
    std::uint32_t waitTimeoutMs = 0;
    bool validationAttempted = false;
    int validationOutcome = 0;
    std::uint32_t validationError = 0;
    bool installAttempted = false;
    int installOutcome = 0;
    std::uint32_t installError = 0;
    std::uint32_t scanError = 0;
    bool guardInstalled = false;
    std::optional<std::uint64_t> processAgeAtGuardMs;
    VectorProbeState mutexProbe = VectorProbeState::NotAttempted;
    std::optional<std::int32_t> mutexCountBeforeGuard;
    std::uint64_t samples = 0;
    std::uint64_t completeSamples = 0;
    std::uint64_t monitorElapsedMs = 0;
    std::uint64_t lastSampleAtMs = 0;
    std::uint64_t maxSampleGapMs = 0;
    std::optional<std::uint64_t> firstInterceptionAtMs;
    FieldStatistics counter, discovery, sso;
    MonitorSample latest;
    std::uint64_t lastGoodCounter = 0;
    int lastGoodDiscovery = -1, lastGoodSso = -1;
    static constexpr std::size_t kEventLimit = 32;
    static constexpr std::size_t kKeepFirst = 4;
    std::vector<DiagnosticEvent> events;
    std::size_t eventCount = 0;
    std::uint64_t omittedEvents = 0;

    static bool SameField(const FieldRead& a, const FieldRead& b) noexcept {
        return a.quality == b.quality && a.error == b.error;
    }
    static bool SameSample(const MonitorSample& a, const MonitorSample& b) noexcept {
        return SameField(a.counter, b.counter) && SameField(a.discovery, b.discovery) &&
               SameField(a.sso, b.sso) &&
               (a.counter.quality != ReadQuality::Value || a.skippedInvalidUnlocks == b.skippedInvalidUnlocks) &&
               (a.discovery.quality != ReadQuality::Value || a.discoveryState == b.discoveryState) &&
               (a.sso.quality != ReadQuality::Value || a.ssoState == b.ssoState);
    }
    static void RecordField(FieldStatistics& stats, const FieldRead& field,
                            std::uint64_t atMs) noexcept {
        if (field.quality == ReadQuality::Value) {
            ++stats.values;
            stats.lastValueAtMs = atMs;
        } else if (field.quality == ReadQuality::NotCreated) {
            ++stats.absent;
        } else {
            ++stats.failures;
            if (field.error != 0) stats.lastError = field.error;
        }
    }
    bool Record(const MonitorSample& sample, std::uint64_t atMs) {
        const bool changed = samples == 0 || !SameSample(latest, sample);
        if (samples != 0 && atMs >= lastSampleAtMs) {
            maxSampleGapMs = (std::max)(maxSampleGapMs, atMs - lastSampleAtMs);
        }
        ++samples;
        if (CompleteSample(sample)) ++completeSamples;
        lastSampleAtMs = monitorElapsedMs = atMs;
        RecordField(counter, sample.counter, atMs);
        RecordField(discovery, sample.discovery, atMs);
        RecordField(sso, sample.sso, atMs);
        if (sample.counter.quality == ReadQuality::Value) {
            lastGoodCounter = sample.skippedInvalidUnlocks;
            if (lastGoodCounter != 0 && !firstInterceptionAtMs.has_value()) firstInterceptionAtMs = atMs;
        }
        if (sample.discovery.quality == ReadQuality::Value) lastGoodDiscovery = sample.discoveryState;
        if (sample.sso.quality == ReadQuality::Value) lastGoodSso = sample.ssoState;
        latest = sample;
        if (changed) {
            if (events.empty()) events.reserve(kEventLimit);
            if (eventCount == kEventLimit) {
                for (std::size_t i = kKeepFirst; i + 1 < events.size(); ++i) events[i] = events[i + 1];
                events.back() = {atMs, sample};
                ++omittedEvents;
            } else {
                events.push_back({atMs, sample});
                ++eventCount;
            }
        }
        return changed;
    }
};

} // namespace civ6fix
