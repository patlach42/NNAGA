/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
// This is the only translation unit that reaches into ysfx's private headers.
// It is isolated so the extra include paths and -fsigned-char stay off the rest
// of plugin_abstraction, and so a ysfx submodule bump breaks one small file.
#include "JsfxYsfxInternals.h"
#include "ysfx.hpp"

namespace guitarrackcraft {

void jsfxPreallocRam(ysfx_t* fx, uint32_t items) {
    if (!fx || items == 0) return;
    NSEEL_VMCTX vm = fx->vm.get();
    if (!vm) return;
    NSEEL_VM_preallocram(vm, static_cast<int>(items));

    // calloc hands back fresh zero pages, so the allocation alone would only
    // move the malloc off the audio thread and leave the first-touch faults
    // behind. Write one word per page to fault them in here instead. The RAM is
    // already zero and @init has not run yet, so this changes no script state.
    const size_t pageBytes = 4096;
    const uint32_t itemsPerPage = static_cast<uint32_t>(pageBytes / sizeof(EEL_F));
    for (uint32_t offset = 0; offset < items; offset += itemsPerPage) {
        int available = 0;
        EEL_F* page = NSEEL_VM_getramptr_noalloc(vm, offset, &available);
        if (!page || available <= 0) continue;
        *page = 0.0;
    }
}

bool jsfxRunSliderCode(ysfx_t* fx) {
    if (!fx || !fx->code.compiled || !fx->code.slider) return false;
    NSEEL_code_execute(fx->code.slider.get());
    fx->must_compute_slider = false;
    return true;
}

} // namespace guitarrackcraft
