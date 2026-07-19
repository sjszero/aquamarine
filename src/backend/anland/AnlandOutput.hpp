// src/backend/anland/AnlandOutput.hpp
#ifndef AQUAMARINE_ANLAND_OUTPUT_HPP
#define AQUAMARINE_ANLAND_OUTPUT_HPP

#include <aquamarine/output/Output.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#include <hyprutils/math/Region.hpp>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

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
    virtual size_t getGammaSize() override { return 0; }
    virtual size_t getDeGammaSize() override { return 0; }
    virtual bool destroy() override { return false; }

    // Cursor (不支持)
    virtual bool setCursor(CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) override { return false; }
    virtual void moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule = false) override {}
    virtual void setCursorVisible(bool visible) override {}
    virtual Hyprutils::Math::Vector2D cursorPlaneSize() override { return {-1, -1}; }

    // Anland 特有
    bool initialize(uint32_t width, uint32_t height, uint32_t refresh);
    void releaseBuffers();
    void updateRefreshRate(uint32_t refresh);
    void enterFallback();
    void exitFallback();
    bool isInFallback() const { return m_inFallback; }
    void onBufferReady();

    // 获取当前渲染目标
    GLuint getCurrentFramebuffer() const;
    GLuint getCurrentTexture() const;

    display_ctx* display();
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    bool importBuffer(int index);
    void destroyBuffer(int index);
    void importBuffers();

    bool ensureEGLInitialized();

    CAnlandBackend* m_backend = nullptr;

    struct BufferSlot {
        int fd = -1;
        uint32_t width = 0, height = 0;
        uint32_t format = 0;
        uint64_t modifier = 0;
        uint32_t offset = 0;
        uint32_t stride = 0;

        EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        GLuint framebuffer = 0;

        CSharedPointer<CAnlandDmaBuffer> buffer;
        bool imported = false;
        bool inUse = false;
        bool hasDamage = true;
    };

    std::array<BufferSlot, MAX_BUFS> m_slots;
    int m_bufferCount = 0;
    int m_selectedIndex = 0;

    bool m_inFallback = true;
    bool m_outputReady = false;
    bool m_buffersImported = false;
    bool m_firstCommit = true;           // 第一次 commit 跳过所有操作
    std::atomic<bool> m_framePending{false};
    std::atomic<bool> m_commitInProgress{false};

    bool m_frameScheduled = false;
    CSharedPointer<std::function<void(void)>> m_frameIdle;

    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    uint32_t m_refresh = 60000;

    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    bool m_eglInitialized = false;

    mutable std::mutex m_bufferMutex;
    std::atomic<bool> m_destroying{false};
    std::atomic<bool> m_shutdownDone{false};
};

} // namespace Aquamarine

#endif