// src/backend/anland/AnlandBuffer.hpp
#ifndef AQUAMARINE_ANLAND_BUFFER_HPP
#define AQUAMARINE_ANLAND_BUFFER_HPP

#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/math/Vector2D.hpp>

extern "C" {
#include "display_producer.h"
}

namespace Aquamarine {

class CAnlandBackend;

/**
 * AnlandBuffer - 封装消费者提供的 dmabuf
 *
 * 继承自 IBuffer，提供 dmabuf 属性查询和可选的 CPU 访问。
 */
class CAnlandBuffer : public IBuffer {
public:
    CAnlandBuffer(display_ctx* display, int index, CAnlandBackend* backend);
    virtual ~CAnlandBuffer();

    // IBuffer 实现
    virtual eBufferCapability caps() override {
        return BUFFER_CAPABILITY_DATAPTR;
    }
    virtual eBufferType type() override {
        return BUFFER_TYPE_DMABUF;
    }
    virtual void update(const Hyprutils::Math::CRegion& damage) override;
    virtual bool isSynchronous() override { return false; }
    virtual bool good() override;
    virtual SDMABUFAttrs dmabuf() override;
    virtual SSHMAttrs shm() override;
    virtual std::tuple<uint8_t*, uint32_t, size_t> beginDataPtr(uint32_t flags) override;
    virtual void endDataPtr() override;

    void onBackendRelease();

private:
    display_ctx* m_display = nullptr;
    CAnlandBackend* m_backend = nullptr;
    int m_index = -1;
    int m_fd = -1;
    uint32_t m_format = 0;
    uint64_t m_modifier = 0;
    uint32_t m_stride = 0;
    uint32_t m_offset = 0;
    bool m_good = false;
    bool m_locked = false;

    // CPU 访问
    void* m_mappedData = nullptr;
    size_t m_mappedSize = 0;
    bool m_mapped = false;
};

} // namespace Aquamarine

#endif // AQUAMARINE_ANLAND_BUFFER_HPP