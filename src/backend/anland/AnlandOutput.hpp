// src/backend/anland/AnlandOutput.hpp
#ifndef AQUAMARINE_ANLAND_OUTPUT_HPP
#define AQUAMARINE_ANLAND_OUTPUT_HPP

#include <aquamarine/output/Output.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
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

// 假装是标准 Output，实际上通过 display_producer 与 Android 通信
class CAnlandOutput : public IOutput {
public:
    explicit CAnlandOutput(CAnlandBackend* backend);
    virtual ~CAnlandOutput();

    // IOutput - 假装是标准 DRM output
    virtual bool commit() override;
    virtual bool test() override;
    virtual CSharedPointer<IBackendImplementation> getBackend() override;
    virtual std::vector<SDRMFormat> getRenderFormats() override;
    virtual bool pendingPageFlip() override { return m_framePending; }
    virtual void scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN) override;
    virtual size_t getGammaSize() override { return 0; }
    virtual size_t getDeGammaSize() override { return 0; }
    virtual bool destroy() override { return false; }

    // 假装支持硬件光标（实际不支持）
    virtual bool setCursor(CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) override { return false; }
    virtual void moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule = false) override {}
    virtual void setCursorVisible(bool visible) override {}
    virtual Hyprutils::Math::Vector2D cursorPlaneSize() override { return {-1, -1}; }

    // Anland 特有
    bool initialize(uint32_t width, uint32_t height, uint32_t refresh);
    void releaseBuffers();
    void updateRefreshRate(uint32_t refresh);
    void reconfigureSwapchain();
    void enterFallback();
    void exitFallback();
    bool isInFallback() const { return m_inFallback; }
    void onBufferReady();

    display_ctx* display();
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    CAnlandBackend* m_backend = nullptr;

    // 标准 swapchain - 假装是 GBM swapchain
    CSharedPointer<CSwapchain> m_swapchain;

    bool m_inFallback = true;
    bool m_outputReady = false;
    std::atomic<bool> m_needsFrame{false};
    std::atomic<bool> m_framePending{false};
    std::atomic<bool> m_commitInProgress{false};

    bool m_frameScheduled = false;
    CSharedPointer<std::function<void(void)>> m_frameIdle;

    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    uint32_t m_refresh = 60000;

    mutable std::mutex m_bufferMutex;

    std::atomic<bool> m_destroying{false};
    std::atomic<bool> m_shutdownDone{false};
};

} // namespace Aquamarine

#endif