// src/backend/anland/AnlandPointer.hpp
#ifndef AQUAMARINE_ANLAND_POINTER_HPP
#define AQUAMARINE_ANLAND_POINTER_HPP

#include <aquamarine/input/Input.hpp>
#include <string>

namespace Aquamarine {

class CAnlandBackend;

class CAnlandPointer : public IPointer {
public:
    explicit CAnlandPointer(CAnlandBackend* backend);
    virtual ~CAnlandPointer();

    virtual const std::string& getName() override { return m_name; }
    virtual libinput_device* getLibinputHandle() override { return nullptr; }

    void emitMotion(uint32_t timeMs, const Hyprutils::Math::Vector2D& delta);
    void emitWarp(uint32_t timeMs, const Hyprutils::Math::Vector2D& absolute);
    void emitButton(uint32_t timeMs, uint32_t button, bool pressed);
    void emitAxis(uint32_t timeMs, uint32_t axis, double value);
    void emitFrame();

private:
    CAnlandBackend* m_backend = nullptr;
    std::string m_name = "Anland Pointer";
};

} // namespace Aquamarine

#endif