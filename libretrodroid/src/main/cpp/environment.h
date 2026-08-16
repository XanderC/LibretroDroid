/*
 *     Copyright (C) 2020  Filippo Scognamiglio
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

#ifndef LIBRETRODROID_ENVIRONMENT_H
#define LIBRETRODROID_ENVIRONMENT_H

#define MODULE_NAME_CORE "Libretro Core"

#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <EGL/egl.h>
#include <unordered_map>
#include <array>

#include "../../libretro-common/include/libretro.h"
#include "log.h"
#include "rumblestate.h"

struct Variable {
    std::string key;
    std::string value;
    std::string description;

    Variable() = default;
    Variable(const Variable&) = default;
    Variable(Variable&&) noexcept = default;
    Variable& operator=(const Variable&) = default;
    Variable& operator=(Variable&&) noexcept = default;
};

struct Controller {
    unsigned id = 0;
    std::string description;

    Controller() = default;
    Controller(const Controller&) = default;
    Controller(Controller&&) noexcept = default;
    Controller& operator=(const Controller&) = default;
    Controller& operator=(Controller&&) noexcept = default;
};

class Environment {
public:
    static Environment& getInstance()
    {
        static Environment instance;
        return instance;
    }
    Environment(Environment const&) = delete;
    void operator=(Environment const&) = delete;

    static void callback_retro_log(enum retro_log_level level, const char *fmt, ...);

    static bool callback_set_rumble_state(
        unsigned port,
        enum retro_rumble_effect effect,
        uint16_t strength
    );

    static bool callback_environment(unsigned cmd, void *data);

    void setEnableVirtualFileSystem(bool value);
    void setEnableMicrophone(bool value);

private:
    Environment() {}

public:
    void initialize(
        const std::string &requiredSystemDirectory,
        const std::string &requiredSavesDirectory,
        retro_hw_get_current_framebuffer_t required_callback_get_current_framebuffer
    );

    void deinitialize();

    void updateVariable(const std::string &key, const std::string &value);

    void setLanguage(const std::string &androidLanguage);

    float retrieveGameSpecificAspectRatio();

    bool handle_callback_set_rumble_state(
        unsigned port,
        enum retro_rumble_effect effect,
        uint16_t strength
    );

    bool handle_callback_environment(unsigned cmd, void *data);

    retro_hw_context_reset_t getHwContextReset() const;
    retro_hw_context_reset_t getHwContextDestroy() const;

    struct retro_disk_control_callback* getRetroDiskControlCallback() const;

    int getPixelFormat() const;
    bool isUseHwAcceleration() const;
    bool isUseDepth() const;
    bool isUseStencil() const;
    bool isBottomLeftOrigin() const;

    float getScreenRotation() const;
    bool isScreenRotationUpdated() const;
    void clearScreenRotationUpdated();

    unsigned int getGameGeometryWidth() const;
    unsigned int getGameGeometryHeight() const;
    float getGameGeometryAspectRatio() const;
    bool isGameGeometryUpdated() const;
    void clearGameGeometryUpdated();

    std::array<libretrodroid::RumbleState, 4> & getLastRumbleStates();

    const std::vector<struct Variable> getVariables() const;

    const std::vector<std::vector<struct Controller>> &getControllers() const;

    /** Diagnostics only: somewhere writable to drop a framebuffer dump. */
    const std::string &getSavesDirectory() const { return savesDirectory; }

    std::string getVariableValue(const std::string &key) const {
        auto it = variables.find(key);
        if (it != variables.end()) {
            return it->second.value;
        }
        return "";
    }

    bool isDolphinCore() const {
        return variables.find("dolphin_efb_scale") != variables.end();
    }

    unsigned int getDolphinScaleMultiplier() const {
        auto itDolphin = variables.find("dolphin_efb_scale");
        if (itDolphin != variables.end()) {
            const std::string &val = itDolphin->second.value;
            if (val == "6" || val.rfind("6x", 0) == 0 || val.rfind("x6", 0) == 0) return 6;
            if (val == "5" || val.rfind("5x", 0) == 0 || val.rfind("x5", 0) == 0) return 5;
            if (val == "4" || val.rfind("4x", 0) == 0 || val.rfind("x4", 0) == 0) return 4;
            if (val == "3" || val.rfind("3x", 0) == 0 || val.rfind("x3", 0) == 0) return 3;
            if (val == "2" || val.rfind("2x", 0) == 0 || val.rfind("x2", 0) == 0) return 2;
            if (val == "1" || val.rfind("1x", 0) == 0 || val.rfind("x1", 0) == 0) return 1;
        }
        return 1;
    }

private:
    bool environment_handle_set_variables(const struct retro_variable* received);
    bool environment_handle_get_variable(struct retro_variable* requested);
    bool environment_handle_set_controller_info(const struct retro_controller_info* received);
    bool environment_handle_set_hw_render(struct retro_hw_render_callback* hw_render_callback);
    bool environment_handle_get_vfs_interface(struct retro_vfs_interface_info* vfs_interface_info);
    bool environment_handle_get_microphone_interface(struct retro_microphone_interface* microphone_interface);

private:
    retro_hw_context_reset_t hw_context_reset = nullptr;
    retro_hw_context_reset_t hw_context_destroy = nullptr;
    struct retro_disk_control_callback *retro_disk_control_callback = nullptr;

    std::string savesDirectory;
    std::string systemDirectory;
    retro_hw_get_current_framebuffer_t callback_get_current_framebuffer = nullptr;
    unsigned language = RETRO_LANGUAGE_ENGLISH;
    bool useVirtualFileSystem = false;
    bool enableMicrophone = false;

    int pixelFormat = RETRO_PIXEL_FORMAT_RGB565;
    bool useHWAcceleration = false;
    bool useDepth = false;
    bool useStencil = false;
    bool bottomLeftOrigin = false;

    float screenRotation = 0;
    bool screenRotationUpdated = false;

    bool gameGeometryUpdated = false;
    unsigned gameGeometryWidth = 0;
    unsigned gameGeometryHeight = 0;
    float gameGeometryAspectRatio = -1.0f;

    std::array<libretrodroid::RumbleState, 4> rumbleStates;

    std::unordered_map<std::string, struct Variable> variables;
    bool dirtyVariables = false;

    std::vector<std::vector<struct Controller>> controllers;
};

#endif //LIBRETRODROID_ENVIRONMENT_H

