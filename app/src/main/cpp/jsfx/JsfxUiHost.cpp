/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "JsfxUiHost.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace guitarrackcraft {

struct JsfxUiHost::State {
    struct Key {
        uint32_t modifiers = 0;
        uint32_t code = 0;
        bool pressed = false;
    };
    struct PendingMenu {
        uint64_t id = 0;
        bool active = false;
        int32_t result = 0;
    };

    ysfx_t* effect = nullptr;
    std::mutex mutex;
    std::condition_variable condition;
    std::thread thread;
    bool stopping = false;
    bool dirty = true;
    bool visible = false;
    bool focused = false;
    bool mouseOver = false;
    bool configured = false;
    bool effectPaused = false;
    bool effectCallActive = false;
    uint32_t width = 320;
    uint32_t height = 240;
    float scale = 1.0f;
    uint32_t modifiers = 0;
    uint32_t buttons = 0;
    int32_t mouseX = 0;
    int32_t mouseY = 0;
    double wheel = 0.0;
    double horizontalWheel = 0.0;
    Key keys[128]{};
    uint32_t keyRead = 0;
    uint32_t keyWrite = 0;
    ANativeWindow* window = nullptr;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> converted;
    std::vector<std::string> dropFiles;
    std::string activeDropPath;
    int32_t cursor = 0;
    uint64_t nextMenuId = 1;
    PendingMenu pendingMenu;
    std::vector<MenuRequest> menuRequests;
};

JsfxUiHost::JsfxUiHost(ysfx_t* effect) : state_(new State) {
    state_->effect = effect;
    uint32_t dimensions[2]{};
    if (effect && ysfx_get_gfx_dim(effect, dimensions) &&
        dimensions[0] > 0 && dimensions[0] <= 8192 &&
        dimensions[1] > 0 && dimensions[1] <= 8192) {
        state_->width = dimensions[0];
        state_->height = dimensions[1];
    }
    state_->thread = std::thread(&JsfxUiHost::run, this);
}

JsfxUiHost::~JsfxUiHost() {
    stop();
    delete state_;
}

void JsfxUiHost::attachWindow(ANativeWindow* window) {
    if (window) ANativeWindow_acquire(window);
    ANativeWindow* previous = nullptr;
    {
        std::lock_guard lock(state_->mutex);
        previous = std::exchange(state_->window, window);
        state_->configured = false;
        state_->dirty = true;
    }
    if (previous) ANativeWindow_release(previous);
    state_->condition.notify_all();
}

void JsfxUiHost::detachWindow() {
    attachWindow(nullptr);
}

void JsfxUiHost::stop() {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->pendingMenu.active = false;
    }
    state_->condition.notify_all();
    if (state_->thread.joinable()) state_->thread.join();
    detachWindow();
}

void JsfxUiHost::resize(uint32_t width, uint32_t height, float scale) {
    if (width == 0 || height == 0 || width > 8192 || height > 8192) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->width = width;
        state_->height = height;
        state_->scale = std::max(1.0f, scale);
        state_->configured = false;
        state_->dirty = true;
    }
    state_->condition.notify_all();
}

void JsfxUiHost::setVisible(bool visible) {
    {
        std::lock_guard lock(state_->mutex);
        state_->visible = visible;
        state_->dirty = true;
    }
    state_->condition.notify_all();
}

void JsfxUiHost::setFocus(bool focused, bool mouseOver) {
    {
        std::lock_guard lock(state_->mutex);
        state_->focused = focused;
        state_->mouseOver = mouseOver;
        state_->dirty = true;
    }
    state_->condition.notify_all();
}

void JsfxUiHost::setMouseOver(bool mouseOver) {
    {
        std::lock_guard lock(state_->mutex);
        state_->mouseOver = mouseOver;
        state_->dirty = true;
    }
    state_->condition.notify_all();
}

void JsfxUiHost::pauseEffect() {
    std::unique_lock lock(state_->mutex);
    state_->effectPaused = true;
    state_->pendingMenu.active = false;
    state_->condition.notify_all();
    state_->condition.wait(lock, [&] {
        return state_->stopping || !state_->effectCallActive;
    });
}

void JsfxUiHost::resumeEffect() {
    {
        std::lock_guard lock(state_->mutex);
        state_->effectPaused = false;
        state_->dirty = true;
    }
    state_->condition.notify_all();
}

void JsfxUiHost::pointer(uint32_t modifiers, int32_t x, int32_t y, uint32_t buttons,
                         double wheel, double horizontalWheel) {
    {
        std::lock_guard lock(state_->mutex);
        state_->modifiers = modifiers;
        state_->mouseX = x;
        state_->mouseY = y;
        state_->buttons = buttons;
        state_->wheel += wheel;
        state_->horizontalWheel += horizontalWheel;
        state_->dirty = true;
    }
    state_->condition.notify_all();
}

void JsfxUiHost::key(uint32_t modifiers, uint32_t keyCode, bool pressed) {
    {
        std::lock_guard lock(state_->mutex);
        const uint32_t next = (state_->keyWrite + 1) % std::size(state_->keys);
        if (next != state_->keyRead) {
            state_->keys[state_->keyWrite] = {modifiers, keyCode, pressed};
            state_->keyWrite = next;
        }
        state_->dirty = true;
    }
    state_->condition.notify_all();
}

void JsfxUiHost::setDropFiles(const std::vector<std::string>& files) {
    {
        std::lock_guard lock(state_->mutex);
        state_->dropFiles = files;
        state_->dirty = true;
    }
    state_->condition.notify_all();
}

bool JsfxUiHost::pollMenu(MenuRequest& request) {
    std::lock_guard lock(state_->mutex);
    if (state_->menuRequests.empty()) return false;
    request = std::move(state_->menuRequests.front());
    state_->menuRequests.erase(state_->menuRequests.begin());
    return true;
}

void JsfxUiHost::respondMenu(uint64_t id, int32_t item) {
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->pendingMenu.active || state_->pendingMenu.id != id) return;
        state_->pendingMenu.result = item;
        state_->pendingMenu.active = false;
    }
    state_->condition.notify_all();
}

int32_t JsfxUiHost::cursor() const noexcept {
    std::lock_guard lock(state_->mutex);
    return state_->cursor;
}

void JsfxUiHost::preferredSize(uint32_t& width, uint32_t& height) const {
    std::lock_guard lock(state_->mutex);
    width = state_->width;
    height = state_->height;
}

int32_t JsfxUiHost::showMenu(void* userData, const char* specification, int32_t x, int32_t y) {
    auto* state = static_cast<State*>(userData);
    std::unique_lock lock(state->mutex);
    if (state->stopping) return 0;
    const uint64_t id = state->nextMenuId++;
    state->pendingMenu = {id, true, 0};
    state->menuRequests.push_back({id, specification ? specification : "", x, y});
    state->condition.notify_all();
    const bool answered = state->condition.wait_for(lock, std::chrono::seconds(30), [&] {
        return !state->pendingMenu.active || state->stopping;
    });
    if (!answered || state->stopping) {
        state->pendingMenu.active = false;
        return 0;
    }
    return state->pendingMenu.result;
}

void JsfxUiHost::setCursor(void* userData, int32_t cursor) {
    auto* state = static_cast<State*>(userData);
    std::lock_guard lock(state->mutex);
    state->cursor = cursor;
}

const char* JsfxUiHost::getDropFile(void* userData, int32_t index) {
    auto* state = static_cast<State*>(userData);
    std::lock_guard lock(state->mutex);
    if (index < 0) {
        state->dropFiles.clear();
        state->activeDropPath.clear();
        return nullptr;
    }
    if (static_cast<size_t>(index) >= state->dropFiles.size()) return nullptr;
    state->activeDropPath = state->dropFiles[static_cast<size_t>(index)];
    return state->activeDropPath.c_str();
}

void JsfxUiHost::run() {
    auto* state = state_;
    ysfx_gfx_config_t config{};
    config.user_data = state;
    config.show_menu = showMenu;
    config.set_cursor = setCursor;
    config.get_drop_file = getDropFile;
    auto frameInterval = std::chrono::milliseconds(33);

    while (true) {
        std::unique_lock lock(state->mutex);
        state->condition.wait_for(lock, frameInterval, [&] {
            return state->stopping || state->effectPaused ||
                   state->dirty || !state->configured;
        });
        if (state->stopping) break;
        if (state->effectPaused) {
            state->condition.notify_all();
            state->condition.wait(lock, [&] {
                return state->stopping || !state->effectPaused;
            });
            if (state->stopping) break;
        }

        ysfx_t* effect = state->effect;
        const uint32_t width = state->width;
        const uint32_t height = state->height;
        const float scale = state->scale;
        const uint32_t modifiers = state->modifiers;
        const int32_t mouseX = state->mouseX;
        const int32_t mouseY = state->mouseY;
        const uint32_t buttons = state->buttons;
        const double wheel = std::exchange(state->wheel, 0.0);
        const double horizontalWheel = std::exchange(state->horizontalWheel, 0.0);
        const bool visible = state->visible;
        const bool focused = state->focused;
        const bool mouseOver = state->mouseOver;
        const bool redrawRequested = state->dirty || !state->configured;
        const bool configure = !std::exchange(state->configured, true);
        state->dirty = false;

        State::Key keys[128];
        uint32_t keyCount = 0;
        while (state->keyRead != state->keyWrite && keyCount < std::size(keys)) {
            keys[keyCount++] = state->keys[state->keyRead];
            state->keyRead = (state->keyRead + 1) % std::size(state->keys);
        }
        ANativeWindow* window = state->window;
        if (window) ANativeWindow_acquire(window);
        state->effectCallActive = true;
        lock.unlock();

        const size_t byteCount = static_cast<size_t>(width) * height * 4;
        if (state->pixels.size() != byteCount) {
            state->pixels.resize(byteCount);
            state->converted.resize(byteCount);
        }
        if (configure) {
            config.pixel_width = width;
            config.pixel_height = height;
            config.pixel_stride = width * 4;
            config.pixels = state->pixels.data();
            config.scale_factor = scale;
            ysfx_gfx_setup(effect, &config);
            if (window) {
                ANativeWindow_setBuffersGeometry(
                    window, static_cast<int32_t>(width), static_cast<int32_t>(height),
                    WINDOW_FORMAT_RGBA_8888);
            }
        }
        for (uint32_t i = 0; i < keyCount; ++i) {
            ysfx_gfx_add_key(effect, keys[i].modifiers, keys[i].code, keys[i].pressed);
        }
        ysfx_gfx_update_mouse(
            effect, modifiers, mouseX, mouseY, buttons, wheel, horizontalWheel);
        ysfx_gfx_set_window_state(effect, focused, visible, mouseOver);
        const bool shouldRun = (visible || redrawRequested);
        const bool changed = shouldRun && ysfx_gfx_run(effect);
        const bool shouldPresent = changed || redrawRequested;
        const uint32_t requestedFps = std::clamp(ysfx_get_requested_framerate(effect), 1u, 240u);
        frameInterval = std::chrono::milliseconds(std::max(1u, 1000u / requestedFps));

        if (shouldPresent && visible && window) {
            ANativeWindow_Buffer output{};
            if (ANativeWindow_lock(window, &output, nullptr) == 0) {
                const uint32_t rows = std::min(height, static_cast<uint32_t>(output.height));
                const uint32_t columns = std::min(width, static_cast<uint32_t>(output.width));
                for (uint32_t row = 0; row < rows; ++row) {
                    const auto* source = state->pixels.data() + static_cast<size_t>(row) * width * 4;
                    auto* converted = state->converted.data() + static_cast<size_t>(row) * width * 4;
                    for (uint32_t column = 0; column < columns; ++column) {
                        converted[column * 4] = source[column * 4 + 2];
                        converted[column * 4 + 1] = source[column * 4 + 1];
                        converted[column * 4 + 2] = source[column * 4];
                        // JSFX/LICE may leave pixels with zero alpha. Android's
                        // SurfaceView is an opaque host, so make rendered RGB visible.
                        converted[column * 4 + 3] = 0xFF;
                    }
                    std::memcpy(
                        static_cast<uint8_t*>(output.bits) + static_cast<size_t>(row) * output.stride * 4,
                        converted,
                        static_cast<size_t>(columns) * 4);
                }
                ANativeWindow_unlockAndPost(window);
            }
        }

        if (window) ANativeWindow_release(window);
        lock.lock();
        state->effectCallActive = false;
        lock.unlock();
        state->condition.notify_all();
    }
}

} // namespace guitarrackcraft
