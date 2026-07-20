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

    enum eBackendType : uint32_t {
        AQ_BACKEND_WAYLAND = 0,
        AQ_BACKEND_DRM,
        AQ_BACKEND_HEADLESS,
        AQ_BACKEND_NULL,
        AQ_BACKEND_ANLAND,      // Anland (Android display) backend
    };

    enum eBackendRequestMode : uint32_t {
        AQ_BACKEND_REQUEST_MANDATORY = 0,
        AQ_BACKEND_REQUEST_IF_AVAILABLE,
        AQ_BACKEND_REQUEST_FALLBACK,
    };

    enum eBackendLogLevel : uint32_t {
        AQ_LOG_TRACE = 0,
        AQ_LOG_DEBUG,
        AQ_LOG_WARNING,
        AQ_LOG_ERROR,
        AQ_LOG_CRITICAL,
    };

    struct SBackendImplementationOptions {
        explicit SBackendImplementationOptions();
        eBackendType        backendType;
        eBackendRequestMode backendRequestMode;
    };

    struct SBackendOptions {
        explicit SBackendOptions();
        std::function<void(eBackendLogLevel, std::string)>                   logFunction;
        Hyprutils::Memory::CSharedPointer<Hyprutils::CLI::CLoggerConnection> logConnection;
    };

    struct SPollFD {
        int                       fd = -1;
        std::function<void(void)> onSignal;
    };

    class IBackendImplementation {
      public:
        virtual ~IBackendImplementation() = default;

        enum eBackendCapabilities : uint32_t {
            AQ_BACKEND_CAPABILITY_POINTER = (1 << 0),
        };

        virtual eBackendType                                               type()                                     = 0;
        virtual bool                                                       start()                                    = 0;
        virtual std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>    pollFDs()                                  = 0;
        virtual int                                                        drmFD()                                    = 0;
        virtual bool                                                       dispatchEvents()                           = 0;
        virtual uint32_t                                                   capabilities()                             = 0;
        virtual void                                                       onReady()                                  = 0;
        virtual std::vector<SDRMFormat>                                    getRenderFormats()                         = 0;
        virtual std::vector<SDRMFormat>                                    getCursorFormats()                         = 0;
        virtual bool                                                       createOutput(const std::string& name = "") = 0;
        virtual Hyprutils::Memory::CSharedPointer<IAllocator>              preferredAllocator()                       = 0;
        virtual std::vector<SDRMFormat>                                    getRenderableFormats()                     = 0;
        virtual std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>> getAllocators()                           = 0;
        virtual Hyprutils::Memory::CWeakPointer<IBackendImplementation>    getPrimary()                               = 0;
        virtual int                                                        drmRenderNodeFD()                          = 0;
    };

    class CBackend {
      public:
        static Hyprutils::Memory::CSharedPointer<CBackend> create(
            const std::vector<SBackendImplementationOptions>& backends,
            const SBackendOptions& options);

        ~CBackend();

        bool start();

        void log(eBackendLogLevel level, const std::string& msg);

        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> getPollFDs();

        bool hasSession();

        int drmFD();

        int drmRenderNodeFD();

        std::vector<SDRMFormat> getPrimaryRenderFormats();

        const std::vector<Hyprutils::Memory::CSharedPointer<IBackendImplementation>>& getImplementations();

        void addIdleEvent(Hyprutils::Memory::CSharedPointer<std::function<void(void)>> fn);

        void removeIdleEvent(Hyprutils::Memory::CSharedPointer<std::function<void(void)>> pfn);

        int reopenDRMNode(int drmFD, bool allowRenderNode = true);

        void onNewGpu(std::string path);

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

        Hyprutils::Memory::CSharedPointer<IAllocator> primaryAllocator;
        bool                                          ready = false;
        Hyprutils::Memory::CSharedPointer<CSession>   session;

      private:
        CBackend();

        bool                                                                   terminate = false;

        std::vector<SBackendImplementationOptions>                             implementationOptions;
        std::vector<Hyprutils::Memory::CSharedPointer<IBackendImplementation>> implementations;
        SBackendOptions                                                        options;
        Hyprutils::Memory::CWeakPointer<CBackend>                              self;
        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>                sessionFDs;
        Hyprutils::Memory::CSharedPointer<CLogger>                             logger;

        struct {
            int                                                                       fd = -1;
            std::vector<Hyprutils::Memory::CSharedPointer<std::function<void(void)>>> pending;
        } idle;

        void dispatchIdle();
        void updateIdleTimer();

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