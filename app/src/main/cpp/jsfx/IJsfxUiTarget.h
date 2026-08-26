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

namespace guitarrackcraft {
class JsfxUiHost;

class IJsfxUiTarget {
public:
    virtual ~IJsfxUiTarget() = default;
    virtual bool hasJsfxGfx() const noexcept = 0;
    virtual JsfxUiHost* jsfxUiHost() noexcept = 0;
};
} // namespace guitarrackcraft
