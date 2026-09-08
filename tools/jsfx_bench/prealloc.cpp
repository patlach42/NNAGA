#include "ysfx.hpp"
// Prealloc the whole EEL RAM budget off the audio thread, so __NSEEL_RAMAlloc
// never callocs from inside @sample.
extern "C" void ysfx_prealloc_ram(ysfx_t* fx){
    NSEEL_VM_preallocram(fx->vm.get(), -1);
}
