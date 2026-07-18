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

class CAnlandBackend;
class CAnlandBuffer;

/**
 * 缓冲区槽位 - 管理单个 dmabuf 的所有资源
 *
 * 借鉴 KWin 的 AnlandEglLayer 设计：
 * - 每个缓冲区独立管理 EGLImage、GL 纹理和 FBO
 * - 记录损伤区域，支持 buffer-age 优化
 */
struct BufferSlot {
    // dmabuf 信息
    int fd = -1;
    uint32_t width = 0, height = 0;
    uint32_t format = 0;       // 消费者端格式
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
    bool imported = false;      // 是否已导入
    bool inUse = false;         // 是否正在使用
    bool rendered = false;      // 是否已渲染
    bool displayed = false;     // 是否已显示
    bool failed = false;        // 导入是否失败

    // 损伤区域跟踪（借鉴 KWin 的 m_accumDamage）
    Hyprutils::Math::CRegion accumDamage;
    bool hasDamage = true;
};

/**
 * AnlandOutput - 虚拟显示输出
 *
 * 直接管理消费者的 dmabuf 缓冲区，渲染到 EGLImage/FBO，
 * 并触发 buffer-ready 事件驱动帧同步。
 *
 * 设计亮点（借鉴 KWin）：
 * 1. Per-buffer 损伤区域：m_slots[i].accumDamage
 * 2. 帧同步：scheduleFrame() + idle 事件
 * 3. 热插拔：enterFallback() / exitFallback()
 * 4. 多缓冲支持：最多 8 个槽位
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
    virtual bool pendingIdleFrame() override { return m_needsFrame || m_frameScheduled; }
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

    // 损伤区域处理（借鉴 KWin）
    void markDamage(int index, const Hyprutils::Math::CRegion& damage);
    void clearDamage(int index);

    // EGL 初始化
    bool ensureEGLInitialized();

    // 成员变量
    CAnlandBackend* m_backend = nullptr;

    // 缓冲区槽位
    std::array<BufferSlot, MAX_BUFS> m_slots;
    int m_bufferCount = 0;
    int m_selectedIndex = 0;    // 当前渲染目标
    int m_frontIndex = 0;       // 当前显示目标
    int m_backIndex = 0;        // 下一个渲染目标

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
    Hyprutils::Memory::CSharedPointer<std::function<void(void)>> m_frameIdle;

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