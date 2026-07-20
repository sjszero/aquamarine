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
};

} // namespace Aquamarine

#endif