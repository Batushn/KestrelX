#include "kestrel/hle/gameinput.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace kestrel::hle {

namespace {

// Clean-room binary layout matching Windows XINPUT_GAMEPAD / XINPUT_STATE (public ABI).
struct RawXInputGamepad {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct RawXInputState {
    DWORD dwPacketNumber;
    RawXInputGamepad Gamepad;
};

using XInputGetStateFn = DWORD(WINAPI*)(DWORD dwUserIndex, RawXInputState* pState);

struct RawXInputVibration {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};
using XInputSetStateFn = DWORD(WINAPI*)(DWORD dwUserIndex, RawXInputVibration* pVibration);

XInputSetStateFn resolve_xinput_set_state() {
    static XInputSetStateFn fn = []() -> XInputSetStateFn {
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
            if (HMODULE mod = LoadLibraryW(dll)) {
                if (auto p = reinterpret_cast<XInputSetStateFn>(GetProcAddress(mod, "XInputSetState"))) {
                    return p;
                }
            }
        }
        return nullptr;
    }();
    return fn;
}

std::atomic<uint64_t> g_instances{0};
std::atomic<uint64_t> g_readings{0};
std::atomic<uint64_t> g_rumble_commands{0};

std::mutex g_mock_mtx;
bool g_mock_enabled = false;
bool g_mock_connected = false;
KxGameInputGamepadState g_mock_state{};

XInputGetStateFn resolve_xinput() {
    static XInputGetStateFn fn = []() -> XInputGetStateFn {
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
            if (HMODULE mod = LoadLibraryW(dll)) {
                if (auto p = reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"))) {
                    return p;
                }
            }
        }
        return nullptr;
    }();
    return fn;
}

float norm_stick(int16_t raw, int16_t deadzone) {
    const int32_t v = raw;
    if (std::abs(v) <= deadzone) return 0.0f;
    const float f = (v < 0) ? (static_cast<float>(v) / 32768.0f) : (static_cast<float>(v) / 32767.0f);
    return std::clamp(f, -1.0f, 1.0f);
}

uint64_t qpc_microseconds() {
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (freq.QuadPart > 0) ? static_cast<uint64_t>((now.QuadPart * 1000000LL) / freq.QuadPart) : 0ull;
}

class KxGameInputReading final : public IKxGameInputReading {
public:
    KxGameInputReading(uint64_t ts, const KxGameInputGamepadState& st) : ts_(ts), state_(st) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    uint32_t STDMETHODCALLTYPE GetInputKind() override { return KxGameInputKindGamepad; }
    uint64_t STDMETHODCALLTYPE GetTimestamp() override { return ts_; }
    bool STDMETHODCALLTYPE GetGamepadState(KxGameInputGamepadState* out) override {
        if (!out) return false;
        *out = state_;
        return true;
    }

private:
    std::atomic<ULONG> ref_{1};
    uint64_t ts_ = 0;
    KxGameInputGamepadState state_{};
};

class KxGameInputDevice final : public IKxGameInputDevice {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    void STDMETHODCALLTYPE SetRumbleState(const KxGameInputRumbleParams* params) override {
        if (!params) return;
        g_rumble_commands.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(mtx_);
            current_rumble_ = *params;
        }
        if (auto sfn = resolve_xinput_set_state()) {
            RawXInputVibration vib{};
            vib.wLeftMotorSpeed = static_cast<WORD>(std::clamp(params->low_frequency, 0.0f, 1.0f) * 65535.0f);
            vib.wRightMotorSpeed = static_cast<WORD>(std::clamp(params->high_frequency, 0.0f, 1.0f) * 65535.0f);
            sfn(0, &vib);
        }
    }

    void STDMETHODCALLTYPE GetRumbleState(KxGameInputRumbleParams* out_params) override {
        if (!out_params) return;
        std::lock_guard lock(mtx_);
        *out_params = current_rumble_;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::mutex mtx_;
    KxGameInputRumbleParams current_rumble_{};
};

class KxGameInput final : public IKxGameInput {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() override { return qpc_microseconds(); }

    HRESULT STDMETHODCALLTYPE GetCurrentReading(
        uint32_t input_kind, void*, IKxGameInputReading** out_reading) override {
        if (!out_reading) return E_POINTER;
        *out_reading = nullptr;
        if ((input_kind & KxGameInputKindGamepad) == 0) return E_INVALIDARG;

        KxGameInputGamepadState state{};
        bool connected = false;

        {
            std::lock_guard lock(g_mock_mtx);
            if (g_mock_enabled) {
                if (!g_mock_connected) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
                state = g_mock_state;
                connected = true;
            }
        }

        if (!connected) {
            if (auto xfn = resolve_xinput()) {
                for (DWORD u = 0; u < 4; ++u) {
                    RawXInputState xs{};
                    if (xfn(u, &xs) == ERROR_SUCCESS) {
                        state = translate_xinput_raw(
                            xs.Gamepad.wButtons, xs.Gamepad.bLeftTrigger, xs.Gamepad.bRightTrigger,
                            xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY, xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY);
                        connected = true;
                        break;
                    }
                }
            }
        }

        // Keyboard fallback (WASD / Space / Enter) when no hardware gamepad is attached
        if (!connected) {
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) state.buttons |= KxGamepadA;
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) state.buttons |= KxGamepadMenu;
            if (GetAsyncKeyState('W') & 0x8000) state.left_thumbstick_y = 1.0f;
            if (GetAsyncKeyState('S') & 0x8000) state.left_thumbstick_y = -1.0f;
            if (GetAsyncKeyState('D') & 0x8000) state.left_thumbstick_x = 1.0f;
            if (GetAsyncKeyState('A') & 0x8000) state.left_thumbstick_x = -1.0f;
        }

        g_readings.fetch_add(1, std::memory_order_relaxed);
        *out_reading = new KxGameInputReading(qpc_microseconds(), state);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDefaultDevice(IKxGameInputDevice** out_dev) override {
        if (!out_dev) return E_POINTER;
        *out_dev = new KxGameInputDevice();
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
};

HRESULT __stdcall kx_GameInputCreate(IKxGameInput** out_gi) {
    if (!out_gi) return E_POINTER;
    g_instances.fetch_add(1, std::memory_order_relaxed);
    *out_gi = new KxGameInput();
    return S_OK;
}

}  // namespace

KxGameInputGamepadState translate_xinput_raw(
    uint16_t xb, uint8_t lt, uint8_t rt,
    int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    KxGameInputGamepadState s{};
    if (xb & 0x0001u) s.buttons |= KxGamepadDPadUp;
    if (xb & 0x0002u) s.buttons |= KxGamepadDPadDown;
    if (xb & 0x0004u) s.buttons |= KxGamepadDPadLeft;
    if (xb & 0x0008u) s.buttons |= KxGamepadDPadRight;
    if (xb & 0x0010u) s.buttons |= KxGamepadMenu;
    if (xb & 0x0020u) s.buttons |= KxGamepadView;
    if (xb & 0x0040u) s.buttons |= KxGamepadLeftThumbstick;
    if (xb & 0x0080u) s.buttons |= KxGamepadRightThumbstick;
    if (xb & 0x0100u) s.buttons |= KxGamepadLeftShoulder;
    if (xb & 0x0200u) s.buttons |= KxGamepadRightShoulder;
    if (xb & 0x1000u) s.buttons |= KxGamepadA;
    if (xb & 0x2000u) s.buttons |= KxGamepadB;
    if (xb & 0x4000u) s.buttons |= KxGamepadX;
    if (xb & 0x8000u) s.buttons |= KxGamepadY;

    s.left_trigger = (lt > 30) ? (static_cast<float>(lt) / 255.0f) : 0.0f;
    s.right_trigger = (rt > 30) ? (static_cast<float>(rt) / 255.0f) : 0.0f;
    s.left_thumbstick_x = norm_stick(lx, 7849);
    s.left_thumbstick_y = norm_stick(ly, 7849);
    s.right_thumbstick_x = norm_stick(rx, 8689);
    s.right_thumbstick_y = norm_stick(ry, 8689);
    return s;
}

void set_gameinput_mock_state(const KxGameInputGamepadState& state, bool connected) {
    std::lock_guard lock(g_mock_mtx);
    g_mock_enabled = true;
    g_mock_connected = connected;
    g_mock_state = state;
}

void clear_gameinput_mock_state() {
    std::lock_guard lock(g_mock_mtx);
    g_mock_enabled = false;
    g_mock_connected = false;
    g_mock_state = {};
}

void register_gameinput(Registry& reg) {
    reg.register_module("gameinput.dll");
    reg.register_export("gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&kx_GameInputCreate));
    reg.register_export("xgameruntime.dll", "GameInputCreate", reinterpret_cast<void*>(&kx_GameInputCreate));
}

GameInputStats gameinput_stats() { return {g_instances.load(), g_readings.load(), g_rumble_commands.load()}; }

void reset_gameinput_stats() {
    g_instances = 0;
    g_readings = 0;
    g_rumble_commands = 0;
    clear_gameinput_mock_state();
}

}  // namespace kestrel::hle
