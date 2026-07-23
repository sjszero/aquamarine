// src/backend/anland/AnlandOutput.hpp
#ifndef AQUAMARINE_ANLAND_OUTPUT_HPP
#define AQUAMARINE_ANLAND_OUTPUT_HPP

#include <aquamarine/output/Output.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#include <hyprutils/math/Region.hpp>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

extern "C" {
#include "display_producer.h"
}

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;

class CAnlandBackend;
class CAnlandDmaBuffer;

class CAnlandOutput : public IOutput {
public:
    explicit CAnlandOutput(CAnlandBackend* backend);
    virtual ~CAnlandOutput();

    // IOutput
    virtual bool commit() override;
    virtual bool test() override;
    virtual CSharedPointer<IBackendImplementation> getBackend() override;
    virtual std::vector<SDRMFormat> getRenderFormats() override;
    virtual bool pendingPageFlip() override { return m_framePending; }
    virtual void scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN) override;
    virtual size_t getGammaSize() override { return 256; }
    virtual size_t getDeGammaSize() override { return 0; }
    virtual bool destroy() override { return false; }

    // Cursor
    virtual bool setCursor(CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) override { return false; }
    virtual void moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule = false) override {}
    virtual void setCursorVisible(bool visible) override {}
    virtual Hyprutils::Math::Vector2D cursorPlaneSize() override { return {-1, -1}; }

    // Anland specific
    bool initialize(uint32_t width, uint32_t height, uint32_t refresh);
    void releaseBuffers();
    void updateRefreshRate(uint32_t refresh);
    void enterFallback();
    void exitFallback();
    bool isInFallback() const { return m_inFallback; }
    void onBufferReady();

    // Buffer management
    int getBufferCount() const { return m_bufferCount; }
    CSharedPointer<CAnlandDmaBuffer> getBuffer(int index) const;
    CSharedPointer<CBackend> getCBackend() const;

    // EGL context management
    void setEGL(EGLDisplay dpy, EGLContext ctx);
    EGLDisplay getEGLDisplay() const { return m_eglDisplay; }
    EGLContext getEGLContext() const { return m_eglContext; }
    void setImageDescription(void* desc) { m_imageDescription = desc; }
    void* getImageDescription() const { return m_imageDescription; }

    display_ctx* display();
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    uint32_t getDrmFormat() const { return m_drmFormat; }

private:
    bool importBuffer(int index);
    void destroyBuffer(int index);
    void importBuffers();
    void updateMode(uint32_t width, uint32_t height, uint32_t format);

    // Damage tracking
    struct BufferSlot {
        int fd = -1;
        uint32_t width = 0, height = 0;
        uint32_t format = 0;
        uint64_t modifier = 0;
        uint32_t offset = 0;
        uint32_t stride = 0;

        CSharedPointer<CAnlandDmaBuffer> buffer;
        bool imported = false;
        bool inUse = false;
        bool hasDamage = true;
        Hyprutils::Math::CRegion accumDamage;
    };

    std::array<BufferSlot, MAX_BUFS> m_slots;
    int m_bufferCount = 0;
    int m_selectedIndex = 0;

    bool m_inFallback = true;
    bool m_outputReady = false;
    bool m_buffersImported = false;
    bool m_shouldTriggerRefresh = false;
    std::atomic<bool> m_framePending{false};
    std::atomic<bool> m_commitInProgress{false};

    bool m_frameScheduled = false;
    CSharedPointer<std::function<void(void)>> m_frameIdle;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_refresh = 60000;
    uint32_t m_drmFormat = DRM_FORMAT_XRGB8888;

    // EGL context
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_eglContext = EGL_NO_CONTEXT;

    void* m_imageDescription = nullptr;

    mutable std::mutex m_bufferMutex;
    std::atomic<bool> m_destroying{false};
    std::atomic<bool> m_shutdownDone{false};

    CAnlandBackend* m_backend = nullptr;
};

} // namespace Aquamarine

#endif