// src/backend/anland/AnlandOutput.hpp
#ifndef AQUAMARINE_ANLAND_OUTPUT_HPP
#define AQUAMARINE_ANLAND_OUTPUT_HPP

#include <aquamarine/output/Output.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

extern "C" {
#include "display_producer.h"
}

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;

class CAnlandBackend;

class CAnlandOutput : public IOutput {
public:
    explicit CAnlandOutput(CAnlandBackend* backend);
    virtual ~CAnlandOutput();

    // IOutput 实现 - 使用标准 swapchain
    virtual bool commit() override;
    virtual bool test() override;
    virtual CSharedPointer<IBackendImplementation> getBackend() override;
    virtual std::vector<SDRMFormat> getRenderFormats() override;
    virtual bool pendingPageFlip() override { return m_framePending; }
    virtual void scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN) override;
    virtual size_t getGammaSize() override { return 0; }
    virtual size_t getDeGammaSize() override { return 0; }
    virtual bool destroy() override { return false; }

    // Cursor (不支持硬件光标)
    virtual bool setCursor(CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) override { return false; }
    virtual void moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule = false) override {}
    virtual void setCursorVisible(bool visible) override {}
    virtual Hyprutils::Math::Vector2D cursorPlaneSize() override { return {-1, -1}; }

    // Anland 特有 API
    bool initialize(uint32_t width, uint32_t height, uint32_t refresh);
    void releaseBuffers();
    void updateRefreshRate(uint32_t refresh);

    // 使用标准 swapchain
    void reconfigureSwapchain();

    // Fallback 管理
    void enterFallback();
    void exitFallback();
    bool isInFallback() const { return m_inFallback; }

    // 事件处理
    void onBufferReady();

    // 查询
    display_ctx* display();
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    CAnlandBackend* m_backend = nullptr;

    // 标准 swapchain
    CSharedPointer<CSwapchain> m_swapchain;

    // 状态
    bool m_inFallback = true;
    bool m_outputReady = false;
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

    // 线程安全
    mutable std::mutex m_bufferMutex;

    // 生命周期
    std::atomic<bool> m_destroying{false};
    std::atomic<bool> m_shutdownDone{false};
};

} // namespace Aquamarine

#endif