// src/backend/anland/AnlandKeyboard.cpp
#include "AnlandKeyboard.hpp"
#include "AnlandBackend.hpp"
#include <chrono>

namespace Aquamarine {

static uint32_t getCurrentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

CAnlandKeyboard::CAnlandKeyboard(CAnlandBackend* backend) : m_backend(backend) {}
CAnlandKeyboard::~CAnlandKeyboard() = default;

void CAnlandKeyboard::emitKey(uint32_t timeMs, uint32_t keycode, bool pressed) {
    IKeyboard::SKeyEvent ev;
    ev.timeMs = timeMs;
    ev.key = keycode;
    ev.pressed = pressed;
    events.key.emit(ev);
}

void CAnlandKeyboard::emitModifiers(uint32_t depressed, uint32_t latched,
                                    uint32_t locked, uint32_t group) {
    IKeyboard::SModifiersEvent ev;
    ev.depressed = depressed;
    ev.latched = latched;
    ev.locked = locked;
    ev.group = group;
    events.modifiers.emit(ev);
}

} // namespace Aquamarine