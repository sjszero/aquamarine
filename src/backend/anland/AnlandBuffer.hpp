// src/backend/anland/AnlandBuffer.hpp
#ifndef AQUAMARINE_ANLAND_BUFFER_HPP
#define AQUAMARINE_ANLAND_BUFFER_HPP

#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <drm_fourcc.h>
#include "display_producer.h"

namespace Aquamarine {

class CAnlandAllocator;

/**
 * Anland DMA buffer - wraps a dmabuf fd from the Android display daemon
 *
 * Supports:
 * - DRM modifiers (compressed textures)
 * - Multiple formats (8-bit, 10-bit, FP16)
 * - Buffer release signaling
 * - Forced linear modifier for compatibility
 */
class CAnlandDmaBuffer : public IBuffer {
public:
    CAnlandDmaBuffer(int fd, const buf_info& info,
                     uint32_t drmFormat, uint64_t modifier = DRM_FORMAT_MOD_INVALID,
                     bool forceLinear = false);
    virtual ~CAnlandDmaBuffer();

    virtual eBufferCapability caps() override { return BUFFER_CAPABILITY_NONE; }
    virtual eBufferType type() override { return BUFFER_TYPE_DMABUF; }
    virtual void update(const Hyprutils::Math::CRegion& damage) override {}
    virtual bool isSynchronous() override { return false; }
    virtual bool good() override { return m_fd >= 0 && m_eglImage != EGL_NO_IMAGE_KHR; }
    virtual SDMABUFAttrs dmabuf() override;
    virtual SSHMAttrs shm() override { return SSHMAttrs{}; }
    virtual std::tuple<uint8_t*, uint32_t, size_t> beginDataPtr(uint32_t flags) override { return {nullptr, 0, 0}; }
    virtual void endDataPtr() override {}
    virtual void sendRelease() override;

    // EGL image for direct rendering (bypasses CGLRenderbuffer)
    EGLImageKHR m_eglImage = EGL_NO_IMAGE_KHR;
    bool inUse = false;

private:
    int m_fd = -1;
    buf_info m_info;
    uint32_t m_drmFormat = DRM_FORMAT_XRGB8888;
    uint64_t m_modifier = DRM_FORMAT_MOD_INVALID;
    int m_ownedFd = -1;
    bool m_forceLinear = false;
};

} // namespace Aquamarine

#endif