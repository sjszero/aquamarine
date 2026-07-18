// src/backend/anland/AnlandBackend.hpp
#ifndef AQUAMARINE_ANLAND_BACKEND_HPP
#define AQUAMARINE_ANLAND_BACKEND_HPP

#include <aquamarine/backend/Backend.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "display_producer.h"
#include "anland_audio.h"
#include "anland_camera.h"
}

namespace Aquamarine {

class CAnlandOutput;
class CAnlandPointer;
class CAnlandKeyboard;
class CAnlandTouch;

/**
 * AnlandBackend - Android display backend for Aquamarine
 *
 * Communicates with Android's display_daemon via UNIX socket,
 * receives pre-allocated dmabufs, input events, audio and camera streams.
 *
 * Design principles:
 * - Zero-copy rendering: renders directly into consumer-provided dmabufs
 * - Hot-plug support: fallback/reconnect state machine
 * - EGL context sharing: reuses Hyprland's EGL context
 * - Per-buffer damage tracking: efficient partial updates
 */
class CAnlandBackend : public IBackendImplementation {
public:
    CAnlandBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend,
                   const std::string& socketPath = "/run/display.sock");
    virtual ~CAnlandBackend();

    // IBackendImplementation
    virtual eBackendType type() override { return AQ_BACKEND_ANLAND; }
    virtual bool start() override;
    virtual std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> pollFDs() override;
    virtual int drmFD() override { return -1; }
    virtual int drmRenderNodeFD() override { return -1; }
    virtual bool dispatchEvents() override;
    virtual uint32_t capabilities() override { return AQ_BACKEND_CAPABILITY_POINTER; }
    virtual void onReady() override;
    virtual std::vector<SDRMFormat> getRenderFormats() override;
    virtual std::vector<SDRMFormat> getCursorFormats() override { return {}; }
    virtual bool createOutput(const std::string& name = "") override;
    virtual Hyprutils::Memory::CSharedPointer<IAllocator> preferredAllocator() override { return nullptr; }
    virtual std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>> getAllocators() override { return {}; }
    virtual Hyprutils::Memory::CWeakPointer<IBackendImplementation> getPrimary() override { return self; }

    // 状态查询
    display_ctx* display() { return m_display; }
    Hyprutils::Memory::CSharedPointer<CAnlandOutput> getOutput() const { return m_output; }
    bool isConnected() const { return m_display != nullptr && !m_inFallback; }
    bool isFallback() const { return m_inFallback; }

    // 生命周期
    void onFallback();
    void onOutputChanged();
    void shutdown();

    Hyprutils::Memory::CWeakPointer<CAnlandBackend> self;

private:
    // 重连机制
    void setupReconnectTimer();
    void teardownReconnectTimer();
    void onReconnectTimerFd();
    bool tryConnect();

    // 输出管理
    void createOutputIfNeeded();
    void emitOutputIfReady();

    // 输入事件处理
    void handleInputEvent(const InputEvent& ev);
    void handleResourceEvent(const InputEvent& ev);

    // 音频/摄像头
    void updateAudioFd();
    void updateCameraResources();

    Hyprutils::Memory::CSharedPointer<CBackend> m_backend;
    std::string m_socketPath;
    display_ctx* m_display = nullptr;

    Hyprutils::Memory::CSharedPointer<CAnlandOutput> m_output;
    Hyprutils::Memory::CSharedPointer<CAnlandPointer> m_pointer;
    Hyprutils::Memory::CSharedPointer<CAnlandKeyboard> m_keyboard;
    Hyprutils::Memory::CSharedPointer<CAnlandTouch> m_touch;

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
};

} // namespace Aquamarine

#endif // AQUAMARINE_ANLAND_BACKEND_HPP