// src/backend/anland/AnlandBuffer.hpp
#ifndef AQUAMARINE_ANLAND_BUFFER_HPP
#define AQUAMARINE_ANLAND_BUFFER_HPP

#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include "display_producer.h"

namespace Aquamarine {

class CAnlandAllocator;

class CAnlandDmaBuffer : public IBuffer {
public:
    CAnlandDmaBuffer(int fd, const buf_info& info);
    virtual ~CAnlandDmaBuffer();

    virtual eBufferCapability caps() override { return BUFFER_CAPABILITY_NONE; }
    virtual eBufferType type() override { return BUFFER_TYPE_DMABUF; }
    virtual void update(const Hyprutils::Math::CRegion& damage) override {}
    virtual bool isSynchronous() override { return false; }
    virtual bool good() override { return m_fd >= 0; }
    virtual SDMABUFAttrs dmabuf() override;
    virtual SSHMAttrs shm() override { return SSHMAttrs{}; }
    virtual std::tuple<uint8_t*, uint32_t, size_t> beginDataPtr(uint32_t flags) override { return {nullptr, 0, 0}; }
    virtual void endDataPtr() override {}
    
    // 添加 release 支持
    virtual void sendRelease() override {
        inUse = false;
        // 通知 swapchain 缓冲区已释放
        events.backendRelease.emit();
    }

    bool inUse = false;

private:
    int m_fd = -1;
    buf_info m_info;
};

} // namespace Aquamarine

#endif