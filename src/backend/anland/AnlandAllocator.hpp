// src/backend/anland/AnlandAllocator.hpp
#ifndef AQUAMARINE_ANLAND_ALLOCATOR_HPP
#define AQUAMARINE_ANLAND_ALLOCATOR_HPP

#include <aquamarine/allocator/Allocator.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <vector>
#include <mutex>
#include "display_producer.h"

namespace Aquamarine {

class CAnlandBackend;
class CAnlandBuffer;

class CAnlandAllocator : public IAllocator {
public:
    static Hyprutils::Memory::CSharedPointer<CAnlandAllocator> create(display_ctx* display, CAnlandBackend* backend);
    virtual ~CAnlandAllocator();

    // IAllocator overrides
    virtual Hyprutils::Memory::CSharedPointer<IBuffer> acquire(const SAllocatorBufferParams& params, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain_) override;
    virtual Hyprutils::Memory::CSharedPointer<CBackend> getBackend() override;
    virtual int drmFD() override { return -1; }
    virtual eAllocatorType type() override { return AQ_ALLOCATOR_TYPE_MISC; }
    virtual void destroyBuffers() override;

    // Import buffers from display
    bool importBuffers();
    int bufferCount() const { return m_bufferCount; }

private:
    CAnlandAllocator(display_ctx* display, CAnlandBackend* backend);

    display_ctx* m_display = nullptr;
    CAnlandBackend* m_backend = nullptr;
    std::vector<Hyprutils::Memory::CWeakPointer<CAnlandBuffer>> m_buffers;
    int m_bufferCount = 0;
    int m_lastAcquired = -1;
    std::mutex m_mutex;
};

} // namespace Aquamarine

#endif