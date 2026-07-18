// src/backend/anland/AnlandOutput.hpp
#ifndef AQUAMARINE_ANLAND_OUTPUT_HPP
#define AQUAMARINE_ANLAND_OUTPUT_HPP

#include <aquamarine/output/Output.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Region.hpp>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#ifndef GL_OES_EGL_image
typedef void* GLeglImageOES;
#endif

typedef void (GL_APIENTRY* PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);

extern "C" {
#include "display_producer.h"
}

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;

class CAnlandBackend;
class CAnlandBuffer;

/**
 * 缓冲区槽位 - 管理单个 dmabuf 的所有资源
 */
struct BufferSlot {
    // dmabuf 信息
    int fd = -1;
    uint32_t width = 0, height = 0;
    uint32_t format = 0;
    uint64_t modifier = 0;
    uint32_t offset = 0;
    uint32_t stride = 0;

    // EGL/GL 资源
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    GLuint framebuffer = 0;

    // 缓冲区对象
    CSharedPointer<CAnlandBuffer> buffer;

    // 状态标志
    bool imported = false;
    bool inUse = false;
    bool rendered = false;
    bool displayed = false;
    bool failed = false;

    // 损伤区域跟踪
    Hyprutils::Math::CRegion accumDamage;
    bool hasDamage = true;
};

/**
 * AnlandOutput - 虚拟显示输出
 */
class CAnlandOutput : public IOutput {
public:
    explicit CAnlandOutput(CAnlandBackend* backend);
    virtual ~CAnlandOutput();

    // IOutput 实现
    virtual bool commit() override;
    virtual bool test() override;
    virtual CSharedPointer<IBackendImplementation> getBackend() override;
    virtual std::vector<SDRMFormat> getRenderFormats() override;
    virtual bool pendingPageFlip() override { return m_framePending; }
    virtual void scheduleFrame(scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN) override;
    virtual size_t getGammaSize() override { return 0; }
    virtual size_t getDeGammaSize() override { return 0; }
    virtual bool destroy() override { return false; }

    // 光标（Anland 不支持硬件光标）
    virtual bool setCursor(CSharedPointer<IBuffer> buffer,
                           const Hyprutils::Math::Vector2D& hotspot) override { return false; }
    virtual void moveCursor(const Hyprutils::Math::Vector2D& coord,
                            bool skipSchedule = false) override {}
    virtual void setCursorVisible(bool visible) override {}
    virtual Hyprutils::Math::Vector2D cursorPlaneSize() override { return {-1, -1}; }

    // Anland 特有 API
    bool initialize(uint32_t width, uint32_t height, uint32_t refresh);
    void importBuffers(int count);
    void releaseBuffers();
    void updateRefreshRate(uint32_t refresh);

    // 渲染管理
    bool beginRender();
    void endRender();

    // 事件处理
    void onBufferReady();
    void onInputReady();

    // Fallback 管理
    void enterFallback();
    void exitFallback();
    bool isInFallback() const { return m_inFallback; }

    // 查询
    GLuint getCurrentFramebuffer() const;
    GLuint getCurrentTexture() const;
    int getCurrentBufferIndex() const { return m_selectedIndex; }
    CSharedPointer<IBuffer> getCurrentBuffer();

    void setRenderFence(int fenceFd);
    void completeFrame();

    display_ctx* display();
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    // 内部方法
    bool importBuffer(int index);
    void destroyBuffer(int index);
    uint32_t protocolFormatToDrm(uint32_t fmt) const;
    void selectNextBuffer();

    // 损伤区域处理
    void markDamage(int index, const Hyprutils::Math::CRegion& damage);
    void clearDamage(int index);

    // EGL 初始化
    bool ensureEGLInitialized();

    // 成员变量
    CAnlandBackend* m_backend = nullptr;

    // 缓冲区槽位
    std::array<BufferSlot, MAX_BUFS> m_slots;
    int m_bufferCount = 0;
    int m_selectedIndex = 0;
    int m_frontIndex = 0;
    int m_backIndex = 0;

    // EGL 状态
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    bool m_eglInitialized = false;

    // 状态
    bool m_inFallback = true;
    bool m_outputReady = false;
    bool m_buffersImported = false;
    std::atomic<bool> m_needsFrame{false};
    std::atomic<bool> m_framePending{false};
    std::atomic<bool> m_commitInProgress{false};

    // 帧调度
    bool m_frameScheduled = false;
    CSharedPointer<std::function<void(void)>> m_frameIdle;

    // 尺寸
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    uint32_t m_refresh = 60000;

    // Fence
    int m_renderFenceFd = -1;

    // 线程安全
    mutable std::mutex m_bufferMutex;

    // 生命周期
    std::atomic<bool> m_destroying{false};
    std::atomic<bool> m_shutdownDone{false};
};

} // namespace Aquamarine

#endif // AQUAMARINE_ANLAND_OUTPUT_HPP