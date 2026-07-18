// include/aquamarine/backend/Backend.hpp
#pragma once

#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/cli/Logger.hpp>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include "../allocator/Allocator.hpp"
#include "Misc.hpp"
#include "Session.hpp"

namespace Aquamarine {
    class CLogger;
    class IOutput;
    class IPointer;
    class IKeyboard;
    class ITouch;
    class ISwitch;
    class ITablet;
    class ITabletTool;
    class ITabletPad;

    /**
     * 后端类型枚举
     */
    enum eBackendType : uint32_t {
        AQ_BACKEND_WAYLAND = 0,
        AQ_BACKEND_DRM,
        AQ_BACKEND_HEADLESS,
        AQ_BACKEND_NULL,
        AQ_BACKEND_ANLAND,      // Anland (Android display) backend
    };

    /**
     * 后端请求模式
     */
    enum eBackendRequestMode : uint32_t {
        AQ_BACKEND_REQUEST_MANDATORY = 0,      // 必须存在，否则失败
        AQ_BACKEND_REQUEST_IF_AVAILABLE,        // 可用则启动
        AQ_BACKEND_REQUEST_FALLBACK,            // 作为后备
    };

    /**
     * 日志级别
     */
    enum eBackendLogLevel : uint32_t {
        AQ_LOG_TRACE = 0,
        AQ_LOG_DEBUG,
        AQ_LOG_WARNING,
        AQ_LOG_ERROR,
        AQ_LOG_CRITICAL,
    };

    /**
     * 后端实现选项
     */
    struct SBackendImplementationOptions {
        explicit SBackendImplementationOptions();
        eBackendType        backendType;
        eBackendRequestMode backendRequestMode;
    };

    /**
     * 后端选项
     */
    struct SBackendOptions {
        explicit SBackendOptions();
        std::function<void(eBackendLogLevel, std::string)>                   logFunction;
        Hyprutils::Memory::CSharedPointer<Hyprutils::CLI::CLoggerConnection> logConnection;
    };

    /**
     * poll FD 描述符
     */
    struct SPollFD {
        int                       fd = -1;
        std::function<void(void)> onSignal;  /* FD 可读时调用 */
    };

    /**
     * 后端实现接口
     */
    class IBackendImplementation {
      public:
        virtual ~IBackendImplementation() = default;

        enum eBackendCapabilities : uint32_t {
            AQ_BACKEND_CAPABILITY_POINTER = (1 << 0),
        };

        virtual eBackendType                                               type() = 0;
        virtual bool                                                       start() = 0;
        virtual std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>    pollFDs() = 0;
        virtual int                                                        drmFD() = 0;
        virtual bool                                                       dispatchEvents() = 0;
        virtual uint32_t                                                   capabilities() = 0;
        virtual void                                                       onReady() = 0;
        virtual std::vector<SDRMFormat>                                    getRenderFormats() = 0;
        virtual std::vector<SDRMFormat>                                    getCursorFormats() = 0;
        virtual bool                                                       createOutput(const std::string& name = "") = 0;
        virtual Hyprutils::Memory::CSharedPointer<IAllocator>              preferredAllocator() = 0;
        virtual std::vector<SDRMFormat>                                    getRenderableFormats();
        virtual std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>> getAllocators() = 0;
        virtual Hyprutils::Memory::CWeakPointer<IBackendImplementation>    getPrimary() = 0;
        virtual int                                                        drmRenderNodeFD() = 0;
    };

    /**
     * Aquamarine 后端主类
     *
     * 管理多个后端实现，提供统一的输出、输入和渲染接口。
     */
    class CBackend {
      public:
        /**
         * 创建后端
         *
         * @param backends  后端实现选项列表
         * @param options   后端选项
         * @return 后端实例
         */
        static Hyprutils::Memory::CSharedPointer<CBackend> create(
            const std::vector<SBackendImplementationOptions>& backends,
            const SBackendOptions& options);

        ~CBackend();

        /**
         * 启动后端
         *
         * @return true 成功, false 失败
         */
        bool start();

        /**
         * 日志输出
         */
        void log(eBackendLogLevel level, const std::string& msg);

        /**
         * 获取所有 poll FD
         *
         * 调用者需要监听这些 FD，当可读时调用对应的 onSignal
         */
        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> getPollFDs();

        /**
         * 是否有会话（DRM 后端）
         */
        bool hasSession();

        /**
         * 获取主 DRM FD
         */
        int drmFD();

        /**
         * 获取主 DRM 渲染节点 FD
         */
        int drmRenderNodeFD();

        /**
         * 获取主后端的渲染格式
         */
        std::vector<SDRMFormat> getPrimaryRenderFormats();

        /**
         * 获取所有后端实现
         */
        const std::vector<Hyprutils::Memory::CSharedPointer<IBackendImplementation>>& getImplementations();

        /**
         * 添加空闲事件
         */
        void addIdleEvent(Hyprutils::Memory::CSharedPointer<std::function<void(void)>> fn);

        /**
         * 移除空闲事件
         */
        void removeIdleEvent(Hyprutils::Memory::CSharedPointer<std::function<void(void)>> pfn);

        /**
         * 重新打开 DRM 节点（用于分配器）
         */
        int reopenDRMNode(int drmFD, bool allowRenderNode = true);

        /**
         * 热插拔新 GPU
         */
        void onNewGpu(std::string path);

        /**
         * 事件信号
         */
        struct {
            Hyprutils::Signal::CSignalT<Hyprutils::Memory::CSharedPointer<IOutput>>     newOutput;
            Hyprutils::Signal::CSignalT<Hyprutils::Memory::CSharedPointer<IPointer>>    newPointer;
            Hyprutils::Signal::CSignalT<Hyprutils::Memory::CSharedPointer<IKeyboard>>   newKeyboard;
            Hyprutils::Signal::CSignalT<Hyprutils::Memory::CSharedPointer<ITouch>>      newTouch;
            Hyprutils::Signal::CSignalT<Hyprutils::Memory::CSharedPointer<ISwitch>>     newSwitch;
            Hyprutils::Signal::CSignalT<Hyprutils::Memory::CSharedPointer<ITablet>>     newTablet;
            Hyprutils::Signal::CSignalT<Hyprutils::Memory::CSharedPointer<ITabletTool>> newTabletTool;
            Hyprutils::Signal::CSignalT<Hyprutils::Memory::CSharedPointer<ITabletPad>>  newTabletPad;

            Hyprutils::Signal::CSignalT<>                                               pollFDsChanged;
        } events;

        /** 主分配器（GBM） */
        Hyprutils::Memory::CSharedPointer<IAllocator> primaryAllocator;

        /** 是否就绪 */
        bool ready = false;

        /** 会话（DRM 后端） */
        Hyprutils::Memory::CSharedPointer<CSession> session;

      private:
        CBackend();

        bool                                                                   terminate = false;

        std::vector<SBackendImplementationOptions>                             implementationOptions;
        std::vector<Hyprutils::Memory::CSharedPointer<IBackendImplementation>> implementations;
        SBackendOptions                                                        options;
        Hyprutils::Memory::CWeakPointer<CBackend>                              self;
        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>                sessionFDs;
        Hyprutils::Memory::CSharedPointer<CLogger>                             logger;

        /** 空闲事件 */
        struct {
            int                                                                       fd = -1;
            std::vector<Hyprutils::Memory::CSharedPointer<std::function<void(void)>>> pending;
        } idle;

        void dispatchIdle();
        void updateIdleTimer();

        /** 事件循环内部状态 */
        struct {
            std::condition_variable loopSignal;
            std::mutex              loopMutex;
            std::atomic<bool>       shouldProcess = false;
            std::mutex              loopRequestMutex;
            std::mutex              eventLock;
        } m_sEventLoopInternals;

        friend class CDRMBackend;
        friend class CAnlandBackend;
    };
};