// src/backend/anland/AnlandTouch.hpp
#ifndef AQUAMARINE_ANLAND_TOUCH_HPP
#define AQUAMARINE_ANLAND_TOUCH_HPP

#include <aquamarine/input/Input.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <string>
#include <unordered_map>

namespace Aquamarine {

class CAnlandBackend;

class CAnlandTouch : public ITouch {
public:
    explicit CAnlandTouch(CAnlandBackend* backend);
    virtual ~CAnlandTouch();

    virtual const std::string& getName() override { return m_name; }
    virtual libinput_device* getLibinputHandle() override { return nullptr; }

    void emitDown(uint32_t timeMs, uint32_t id, const Hyprutils::Math::Vector2D& pos);
    void emitUp(uint32_t timeMs, uint32_t id);
    void emitMotion(uint32_t timeMs, uint32_t id, const Hyprutils::Math::Vector2D& pos);
    void emitFrame();
    void emitCancel();

private:
    CAnlandBackend* m_backend = nullptr;
    std::string m_name = "Anland Touch";
    std::unordered_map<uint32_t, Hyprutils::Math::Vector2D> m_touches;
};

} // namespace Aquamarine

#endif // AQUAMARINE_ANLAND_TOUCH_HPP