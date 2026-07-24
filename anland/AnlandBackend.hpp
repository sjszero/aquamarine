// src/backend/anland/AnlandBackend.hpp
#ifndef AQUAMARINE_ANLAND_BACKEND_HPP
#define AQUAMARINE_ANLAND_BACKEND_HPP

#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <functional>
#include <deque>
#include <chrono>
#include <optional>
#include <cstdint>

// 前向声明 CEventLoopTimer（Hyprland 类型）
class CEventLoopTimer;

extern "C" {
#include "display_producer.h"
#include "anland_audio.h"
#include "anland_camera.h"
}

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::CWeakPointer;
using Hyprutils::Math::Vector2D;

class CAnlandOutput;
class CAnlandPointer;
class CAnlandKeyboard;
class CAnlandTouch;

/**
 * 剪贴板回调类垉 - 由 CHackend \n后吗 CEnventLoopTimer（HYPRLAND翻车）
 */
using ClipboardCallback = std::function<void(const std::string& text)>;

/**
 * 文本输入回调类型 - 由 Hyprland \n类型调用
 */
using TextInputCallback = std::function<void(const std::string& text)>;

/// DMA-BUF 缓存秽位数量（参照 niri 版 8-slot 设计）
constexpr size_t ANLAND_DMABUF_CACHE_SLOTS = 8;

/**
 * Anland backend for Hyprland (Aquamarine) — 优化版
 *
 * 参照 kwin 版和 niri 版参考实现的优点：
 * - DMA-BUF 缓存机制（niri ่�വ൳േൾൌസ cache）
 * - 帧同步（Fence）增强（GPU-side native fence + blocking fallback）
 * - pending_frame 渲染门控防止空转
 * - IME 延迟重绘优化（2ms grace windou）
 * - 触摸状态管理和长按手势增强
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

    // IME 延迟重绘支持（优化版：niri 风格 2ms grace + ANLAND_LAT 追踪）
    void deferFrameForIME();
    bool hasDeferredIME() const { return m_imeDeferred; }
    void clearDeferredIME() { m_imeDeferred = false; }

    // 渲染优化：标记待处理帧（由 onBufferReady 设置）
    void markPendingFrame() { m_pendingFrame = true; }
    bool hasPendingFrame() const { return m_pendingFrame; }

    // 帧同步优化：导出 native fence fd 并设置到 consumer
    bool exportRenderFence(int fenceFd);

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

    // 新增：渲染优化方法
    void evictDmabufCache();
    int  createDupFd(int origFd);

    int openDummyDRM();

    CSharedPointer<CBackend> m_backend;
    std::string m_socketPath;
    display_ctx* m_display = nullptr;

    int m_dummyDRMFD = -1;
    int m_gbmDevice = -1; // GBM 设备 fd（可放了 dmabuf cache 的 fd dup）

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
    
    // 音频 / 相机 fd
    int m_audioFd = -1;
    
    // ========== 渲染优化：DMA-BUF 缓存（参照 niri 版 8-slot） ==========
    struct DmabufCacheEntry {
        bool     valid = false;
        int      fd = -1;           // dup 后的 dmabuf fd
        uint32_t format = 0;
        uint64_t modifier = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        int      selectedIdx = -1;  // 对应的 buffer 索引
    };
    std::array<DmabufCacheEntry, ANLAND_DMABUF_CACHE_SLOTS> m_dmabufCache;
    std::mutex m_dmabufCacheMutex;
    
    // ========== 渲染优化：pending_frame 门控（参照 niri） ==========
    // 标记是否有待处理的帧需要渲染。仅在 consumer 发来 buffer-ready 信号时置 true，
    // render() 消费后置 false，防止 Aquamarine 的再入式 while 循环空转。
    std::atomic<bool> m_pendingFrame{false};
    
    // ========== IME 延迟重绘优化 ==========
    std::atomic<bool> m_imeDeferred{false};
    std::chrono::steady_clock::time_point m_imeDeferDeadline;
    CSharedPointer<std::function<void()>> m_imeDeferCallback;

    // ========== Touch 状态跟踪（增强版：支持多点触摸手势识别） ==========
    struct TouchPoint {
        int32_t id;
        Vector2D pos;
        bool active;
        std::chrono::steady_clock::time_point downTime; // 用于长按检测
    };
    static constexpr int MAX_TOUCH_POINTS = 16;
    std::array<TouchPoint, MAX_TOUCH_POINTS> m_touchPoints;
    std::mutex m_touchMutex;
    
    // 触摸手势识别（参照 niri 版三/四指滑动 + 长按右键）
    struct GestureState {
        bool active = false;
        int fingers = 0;
        Vector2D startPos;
        Vector2D lastPos;
        Vector2D accumDelta;
        bool swipeMode = false;
        std::chrono::steady_clock::time_point gestureStartTime;
    } m_gesture;
    
    // 长按检测（600ms 后触发右键，参照 niri）
    struct LongPressState {
        bool active = false;
        int touchId = -1;
        Vector2D pos;
        std::chrono::steady_clock::time_point startTime;
        bool triggered = false;
    } m_longPress;

    // 帧完成通知 consumer 用 eventfd
    int m_frameCompletionFd = -1;
};

} // namespace Aquamarine

#endif