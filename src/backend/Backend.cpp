// src/backend/Backend.cpp
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Wayland.hpp>
#include <aquamarine/backend/Headless.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/backend/Null.hpp>
#include <aquamarine/allocator/GBM.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#include <ranges>
#include <sys/timerfd.h>
#include <ctime>
#include <cstring>
#include <xf86drm.h>
#include <fcntl.h>
#include <unistd.h>

#include "Logger.hpp"

#ifdef HAS_ANLAND_BACKEND
#include "anland/AnlandBackend.hpp"
#endif

using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;
using namespace Aquamarine;
#define SP CSharedPointer

#define TIMESPEC_NSEC_PER_SEC 1000000000LL

/* ============================================================
 * 辅助函数
 * ============================================================ */

static void timespecAddNs(timespec* pTimespec, int64_t delta) {
    int delta_ns_low = delta % TIMESPEC_NSEC_PER_SEC;
    int delta_s_high = delta / TIMESPEC_NSEC_PER_SEC;

    pTimespec->tv_sec += delta_s_high;

    pTimespec->tv_nsec += (long)delta_ns_low;
    if (pTimespec->tv_nsec >= TIMESPEC_NSEC_PER_SEC) {
        pTimespec->tv_nsec -= TIMESPEC_NSEC_PER_SEC;
        ++pTimespec->tv_sec;
    }
}

static const char* backendTypeToName(eBackendType type) {
    switch (type) {
        case AQ_BACKEND_DRM:        return "drm";
        case AQ_BACKEND_HEADLESS:   return "headless";
        case AQ_BACKEND_WAYLAND:    return "wayland";
        case AQ_BACKEND_NULL:       return "null";
#ifdef HAS_ANLAND_BACKEND
        case AQ_BACKEND_ANLAND:     return "anland";
#endif
        default:                    break;
    }
    return "invalid";
}

#ifdef HAS_ANLAND_BACKEND
static bool hasAnlandImplementation(const std::vector<SP<IBackendImplementation>>& impls) {
    for (auto const& impl : impls) {
        if (impl->type() == AQ_BACKEND_ANLAND) {
            return true;
        }
    }
    return false;
}
#endif

/* ============================================================
 * SBackendImplementationOptions
 * ============================================================ */

Aquamarine::SBackendImplementationOptions::SBackendImplementationOptions()
    : backendType(AQ_BACKEND_WAYLAND)
    , backendRequestMode(AQ_BACKEND_REQUEST_IF_AVAILABLE) {
    ;
}

/* ============================================================
 * SBackendOptions
 * ============================================================ */

Aquamarine::SBackendOptions::SBackendOptions()
    : logFunction(nullptr) {
    ;
}

/* ============================================================
 * CBackend 构造/析构
 * ============================================================ */

Aquamarine::CBackend::CBackend() {
    ;
}

Aquamarine::CBackend::~CBackend() {
    // Tear down implementations before the logger is destroyed,
    // as backends may log during teardown (e.g. SDRMConnector::disconnect).
    implementations.clear();
}

/* ============================================================
 * CBackend::create() - 工厂方法
 * ============================================================ */

Hyprutils::Memory::CSharedPointer<CBackend> Aquamarine::CBackend::create(
    const std::vector<SBackendImplementationOptions>& backends,
    const SBackendOptions& options) {

    auto backend = SP<CBackend>(new CBackend());

    backend->options               = options;
    backend->logger                = Hyprutils::Memory::makeShared<CLogger>();
    backend->implementationOptions = backends;
    backend->self                  = backend;

    backend->logger->m_loggerConnection = options.logConnection;
    backend->logger->m_logFn            = options.logFunction;
    backend->logger->updateLevels();

    if (backends.size() <= 0)
        return nullptr;

    backend->log(AQ_LOG_DEBUG, "Creating an Aquamarine backend!");

    /* ============================================================
     * 构建有效后端列表（检测 ANLAND 环境变量）
     * ============================================================ */
    std::vector<SBackendImplementationOptions> effectiveBackends;

#ifdef HAS_ANLAND_BACKEND
    const char* anlandEnv = getenv("ANLAND");
    if (anlandEnv && strcmp(anlandEnv, "1") == 0) {
        backend->log(AQ_LOG_DEBUG, "ANLAND=1 detected: Anland backend only");

        // 只创建 Anland 后端
        SBackendImplementationOptions anlandOpt;
        anlandOpt.backendType = AQ_BACKEND_ANLAND;
        anlandOpt.backendRequestMode = AQ_BACKEND_REQUEST_MANDATORY;
        effectiveBackends.push_back(anlandOpt);

        // 添加 Headless 作为 fallback（当消费者断开时）
        SBackendImplementationOptions headlessOpt;
        headlessOpt.backendType = AQ_BACKEND_HEADLESS;
        headlessOpt.backendRequestMode = AQ_BACKEND_REQUEST_FALLBACK;
        effectiveBackends.push_back(headlessOpt);

        // 不添加 DRM/Wayland 后端
    } else {
        effectiveBackends = backends;
    }
#else
    effectiveBackends = backends;
#endif

    /* ============================================================
     * 创建后端实现
     * ============================================================ */
    for (auto const& b : effectiveBackends) {
        if (b.backendType == AQ_BACKEND_WAYLAND) {
            auto ref = SP<CWaylandBackend>(new CWaylandBackend(backend));
            backend->implementations.emplace_back(ref);
            ref->self = ref;
            backend->log(AQ_LOG_DEBUG, "Created Wayland backend");

        } else if (b.backendType == AQ_BACKEND_DRM) {
            auto ref = CDRMBackend::attempt(backend);
            if (ref.empty()) {
                backend->log(AQ_LOG_ERROR, "DRM Backend failed");
                continue;
            }
            for (auto const& r : ref) {
                backend->implementations.emplace_back(r);
                backend->log(AQ_LOG_DEBUG, "Created DRM backend");
            }

        } else if (b.backendType == AQ_BACKEND_HEADLESS) {
            auto ref = SP<CHeadlessBackend>(new CHeadlessBackend(backend));
            backend->implementations.emplace_back(ref);
            ref->self = ref;
            backend->log(AQ_LOG_DEBUG, "Created Headless backend");

        } else if (b.backendType == AQ_BACKEND_NULL) {
            auto ref = SP<CNullBackend>(new CNullBackend(backend));
            backend->implementations.emplace_back(ref);
            ref->self = ref;
            backend->log(AQ_LOG_DEBUG, "Created Null backend");

#ifdef HAS_ANLAND_BACKEND
        } else if (b.backendType == AQ_BACKEND_ANLAND) {
            // 避免重复创建
            bool alreadyExists = false;
            for (const auto& impl : backend->implementations) {
                if (impl->type() == AQ_BACKEND_ANLAND) {
                    alreadyExists = true;
                    break;
                }
            }
            if (!alreadyExists) {
                const char* socketPath = getenv("ANLAND_SOCKET");
                if (!socketPath || socketPath[0] == '\0') {
                    socketPath = "/run/display.sock";
                }
                auto ref = SP<CAnlandBackend>(new CAnlandBackend(backend, socketPath));
                backend->implementations.emplace_back(ref);
                ref->self = ref;
                backend->log(AQ_LOG_DEBUG, std::string("Created Anland backend, socket: ") + socketPath);
            } else {
                backend->log(AQ_LOG_DEBUG, "Anland backend already exists, skipping duplicate");
            }
#endif

        } else {
            backend->log(AQ_LOG_ERROR, std::format("Unknown backend id: {}", (int)b.backendType));
            continue;
        }
    }

    /* ============================================================
     * 确保至少有一个后端（fallback 到 Headless）
     * ============================================================ */
    if (backend->implementations.empty()) {
        backend->log(AQ_LOG_WARNING, "No backends available, adding Headless as fallback");
        auto ref = SP<CHeadlessBackend>(new CHeadlessBackend(backend));
        backend->implementations.emplace_back(ref);
        ref->self = ref;
    }

    // create a timerfd for idle events
    backend->idle.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

    return backend;
}

/* ============================================================
 * CBackend::start() - 启动后端
 * ============================================================ */

bool Aquamarine::CBackend::start() {
    log(AQ_LOG_DEBUG, "Starting the Aquamarine backend!");

    int started = 0;

    auto optionsForType = [this](eBackendType type) -> SBackendImplementationOptions {
        for (auto const& o : implementationOptions) {
            if (o.backendType == type)
                return o;
        }
        return SBackendImplementationOptions{};
    };

    /* ============================================================
     * 启动所有后端实现
     * ============================================================ */
    for (size_t i = 0; i < implementations.size(); ++i) {
        const auto& impl = implementations.at(i);
        log(AQ_LOG_DEBUG, std::format("Starting backend: {}", backendTypeToName(impl->type())));
        const bool ok = impl->start();

        if (!ok) {
            log(AQ_LOG_ERROR,
                std::format("Requested backend ({}) could not start, enabling fallbacks",
                    backendTypeToName(impl->type())));
            if (optionsForType(impl->type()).backendRequestMode == AQ_BACKEND_REQUEST_MANDATORY) {
                log(AQ_LOG_CRITICAL,
                    std::format("Requested backend ({}) could not start and it's mandatory, cannot continue!",
                        backendTypeToName(impl->type())));
                implementations.clear();
                return false;
            }
        } else {
            started++;
        }
    }

    /* ============================================================
     * 检查是否有任何后端成功启动
     * ============================================================ */
    if (implementations.empty() || started <= 0) {
        log(AQ_LOG_CRITICAL, "No backend could be opened.");
        return false;
    }

    /* ============================================================
     * 移除失败的实现
     * ============================================================ */
    std::erase_if(implementations, [this](const auto& i) {
        bool failed = i->pollFDs().empty() && i->type() != AQ_BACKEND_NULL;
        if (failed) {
            log(AQ_LOG_ERROR, std::format("Implementation {} failed, erasing.", backendTypeToName(i->type())));
        }
        return failed;
    });

    /* ============================================================
     * 创建 GBM 分配器
     *
     * Anland 后端使用消费者提供的 dmabuf，不需要 GBM 分配器。
     * 只有非 Anland 后端才需要 GBM。
     * ============================================================ */
#ifdef HAS_ANLAND_BACKEND
    bool hasAnland = hasAnlandImplementation(implementations);
#else
    bool hasAnland = false;
#endif

    if (!hasAnland) {
        // 优先使用 DRM 后端创建 GBM 分配器
        bool hasDRM = false;
        for (auto const& b : implementations) {
            if (b->drmFD() >= 0) {
                hasDRM = true;
                break;
            }
        }

        if (hasDRM) {
            for (auto const& b : implementations) {
                if (b->drmFD() >= 0) {
                    auto fd = reopenDRMNode(b->drmFD());
                    if (fd < 0) {
                        log(AQ_LOG_ERROR, "Failed to create an allocator (reopenDRMNode failed)");
                    } else {
                        primaryAllocator = CGBMAllocator::create(fd, self);
                        if (primaryAllocator) {
                            log(AQ_LOG_DEBUG, "GBM allocator created successfully");
                        }
                    }
                    break;
                }
            }
        } else {
            log(AQ_LOG_DEBUG, "No DRM backend found, skipping GBM allocator creation");
        }

        /* 检查是否有非 DRM 后端（Headless/Null） */
        bool hasNonDRMBackend = false;
        for (auto const& b : implementations) {
            if (b->type() == AQ_BACKEND_HEADLESS || b->type() == AQ_BACKEND_NULL) {
                hasNonDRMBackend = true;
                break;
            }
        }

        if (!primaryAllocator && !hasNonDRMBackend) {
            log(AQ_LOG_WARNING, "No allocator available, some features may not work");
        }
    } else {
        log(AQ_LOG_DEBUG, "Anland backend detected, skipping GBM allocator creation");
    }

    /* ============================================================
     * 标记就绪，通知各后端
     * ============================================================ */
    ready = true;
    for (auto const& b : implementations) {
        b->onReady();
    }

    if (session)
        session->onReady();

    sessionFDs = session ? session->pollFDs() : std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>{};

    return true;
}

/* ============================================================
 * CBackend::log() - 日志输出
 * ============================================================ */

void Aquamarine::CBackend::log(eBackendLogLevel level, const std::string& msg) {
    if (logger)
        logger->log(level, msg);
}

/* ============================================================
 * CBackend::getPollFDs() - 获取所有 poll FD
 * ============================================================ */

std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> Aquamarine::CBackend::getPollFDs() {
    std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> result;

    for (auto const& i : implementations) {
        auto pollfds = i->pollFDs();
        for (auto const& p : pollfds) {
            log(AQ_LOG_DEBUG, std::format("backend: poll fd {} for implementation {}", p->fd, backendTypeToName(i->type())));
            result.emplace_back(p);
        }
    }

    for (auto const& sfd : sessionFDs) {
        log(AQ_LOG_DEBUG, std::format("backend: poll fd {} for session", sfd->fd));
        result.emplace_back(sfd);
    }

    log(AQ_LOG_DEBUG, std::format("backend: poll fd {} for idle", idle.fd));
    result.emplace_back(makeShared<SPollFD>(idle.fd, [this]() { dispatchIdle(); }));

    return result;
}

/* ============================================================
 * CBackend::drmFD() - 获取主 DRM FD
 * ============================================================ */

int Aquamarine::CBackend::drmFD() {
    for (auto const& i : implementations) {
        int fd = i->drmFD();
        if (fd < 0)
            continue;
        return fd;
    }
    return -1;
}

/* ============================================================
 * CBackend::drmRenderNodeFD() - 获取主 DRM 渲染节点 FD
 * ============================================================ */

int Aquamarine::CBackend::drmRenderNodeFD() {
    for (auto const& i : implementations) {
        int fd = i->drmRenderNodeFD();
        if (fd < 0)
            continue;
        return fd;
    }
    return -1;
}

/* ============================================================
 * CBackend::hasSession() - 是否有会话
 * ============================================================ */

bool Aquamarine::CBackend::hasSession() {
    return !!session;
}

/* ============================================================
 * CBackend::getPrimaryRenderFormats() - 获取主渲染格式
 * ============================================================ */

std::vector<SDRMFormat> Aquamarine::CBackend::getPrimaryRenderFormats() {
    for (auto const& b : implementations) {
        if (b->type() != AQ_BACKEND_DRM && b->type() != AQ_BACKEND_WAYLAND)
            continue;
        return b->getRenderFormats();
    }
    for (auto const& b : implementations) {
        return b->getRenderFormats();
    }
    return {};
}

/* ============================================================
 * CBackend::getImplementations() - 获取所有后端实现
 * ============================================================ */

const std::vector<SP<IBackendImplementation>>& Aquamarine::CBackend::getImplementations() {
    return implementations;
}

/* ============================================================
 * CBackend::addIdleEvent() / removeIdleEvent() - 空闲事件管理
 * ============================================================ */

void Aquamarine::CBackend::addIdleEvent(SP<std::function<void(void)>> fn) {
    idle.pending.emplace_back(fn);
    updateIdleTimer();
}

void Aquamarine::CBackend::removeIdleEvent(SP<std::function<void(void)>> pfn) {
    std::erase(idle.pending, pfn);
}

/* ============================================================
 * CBackend::updateIdleTimer() - 更新空闲计时器
 * ============================================================ */

void Aquamarine::CBackend::updateIdleTimer() {
    uint64_t ADD_NS = idle.pending.empty() ? TIMESPEC_NSEC_PER_SEC * 240ULL : 0;

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    timespecAddNs(&now, ADD_NS);

    itimerspec ts = {.it_value = now};

    if (timerfd_settime(idle.fd, TFD_TIMER_ABSTIME, &ts, nullptr))
        log(AQ_LOG_ERROR, std::format("backend: failed to arm timerfd: {}", strerror(errno)));
}

/* ============================================================
 * CBackend::dispatchIdle() - 调度空闲事件
 * ============================================================ */

void Aquamarine::CBackend::dispatchIdle() {
    if (!CFileDescriptor::isReadable(idle.fd)) {
        log(AQ_LOG_ERROR, std::format("dispatchIdle: dispatched a non-readable idle event on fd: {}", idle.fd));
    } else {
        uint64_t expirations;
        ssize_t ret = read(idle.fd, &expirations, sizeof(expirations));
        if (ret < 0) {
            log(AQ_LOG_ERROR, std::format("dispatchIdle: read from timerfd failed: {}", strerror(errno)));
        }
    }

    auto cpy = idle.pending;
    idle.pending.clear();

    for (auto const& i : cpy) {
        if (i && *i)
            (*i)();
    }

    updateIdleTimer();
}

/* ============================================================
 * CBackend::onNewGpu() - 热插拔新 GPU
 * ============================================================ */

void Aquamarine::CBackend::onNewGpu(std::string path) {
    const auto primary = std::ranges::find_if(implementations,
        [](SP<IBackendImplementation> value) { return value->type() == Aquamarine::AQ_BACKEND_DRM; });
    const auto primaryDrm = primary != implementations.end() ?
        ((Aquamarine::CDRMBackend*)(*primary).get())->self.lock() : nullptr;

    auto ref = CDRMBackend::fromGpu(path, self.lock(), primaryDrm);
    if (!ref) {
        log(AQ_LOG_ERROR, std::format("DRM Backend failed for device {}", path));
        return;
    }
    if (!ref->start()) {
        log(AQ_LOG_ERROR, std::format("Couldn't start DRM Backend for device {}", path));
        return;
    }

    implementations.emplace_back(ref);
    events.pollFDsChanged.emit();
    ref->onReady();
    ref->recheckOutputs();
}

/* ============================================================
 * CBackend::reopenDRMNode() - 重新打开 DRM 节点
 *
 * 从 wlroots 移植，用于分配器引用计数
 * ============================================================ */

int Aquamarine::CBackend::reopenDRMNode(int drmFD, bool allowRenderNode) {
    if (drmIsMaster(drmFD)) {
        uint32_t lesseeID = 0;
        int leaseFD = drmModeCreateLease(drmFD, nullptr, 0, O_CLOEXEC, &lesseeID);
        if (leaseFD >= 0) {
            return leaseFD;
        } else if (leaseFD != -EINVAL && leaseFD != -EOPNOTSUPP) {
            log(AQ_LOG_ERROR, "reopenDRMNode: drmModeCreateLease failed");
            return -1;
        }
        log(AQ_LOG_DEBUG, "reopenDRMNode: drmModeCreateLease failed, falling back to open");
    }

    char* name = nullptr;
    if (allowRenderNode)
        name = drmGetRenderDeviceNameFromFd(drmFD);

    if (!name) {
        name = drmGetDeviceNameFromFd2(drmFD);
        if (!name) {
            log(AQ_LOG_ERROR, "reopenDRMNode: drmGetDeviceNameFromFd2 failed");
            return -1;
        }
    }

    log(AQ_LOG_DEBUG, std::format("reopenDRMNode: opening node {}", name));

    int newFD = open(name, O_RDWR | O_CLOEXEC);
    if (newFD < 0) {
        log(AQ_LOG_ERROR, std::format("reopenDRMNode: failed to open node {}", name));
        free(name);
        return -1;
    }

    free(name);

    if (drmIsMaster(drmFD) && drmGetNodeTypeFromFd(newFD) == DRM_NODE_PRIMARY) {
        drm_magic_t magic;
        if (int ret = drmGetMagic(newFD, &magic); ret < 0) {
            log(AQ_LOG_ERROR, std::format("reopenDRMNode: drmGetMagic failed: {}", strerror(-ret)));
            close(newFD);
            return -1;
        }
        if (int ret = drmAuthMagic(drmFD, magic); ret < 0) {
            log(AQ_LOG_ERROR, std::format("reopenDRMNode: drmAuthMagic failed: {}", strerror(-ret)));
            close(newFD);
            return -1;
        }
    }

    return newFD;
}

/* ============================================================
 * IBackendImplementation::getRenderableFormats() - 默认实现
 * ============================================================ */

std::vector<SDRMFormat> Aquamarine::IBackendImplementation::getRenderableFormats() {
    return {};
}