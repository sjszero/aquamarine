// src/backend/anland/AnlandPointer.cpp
#include "AnlandPointer.hpp"
#include "AnlandBackend.hpp"
#include <chrono>

namespace Aquamarine {

static uint32_t getCurrentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

CAnlandPointer::CAnlandPointer(CAnlandBackend* backend) : m_backend(backend) {}
CAnlandPointer::~CAnlandPointer() = default;

void CAnlandPointer::emitMotion(uint32_t timeMs, const Hyprutils::Math::Vector2D& delta) {
    IPointer::SMoveEvent ev;
    ev.timeMs = timeMs;
    ev.delta = delta;
    ev.unaccel = delta;
    events.move.emit(ev);
}

void CAnlandPointer::emitButton(uint32_t timeMs, uint32_t button, bool pressed) {
    IPointer::SButtonEvent ev;
    ev.timeMs = timeMs;
    ev.button = button;
    ev.pressed = pressed;
    events.button.emit(ev);
}

void CAnlandPointer::emitAxis(uint32_t timeMs, uint32_t axis, double value) {
    IPointer::SAxisEvent ev;
    ev.timeMs = timeMs;
    ev.axis = static_cast<IPointer::ePointerAxis>(axis);
    ev.delta = value;
    ev.source = IPointer::AQ_POINTER_AXIS_SOURCE_CONTINUOUS;
    events.axis.emit(ev);
}

void CAnlandPointer::emitFrame() {
    events.frame.emit();
}

} // namespace Aquamarine