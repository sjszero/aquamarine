// src/backend/anland/AnlandKeyboard.hpp
#ifndef AQUAMARINE_ANLAND_KEYBOARD_HPP
#define AQUAMARINE_ANLAND_KEYBOARD_HPP

#include <aquamarine/input/Input.hpp>
#include <string>

namespace Aquamarine {

class CAnlandBackend;

class CAnlandKeyboard : public IKeyboard {
public:
    explicit CAnlandKeyboard(CAnlandBackend* backend);
    virtual ~CAnlandKeyboard();

    virtual const std::string& getName() override { return m_name; }
    virtual libinput_device* getLibinputHandle() override { return nullptr; }

    void emitKey(uint32_t timeMs, uint32_t keycode, bool pressed);
    void emitModifiers(uint32_t depressed, uint32_t latched,
                       uint32_t locked, uint32_t group);

private:
    CAnlandBackend* m_backend = nullptr;
    std::string m_name = "Anland Keyboard";
};

} // namespace Aquamarine

#endif