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
#pragma once

#include <android/native_window.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ysfx.h>

namespace guitarrackcraft {

class JsfxUiHost final {
public:
    explicit JsfxUiHost(ysfx_t* effect);
    ~JsfxUiHost();
    JsfxUiHost(const JsfxUiHost&) = delete;
    JsfxUiHost& operator=(const JsfxUiHost&) = delete;

    void attachWindow(ANativeWindow* window);
    void detachWindow();
    void resize(uint32_t width, uint32_t height, float scale = 1.0f);
    void setVisible(bool visible);
    void setFocus(bool focus, bool mouseOver = false);
    void setMouseOver(bool over);
    void pauseEffect();
    void resumeEffect();
    void pointer(uint32_t modifiers, int32_t x, int32_t y, uint32_t buttons,
                 double wheel = 0.0, double horizontalWheel = 0.0);
    void key(uint32_t modifiers, uint32_t keyCode, bool pressed);
    void setDropFiles(const std::vector<std::string>& files);

    struct MenuRequest { uint64_t id; std::string spec; int32_t x; int32_t y; };
    bool pollMenu(MenuRequest& request);
    void respondMenu(uint64_t id, int32_t item);
    int32_t cursor() const noexcept;
    void preferredSize(uint32_t& width, uint32_t& height) const;

private:
    struct State;
    static int32_t showMenu(void*, const char*, int32_t, int32_t);
    static void setCursor(void*, int32_t);
    static const char* getDropFile(void*, int32_t);
    void run();
    void stop();
    State* state_;
};

} // namespace guitarrackcraft
