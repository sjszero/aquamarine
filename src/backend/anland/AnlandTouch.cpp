// src/backend/anland/AnlandTouch.cpp
#include "AnlandTouch.hpp"
#include "AnlandBackend.hpp"
#include <chrono>

namespace Aquamarine {

static uint32_t getCurrentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

CAnlandTouch::CAnlandTouch(CAnlandBackend* backend) : m_backend(backend) {}
CAnlandTouch::~CAnlandTouch() = default;

void CAnlandTouch::emitDown(uint32_t timeMs, uint32_t id, const Hyprutils::Math::Vector2D& pos) {
    m_touches[id] = pos;
    ITouch::SDownEvent ev;
    ev.timeMs = timeMs;
    ev.touchID = static_cast<int32_t>(id);
    ev.pos = pos;
    events.down.emit(ev);
}

void CAnlandTouch::emitUp(uint32_t timeMs, uint32_t id) {
    m_touches.erase(id);
    ITouch::SUpEvent ev;
    ev.timeMs = timeMs;
    ev.touchID = static_cast<int32_t>(id);
    events.up.emit(ev);
}

void CAnlandTouch::emitMotion(uint32_t timeMs, uint32_t id, const Hyprutils::Math::Vector2D& pos) {
    m_touches[id] = pos;
    ITouch::SMotionEvent ev;
    ev.timeMs = timeMs;
    ev.touchID = static_cast<int32_t>(id);
    ev.pos = pos;
    events.move.emit(ev);
}

void CAnlandTouch::emitFrame() {
    events.frame.emit();
}

void CAnlandTouch::emitCancel() {
    ITouch::SCancelEvent ev;
    ev.timeMs = getCurrentTimeMs();
    ev.touchID = -1;
    events.cancel.emit(ev);
    m_touches.clear();
}

} // namespace Aquamarine