// src/backend/anland/AnlandBackend.hpp
#ifndef AQUAMARINE_ANLAND_BACKEND_HPP
#define AQUAMARINE_ANLAND_BACKEND_HPP

#include <aquamarine/backend/Backend.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <functional>
#include <deque>
#include <chrono>

extern "C" {
#include "display_producer.h"
#include "anland_audio.h"
#include "anland_camera.h"
}

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::CWeakPointer;

class CAnlandOutput;
class CAnlandPointer;
class CAnlandKeyboard;
class CAnlandTouch;

/**
 * 剪贴板回调类型 - 由 Hyprland 端注册
 */
using ClipboardCallback = std::function<void(const std::string& text)>;

/**
 * 文本输入回调类型 - 由 Hyprland 端注册
 */
using TextInputCallback = std::function<void(const std::string& text)>;

/**
 * Anland backend for Hyprland (Aquamarine)
 *
 * Connects to Android display daemon via UNIX socket and provides:
 * - Display output (dmabuf-based zero-copy rendering)
 * - Input events (pointer, keyboard, touch)
 * - Audio (PipeWire virtual devices)
 * - Camera (PipeWire virtual devices)
 */
class CAnlandBackend : public IBackendImplementation {
public:
    CAnlandBackend(CSharedPointer<CBackend> backend,
                   const std::string& socketPath = "/run/display.sock");
    virtual ~CAnlandBackend();

    // IBackendImplementation
    virtual eBackendType type() override { return AQ_BACKEND_ANLAND; }
    virtual bool start() override;
    virtual std::vector<CSharedPointer<SPollFD>> pollFDs() override;
    virtual int drmFD() override { return m_dummyDRMFD; }
    virtual int drmRenderNodeFD() override { return m_dummyDRMFD; }
    virtual bool dispatchEvents() override;
    virtual uint32_t capabilities() override { return AQ_BACKEND_CAPABILITY_POINTER; }
    virtual void onReady() override;
    virtual std::vector<SDRMFormat> getRenderFormats() override;
    virtual std::vector<SDRMFormat> getCursorFormats() override { return {}; }
    virtual bool createOutput(const std::string& name = "") override;
    virtual CSharedPointer<IAllocator> preferredAllocator() override { return nullptr; }
    virtual std::vector<CSharedPointer<IAllocator>> getAllocators() override { return {}; }
    virtual CWeakPointer<IBackendImplementation> getPrimary() override { return self; }
    virtual std::vector<SDRMFormat> getRenderableFormats() override { return getRenderFormats(); }

    // Public accessors
    CSharedPointer<CBackend> getBackend() const { return m_backend; }
    display_ctx* display() { return m_display; }
    CSharedPointer<CAnlandOutput> getOutput() const { return m_output; }
    bool isConnected() const { return m_display != nullptr && !m_inFallback; }
    bool isFallback() const { return m_inFallback; }

    void onFallback();
    void enterFallback();
    void shutdown();

    // 注册回调函数（由 Hyprland 端调用）
    void setClipboardCallback(ClipboardCallback cb) { 
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_clipboardCallback = std::move(cb); 
    }
    void setTextInputCallback(TextInputCallback cb) { 
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_textInputCallback = std::move(cb); 
    }

    // IME 延迟重绘支持
    void deferFrameForIME();
    bool hasDeferredIME() const { return m_imeDeferred; }
    void clearDeferredIME() { m_imeDeferred = false; }

    // 获取当前时间 (ms)
    static uint32_t getCurrentTimeMs();

    CWeakPointer<CAnlandBackend> self;

private:
    void setupReconnectTimer();
    void teardownReconnectTimer();
    void onReconnectTimerFd();
    bool tryConnect();
    void createOutputIfNeeded();
    void emitOutputIfReady();
    void handleInputEvent(const InputEvent& ev);
    void handleResourceEvent(const InputEvent& ev);
    void updateAudioFd();
    void updateCameraResources();
    void updateClipboard(const InputEvent& ev);
    void updateTextInput(const InputEvent& ev);
    void processPointerMotion(const InputEvent& ev);
    void processPointerButton(const InputEvent& ev);
    void processPointerAxis(const InputEvent& ev);
    void processKey(const InputEvent& ev);
    void processTouch(const InputEvent& ev);
    void processDisplayRefresh(const InputEvent& ev);

    int openDummyDRM();

    CSharedPointer<CBackend> m_backend;
    std::string m_socketPath;
    display_ctx* m_display = nullptr;

    int m_dummyDRMFD = -1;

    CSharedPointer<CAnlandOutput> m_output;
    CSharedPointer<CAnlandPointer> m_pointer;
    CSharedPointer<CAnlandKeyboard> m_keyboard;
    CSharedPointer<CAnlandTouch> m_touch;

    bool m_running = false;
    bool m_inFallback = true;
    bool m_outputCreated = false;
    bool m_outputEmitted = false;
    bool m_hasDisplayInfo = false;
    std::mutex m_connectMutex;

    int m_reconnectTimerFd = -1;
    bool m_reconnectPending = false;

    uint32_t m_screenWidth = 0;
    uint32_t m_screenHeight = 0;
    uint32_t m_screenFormat = 0;
    uint32_t m_screenRefresh = 60000;

    std::atomic<bool> m_destroying{false};
    std::atomic<bool> m_shutdownDone{false};

    // 回调函数 - 由 Hyprland 端注册，带锁保护
    std::mutex m_callbackMutex;
    ClipboardCallback m_clipboardCallback;
    TextInputCallback m_textInputCallback;

    // 剪贴板去重
    std::string m_lastClipboardText;
    
    // IME 延迟重绘
    std::atomic<bool> m_imeDeferred{false};
    std::chrono::steady_clock::time_point m_imeDeferDeadline;
    CSharedPointer<CEventLoopTimer> m_imeDeferTimer;

    // Touch 状态跟踪 (用于手势识别)
    struct TouchPoint {
        int32_t id;
        Vector2D pos;
        bool active;
    };
    std::array<TouchPoint, 16> m_touchPoints;
    std::mutex m_touchMutex;
    
    // 触摸手势识别 (简化版三/四指滑动)
    struct GestureState {
        bool active = false;
        int fingers = 0;
        Vector2D startPos;
        Vector2D lastPos;
        Vector2D accumDelta;
        bool swipeMode = false;
    } m_gesture;
};

} // namespace Aquamarine

#endif