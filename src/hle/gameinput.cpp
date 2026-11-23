#include "kestrel/hle/gameinput.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

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

constexpr uint64_t make_device_id(uint32_t slot) {
    return 0x4B58474900000001ull + static_cast<uint64_t>(slot);
}

float clamp_unit_float(float v) {
    if (!std::isfinite(v)) return 0.0f;
    return std::clamp(v, 0.0f, 1.0f);
}

struct ControllerSlot {
    bool mock_enabled = false;
    bool mock_connected = false;
    KxGameInputGamepadState mock_state{};
    bool hw_connected = false;
    bool last_connected = false;
    KxGameInputRumbleParams rumble{};
};

struct CallbackEntry {
    KxGameInputCallbackToken token = 0;
    uint64_t owner_id = 0;
    void* context = nullptr;
    KxGameInputDeviceCallback callback = nullptr;
};

std::atomic<uint64_t> g_instances{0};
std::atomic<uint64_t> g_readings{0};
std::atomic<uint64_t> g_rumble_commands{0};
std::atomic<uint64_t> g_active_controllers{0};
std::atomic<uint64_t> g_hotplug_events{0};
std::atomic<uint64_t> g_next_instance_id{1};
std::atomic<uint64_t> g_next_callback_token{1};

std::mutex g_state_mtx;
bool g_mock_enabled = false;
bool g_mock_connected = false;
KxGameInputGamepadState g_mock_state{};
std::array<ControllerSlot, kMaxControllerSlots> g_slots{};
std::vector<CallbackEntry> g_callbacks;

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

bool effective_connected_locked(uint32_t slot) {
    if (g_slots[slot].mock_enabled) return g_slots[slot].mock_connected;
    if (slot == 0 && g_mock_enabled) return g_mock_connected;
    return g_slots[slot].hw_connected;
}

void recompute_active_controllers_locked() {
    uint64_t active = 0;
    for (uint32_t i = 0; i < kMaxControllerSlots; ++i) {
        if (effective_connected_locked(i)) ++active;
    }
    g_active_controllers.store(active, std::memory_order_relaxed);
}

class KxGameInputDevice;
void dispatch_hotplug_callbacks(
    uint32_t slot,
    bool now_connected,
    bool prev_connected,
    std::vector<CallbackEntry> snapshot);

void observe_hardware_connection(uint32_t slot, bool hw_conn) {
    if (slot >= kMaxControllerSlots) return;
    std::vector<CallbackEntry> snapshot;
    bool notify = false;
    bool prev = false;
    {
        std::lock_guard lock(g_state_mtx);
        g_slots[slot].hw_connected = hw_conn;
        if (g_slots[slot].mock_enabled || (slot == 0 && g_mock_enabled)) {
            return;
        }
        prev = g_slots[slot].last_connected;
        g_slots[slot].last_connected = hw_conn;
        recompute_active_controllers_locked();
        if (prev != hw_conn) {
            g_hotplug_events.fetch_add(1, std::memory_order_relaxed);
            snapshot = g_callbacks;
            notify = true;
        }
    }
    if (notify) {
        dispatch_hotplug_callbacks(slot, hw_conn, prev, std::move(snapshot));
    }
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
    explicit KxGameInputDevice(uint32_t slot_index)
        : slot_index_(slot_index), device_id_(make_device_id(slot_index)) {}

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
        KxGameInputRumbleParams clamped{};
        clamped.low_frequency = clamp_unit_float(params->low_frequency);
        clamped.high_frequency = clamp_unit_float(params->high_frequency);
        clamped.left_trigger = clamp_unit_float(params->left_trigger);
        clamped.right_trigger = clamp_unit_float(params->right_trigger);

        g_rumble_commands.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(g_state_mtx);
            if (slot_index_ < kMaxControllerSlots) {
                g_slots[slot_index_].rumble = clamped;
            }
        }
        if (auto sfn = resolve_xinput_set_state()) {
            RawXInputVibration vib{};
            vib.wLeftMotorSpeed = static_cast<WORD>(clamped.low_frequency * 65535.0f);
            vib.wRightMotorSpeed = static_cast<WORD>(clamped.high_frequency * 65535.0f);
            sfn(static_cast<DWORD>(slot_index_), &vib);
        }
    }

    void STDMETHODCALLTYPE GetRumbleState(KxGameInputRumbleParams* out_params) override {
        if (!out_params) return;
        std::lock_guard lock(g_state_mtx);
        if (slot_index_ < kMaxControllerSlots) {
            *out_params = g_slots[slot_index_].rumble;
        } else {
            *out_params = {};
        }
    }

    uint32_t STDMETHODCALLTYPE GetSlotIndex() override { return slot_index_; }

    bool STDMETHODCALLTYPE IsConnected() override {
        if (slot_index_ >= kMaxControllerSlots) return false;
        {
            std::lock_guard lock(g_state_mtx);
            if (g_slots[slot_index_].mock_enabled) {
                return g_slots[slot_index_].mock_connected;
            }
            if (slot_index_ == 0 && g_mock_enabled) {
                return g_mock_connected;
            }
        }
        if (auto xfn = resolve_xinput()) {
            RawXInputState xs{};
            const bool hw_conn = (xfn(static_cast<DWORD>(slot_index_), &xs) == ERROR_SUCCESS);
            observe_hardware_connection(slot_index_, hw_conn);
            return hw_conn;
        }
        return false;
    }

    uint64_t STDMETHODCALLTYPE GetDeviceId() override { return device_id_; }

private:
    std::atomic<ULONG> ref_{1};
    uint32_t slot_index_ = 0;
    uint64_t device_id_ = 0;
};

void dispatch_hotplug_callbacks(
    uint32_t slot,
    bool now_connected,
    bool prev_connected,
    std::vector<CallbackEntry> snapshot) {
    if (snapshot.empty()) return;

    auto* dev = new KxGameInputDevice(slot);
    const uint64_t ts = qpc_microseconds();
    const auto cur_status = now_connected ? KxGameInputDeviceStatus::Connected
                                          : KxGameInputDeviceStatus::Disconnected;
    const auto old_status = prev_connected ? KxGameInputDeviceStatus::Connected
                                           : KxGameInputDeviceStatus::Disconnected;

    for (const auto& entry : snapshot) {
        if (!entry.callback) continue;
        bool still_registered = false;
        {
            std::lock_guard lock(g_state_mtx);
            still_registered = std::any_of(
                g_callbacks.begin(), g_callbacks.end(),
                [&](const CallbackEntry& c) { return c.token == entry.token; });
        }
        if (!still_registered) continue;
        entry.callback(entry.token, entry.context, dev, ts, cur_status, old_status);
    }

    dev->Release();
}

class KxGameInput final : public IKxGameInput {
public:
    KxGameInput() : instance_id_(g_next_instance_id.fetch_add(1, std::memory_order_relaxed)) {}

    ~KxGameInput() {
        std::lock_guard lock(g_state_mtx);
        g_callbacks.erase(
            std::remove_if(g_callbacks.begin(), g_callbacks.end(),
                           [this](const CallbackEntry& c) { return c.owner_id == instance_id_; }),
            g_callbacks.end());
    }

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
        uint32_t input_kind, void* device, IKxGameInputReading** out_reading) override {
        if (!out_reading) return E_POINTER;
        *out_reading = nullptr;
        if ((input_kind & KxGameInputKindGamepad) == 0) return E_INVALIDARG;

        KxGameInputGamepadState state{};
        bool connected = false;

        // Targeted device/slot polling
        if (device != nullptr) {
            auto* dev = static_cast<IKxGameInputDevice*>(device);
            const uint32_t slot = dev->GetSlotIndex();
            if (slot >= kMaxControllerSlots) return E_INVALIDARG;

            bool mock_controlled = false;
            {
                std::lock_guard lock(g_state_mtx);
                if (g_slots[slot].mock_enabled) {
                    mock_controlled = true;
                    if (!g_slots[slot].mock_connected) {
                        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
                    }
                    state = g_slots[slot].mock_state;
                    connected = true;
                } else if (slot == 0 && g_mock_enabled) {
                    mock_controlled = true;
                    if (!g_mock_connected) {
                        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
                    }
                    state = g_mock_state;
                    connected = true;
                }
            }

            if (!mock_controlled) {
                if (auto xfn = resolve_xinput()) {
                    RawXInputState xs{};
                    if (xfn(static_cast<DWORD>(slot), &xs) == ERROR_SUCCESS) {
                        observe_hardware_connection(slot, true);
                        state = translate_xinput_raw(
                            xs.Gamepad.wButtons, xs.Gamepad.bLeftTrigger, xs.Gamepad.bRightTrigger,
                            xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY, xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY);
                        connected = true;
                    } else {
                        observe_hardware_connection(slot, false);
                    }
                }
                if (!connected) {
                    return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
                }
            }

            g_readings.fetch_add(1, std::memory_order_relaxed);
            *out_reading = new KxGameInputReading(qpc_microseconds(), state);
            return S_OK;
        }

        // Unfiltered polling (device == nullptr): preserve legacy global mock,
        // per-slot connected mock, first connected XInput slot 0..3, or keyboard fallback.
        {
            std::lock_guard lock(g_state_mtx);
            if (g_mock_enabled) {
                if (!g_mock_connected) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
                state = g_mock_state;
                connected = true;
            } else {
                for (uint32_t u = 0; u < kMaxControllerSlots; ++u) {
                    if (g_slots[u].mock_enabled && g_slots[u].mock_connected) {
                        state = g_slots[u].mock_state;
                        connected = true;
                        break;
                    }
                }
            }
        }

        if (!connected) {
            if (auto xfn = resolve_xinput()) {
                for (DWORD u = 0; u < kMaxControllerSlots; ++u) {
                    bool slot_mocked = false;
                    {
                        std::lock_guard lock(g_state_mtx);
                        slot_mocked = g_slots[u].mock_enabled;
                    }
                    if (slot_mocked) continue;
                    RawXInputState xs{};
                    if (xfn(u, &xs) == ERROR_SUCCESS) {
                        observe_hardware_connection(u, true);
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
        return GetDeviceForSlot(0, out_dev);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceForSlot(uint32_t slot, IKxGameInputDevice** out_dev) override {
        if (!out_dev) return E_POINTER;
        *out_dev = nullptr;
        if (slot >= kMaxControllerSlots) return E_INVALIDARG;
        *out_dev = new KxGameInputDevice(slot);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(
        void* context,
        KxGameInputDeviceCallback callback,
        KxGameInputCallbackToken* out_token) override {
        if (!callback) return E_INVALIDARG;
        const KxGameInputCallbackToken token = g_next_callback_token.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(g_state_mtx);
            g_callbacks.push_back(CallbackEntry{token, instance_id_, context, callback});
        }
        if (out_token) {
            *out_token = token;
        }
        return S_OK;
    }

    bool STDMETHODCALLTYPE UnregisterCallback(KxGameInputCallbackToken token) override {
        if (token == 0) return false;
        std::lock_guard lock(g_state_mtx);
        auto it = std::find_if(
            g_callbacks.begin(), g_callbacks.end(),
            [this, token](const CallbackEntry& c) { return c.token == token && c.owner_id == instance_id_; });
        if (it == g_callbacks.end()) return false;
        g_callbacks.erase(it);
        return true;
    }

private:
    std::atomic<ULONG> ref_{1};
    uint64_t instance_id_ = 0;
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
    std::lock_guard lock(g_state_mtx);
    g_mock_enabled = true;
    g_mock_connected = connected;
    g_mock_state = state;
    if (!g_slots[0].mock_enabled) {
        g_slots[0].last_connected = connected;
    }
    recompute_active_controllers_locked();
}

void clear_gameinput_mock_state() {
    std::lock_guard lock(g_state_mtx);
    g_mock_enabled = false;
    g_mock_connected = false;
    g_mock_state = {};
    if (!g_slots[0].mock_enabled) {
        g_slots[0].last_connected = g_slots[0].hw_connected;
    }
    recompute_active_controllers_locked();
}

void set_gameinput_mock_slot_state(uint32_t slot, const KxGameInputGamepadState& state, bool connected) {
    if (slot >= kMaxControllerSlots) return;
    std::vector<CallbackEntry> snapshot;
    bool notify = false;
    bool prev = false;
    {
        std::lock_guard lock(g_state_mtx);
        prev = g_slots[slot].last_connected;
        g_slots[slot].mock_enabled = true;
        g_slots[slot].mock_state = state;
        g_slots[slot].mock_connected = connected;
        g_slots[slot].last_connected = connected;
        recompute_active_controllers_locked();
        if (prev != connected) {
            g_hotplug_events.fetch_add(1, std::memory_order_relaxed);
            snapshot = g_callbacks;
            notify = true;
        }
    }
    if (notify) {
        dispatch_hotplug_callbacks(slot, connected, prev, std::move(snapshot));
    }
}

void simulate_gameinput_hotplug(uint32_t slot, bool connected) {
    if (slot >= kMaxControllerSlots) return;
    std::vector<CallbackEntry> snapshot;
    bool notify = false;
    bool prev = false;
    {
        std::lock_guard lock(g_state_mtx);
        prev = g_slots[slot].last_connected;
        g_slots[slot].mock_enabled = true;
        g_slots[slot].mock_connected = connected;
        g_slots[slot].last_connected = connected;
        recompute_active_controllers_locked();
        if (prev != connected) {
            g_hotplug_events.fetch_add(1, std::memory_order_relaxed);
            snapshot = g_callbacks;
            notify = true;
        }
    }
    if (notify) {
        dispatch_hotplug_callbacks(slot, connected, prev, std::move(snapshot));
    }
}

void clear_gameinput_mock_slots() {
    std::lock_guard lock(g_state_mtx);
    for (uint32_t i = 0; i < kMaxControllerSlots; ++i) {
        g_slots[i].mock_enabled = false;
        g_slots[i].mock_connected = false;
        g_slots[i].mock_state = {};
        g_slots[i].last_connected = effective_connected_locked(i);
    }
    recompute_active_controllers_locked();
}

void register_gameinput(Registry& reg) {
    reg.register_module("gameinput.dll");
    reg.register_export("gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&kx_GameInputCreate));
    reg.register_export("xgameruntime.dll", "GameInputCreate", reinterpret_cast<void*>(&kx_GameInputCreate));
}

GameInputStats gameinput_stats() {
    return {
        g_instances.load(std::memory_order_relaxed),
        g_readings.load(std::memory_order_relaxed),
        g_rumble_commands.load(std::memory_order_relaxed),
        g_active_controllers.load(std::memory_order_relaxed),
        g_hotplug_events.load(std::memory_order_relaxed),
    };
}

void reset_gameinput_stats() {
    g_instances = 0;
    g_readings = 0;
    g_rumble_commands = 0;
    g_active_controllers = 0;
    g_hotplug_events = 0;
    std::lock_guard lock(g_state_mtx);
    g_mock_enabled = false;
    g_mock_connected = false;
    g_mock_state = {};
    g_slots.fill(ControllerSlot{});
}

}  // namespace kestrel::hle
