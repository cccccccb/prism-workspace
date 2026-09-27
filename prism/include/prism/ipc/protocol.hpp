#pragma once

#include "prism/core/types.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace prism::ipc {

constexpr uint32_t SHM_MAGIC = 0x50524953; // "PRIS" (Prism)
constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr size_t RING_BUFFER_CAPACITY = 2048; // Must be power of 2
constexpr size_t INLINE_STRING_SIZE = 64;

enum class DiffOp : uint8_t {
    SetString = 0x01,
    SetInt64 = 0x02,
    SetFloat = 0x03,
    SetBool = 0x04,
    TriggerAnim = 0x05,
    SignalReady = 0x06,
    AppExit = 0x07
};

#pragma pack(push, 1)

struct StateDiffPacket {
    uint32_t slot_id;    // Hash of target DSL slot ($slot)
    DiffOp op;           // Operation type
    uint8_t payload_len; // String length if applicable
    uint16_t reserved;
    core::Timestamp timestamp_us;

    union {
        int64_t i64;
        double f64;
        bool b;
        char str[INLINE_STRING_SIZE];
    } value;

    static StateDiffPacket MakeString(uint32_t slot, std::string_view s)
    {
        StateDiffPacket pkt{};
        pkt.slot_id = slot;
        pkt.op = DiffOp::SetString;
        pkt.timestamp_us = core::CurrentTimeUs();
        size_t len = std::min(s.size(), INLINE_STRING_SIZE - 1);
        pkt.payload_len = static_cast<uint8_t>(len);
        std::memcpy(pkt.value.str, s.data(), len);
        pkt.value.str[len] = '\0';
        return pkt;
    }

    static StateDiffPacket MakeInt(uint32_t slot, int64_t val)
    {
        StateDiffPacket pkt{};
        pkt.slot_id = slot;
        pkt.op = DiffOp::SetInt64;
        pkt.timestamp_us = core::CurrentTimeUs();
        pkt.value.i64 = val;
        return pkt;
    }

    static StateDiffPacket MakeFloat(uint32_t slot, double val)
    {
        StateDiffPacket pkt{};
        pkt.slot_id = slot;
        pkt.op = DiffOp::SetFloat;
        pkt.timestamp_us = core::CurrentTimeUs();
        pkt.value.f64 = val;
        return pkt;
    }

    static StateDiffPacket MakeBool(uint32_t slot, bool val)
    {
        StateDiffPacket pkt{};
        pkt.slot_id = slot;
        pkt.op = DiffOp::SetBool;
        pkt.timestamp_us = core::CurrentTimeUs();
        pkt.value.b = val;
        return pkt;
    }

    static StateDiffPacket MakeReady()
    {
        StateDiffPacket pkt{};
        pkt.op = DiffOp::SignalReady;
        pkt.timestamp_us = core::CurrentTimeUs();
        return pkt;
    }

    static StateDiffPacket MakeExit()
    {
        StateDiffPacket pkt{};
        pkt.op = DiffOp::AppExit;
        pkt.timestamp_us = core::CurrentTimeUs();
        return pkt;
    }
};

enum class EventType : uint8_t {
    Click = 0x01,
    PointerMove = 0x02,
    PointerDown = 0x03,
    PointerUp = 0x04,
    KeyDown = 0x05,
    CustomAction = 0x06,
    WindowResize = 0x07,
    WindowClose = 0x08
};

struct EventPacket {
    EventType type;
    char action[48]; // UI action name (e.g. "player:toggle")
    core::Timestamp timestamp_us;

    union {
        struct {
            float x;
            float y;
        } pointer;

        struct {
            uint32_t keycode;
            uint32_t modifiers;
        } key;

        struct {
            uint32_t width;
            uint32_t height;
        } size;

        double custom_val;
    } data;

    static EventPacket MakeAction(std::string_view act)
    {
        EventPacket pkt{};
        pkt.type = EventType::CustomAction;
        pkt.timestamp_us = core::CurrentTimeUs();
        size_t len = std::min(act.size(), sizeof(pkt.action) - 1);
        std::memcpy(pkt.action, act.data(), len);
        pkt.action[len] = '\0';
        return pkt;
    }
};

#pragma pack(pop)

struct ShmHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t client_pid;
    uint32_t wm_pid;
    std::atomic<bool> is_connected;
    std::atomic<bool> is_master_ready;
};

} // namespace prism::ipc
