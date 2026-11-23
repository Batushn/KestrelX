#pragma once

#include "kestrel/hle/registry.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <unknwn.h>
#include <cstdint>

namespace kestrel::hle {

enum KxGameInputKind : uint32_t {
    KxGameInputKindUnknown = 0x00000000,
    KxGameInputKindGamepad = 0x00010000,
};

enum KxGameInputGamepadButtons : uint32_t {
    KxGamepadNone           = 0x00000000,
    KxGamepadMenu           = 0x00000001,
    KxGamepadView           = 0x00000002,
    KxGamepadA              = 0x00000004,
    KxGamepadB              = 0x00000008,
    KxGamepadX              = 0x00000010,
    KxGamepadY              = 0x00000020,
    KxGamepadDPadUp         = 0x00000040,
    KxGamepadDPadDown       = 0x00000080,
    KxGamepadDPadLeft       = 0x00000100,
    KxGamepadDPadRight      = 0x00000200,
    KxGamepadLeftShoulder   = 0x00000400,
    KxGamepadRightShoulder  = 0x00000800,
    KxGamepadLeftThumbstick = 0x00001000,
    KxGamepadRightThumbstick= 0x00002000,
};

struct KxGameInputGamepadState {
    uint32_t buttons = 0;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
    float left_thumbstick_x = 0.0f;
    float left_thumbstick_y = 0.0f;
    float right_thumbstick_x = 0.0f;
    float right_thumbstick_y = 0.0f;
};

struct IKxGameInputReading : public IUnknown {
    virtual uint32_t STDMETHODCALLTYPE GetInputKind() = 0;
    virtual uint64_t STDMETHODCALLTYPE GetTimestamp() = 0;
    virtual bool STDMETHODCALLTYPE GetGamepadState(KxGameInputGamepadState* state) = 0;
};

struct IKxGameInput : public IUnknown {
    virtual uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentReading(
        uint32_t input_kind, void* device, IKxGameInputReading** reading) = 0;
};

struct GameInputStats {
    uint64_t instances_created = 0;
    uint64_t readings_polled = 0;
};

// Pure helper for deterministic XInput word/axis -> GameInput state conversion.
KxGameInputGamepadState translate_xinput_raw(
    uint16_t xinput_buttons,
    uint8_t left_trigger,
    uint8_t right_trigger,
    int16_t thumb_lx,
    int16_t thumb_ly,
    int16_t thumb_rx,
    int16_t thumb_ry);

void set_gameinput_mock_state(const KxGameInputGamepadState& state, bool connected = true);
void clear_gameinput_mock_state();
void register_gameinput(Registry& reg);
GameInputStats gameinput_stats();
void reset_gameinput_stats();

}  // namespace kestrel::hle
