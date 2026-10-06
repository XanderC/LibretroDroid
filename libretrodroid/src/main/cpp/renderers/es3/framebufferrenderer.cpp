/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "framebufferrenderer.h"
#include "es3utils.h"
#include "../../log.h"
#include "../../environment.h"

namespace libretrodroid {

FramebufferRenderer::FramebufferRenderer(
    unsigned width,
    unsigned height,
    bool depth,
    bool stencil,
    ShaderManager::Chain shaders
) {
    this->depth = depth;
    this->stencil = stencil;
    this->width = width;
    this->height = height;
    this->shaders = std::move(shaders);

    initializeBuffers();
}

void FramebufferRenderer::onNewFrame(const void *data, unsigned width, unsigned height, size_t pitch) {
    Renderer::onNewFrame(data, width, height, pitch);

    if (width > 0 && height > 0) {
        unsigned int targetWidth = width;
        unsigned int targetHeight = height;

        // Dolphin's Libretro port reports unscaled native dimensions (e.g. 640x528)
        // while internally rendering at efb_scale into the FBO.
        if (Environment::getInstance().isDolphinCore()) {
            unsigned int scale = Environment::getInstance().getDolphinScaleMultiplier();
            if (scale > 1 && width <= 640) {
                targetWidth = width * scale;
                targetHeight = height * scale;
            }
        }

        if (this->width != targetWidth || this->height != targetHeight) {
            LOGI("[rfdiag] FramebufferRenderer::onNewFrame resizing buffer from %ux%u to %ux%u (core reported %ux%u)",
                 this->width, this->height, targetWidth, targetHeight, width, height);
            this->width = targetWidth;
            this->height = targetHeight;
            initializeBuffers();
            isDirty = false;
        }
    }

    if (isDirty) {
        initializeBuffers();
        isDirty = false;
    }
}

void FramebufferRenderer::initializeBuffers() {
    unsigned int targetW = width;
    unsigned int targetH = height;

    if (Environment::getInstance().isDolphinCore()) {
        unsigned int scale = Environment::getInstance().getDolphinScaleMultiplier();
        if (targetW == 0) targetW = 640 * scale;
        if (targetH == 0) targetH = 480 * scale;
    } else {
        if (targetW == 0) targetW = 640;
        if (targetH == 0) targetH = 480;
    }

    this->width = targetW;
    this->height = targetH;

    LOGI("[rfdiag] FramebufferRenderer::initializeBuffers %dx%d depth=%d stencil=%d currentFB=%p",
         width, height, depth, stencil, framebuffer.get());
    framebuffers = ES3Utils::buildShaderPasses(width, height, shaders);

    // Resized in place once it exists: a core may hold on to the name get_current_framebuffer
    // returned (see ES3Utils::resizeFramebuffer), so the framebuffer object must outlive a resize.
    // Only a real one, though: the constructor builds the first before a GL context is current,
    // and that one's name is 0 — the default framebuffer — so it's replaced, not resized.
    if (framebuffer != nullptr && framebuffer->framebuffer != 0 && glIsFramebuffer(framebuffer->framebuffer)) {
        ES3Utils::resizeFramebuffer(*framebuffer, width, height, shaders.linearTexture, false, depth, stencil);
    } else {
        ES3Utils::deleteFramebuffer(std::move(framebuffer));
        framebuffer = ES3Utils::createFramebuffer(
            width,
            height,
            shaders.linearTexture,
            false,
            depth,
            stencil
        );
    }
    LOGI("[rfdiag] FramebufferRenderer::initializeBuffers done. newFB id=%lu tex=%lu",
         (unsigned long) framebuffer->framebuffer, (unsigned long) framebuffer->texture);
}

uintptr_t FramebufferRenderer::getTexture() {
    return framebuffer->texture;
}

uintptr_t FramebufferRenderer::getFramebuffer() {
    return framebuffer->framebuffer;
}

void FramebufferRenderer::setPixelFormat(int pixelFormat) {
    // TODO... Here we should handle 32bit framebuffers.
}

void FramebufferRenderer::updateRenderedResolution(unsigned int inWidth, unsigned int inHeight) {
    if (inWidth == 0 || inHeight == 0) {
        LOGI("[rfdiag] updateRenderedResolution ignoring 0x0 input (current=%ux%u)", this->width, this->height);
        return;
    }

    unsigned int targetWidth = inWidth;
    unsigned int targetHeight = inHeight;

    if (Environment::getInstance().isDolphinCore()) {
        unsigned int scale = Environment::getInstance().getDolphinScaleMultiplier();
        if (scale > 1 && inWidth <= 640) {
            targetWidth = inWidth * scale;
            targetHeight = inHeight * scale;
        }
    }

    LOGI("[rfdiag] updateRenderedResolution in=%ux%u -> target=%ux%u current=%ux%u",
         inWidth, inHeight, targetWidth, targetHeight, this->width, this->height);

    if (this->width != targetWidth || this->height != targetHeight) {
        this->width = targetWidth;
        this->height = targetHeight;
        isDirty = true;
    }
}

bool FramebufferRenderer::rendersInVideoCallback() {
    return true;
}

void FramebufferRenderer::setShaders(ShaderManager::Chain shaders) {
    if (shaders != this->shaders) {
        this->shaders = shaders;
        isDirty = true;
    }
}

Renderer::PassData FramebufferRenderer::getPassData(unsigned int layer) {
    PassData result;

    if (layer >= 0 && layer < framebuffers->size()) {
        result.framebuffer = framebuffers->at(layer)->framebuffer;
        result.width = framebuffers->at(layer)->width;
        result.height = framebuffers->at(layer)->height;
    }

    if (layer > 0 && layer < framebuffers->size() + 1) {
        result.texture = framebuffers->at(layer - 1)->texture;
    }

    return result;
}

} //namespace libretrodroid
