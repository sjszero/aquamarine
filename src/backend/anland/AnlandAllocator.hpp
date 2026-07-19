// src/backend/anland/AnlandAllocator.hpp
#ifndef AQUAMARINE_ANLAND_ALLOCATOR_HPP
#define AQUAMARINE_ANLAND_ALLOCATOR_HPP

#include <aquamarine/allocator/Allocator.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <vector>
#include <mutex>
#include "display_producer.h"

namespace Aquamarine {

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::CWeakPointer;

class CAnlandBackend;
class CAnlandBuffer;

// 假装是 GBM 分配器，实际上从 Android 获取 dmabuf
class CAnlandAllocator : public IAllocator {
public:
    static CSharedPointer<IAllocator> create(display_ctx* display, CAnlandBackend* backend);
    virtual ~CAnlandAllocator();

    // IAllocator overrides - 假装是 GBM
    virtual CSharedPointer<IBuffer> acquire(const SAllocatorBufferParams& params, CSharedPointer<CSwapchain> swapchain_) override;
    virtual CSharedPointer<CBackend> getBackend() override;
    virtual int drmFD() override { return -1; }
    virtual eAllocatorType type() override { return AQ_ALLOCATOR_TYPE_GBM; } // 假装是 GBM！
    virtual void destroyBuffers() override;

    // 从 display 导入 Android 的 dmabuf
    bool importBuffers();
    int bufferCount() const { return m_bufferCount; }
    display_ctx* getDisplay() const { return m_display; }

private:
    CAnlandAllocator(display_ctx* display, CAnlandBackend* backend);

    display_ctx* m_display = nullptr;
    CAnlandBackend* m_backend = nullptr;
    std::vector<CWeakPointer<CAnlandBuffer>> m_buffers;
    int m_bufferCount = 0;
    int m_lastAcquired = -1;
    std::mutex m_mutex;
};

} // namespace Aquamarine

#endif