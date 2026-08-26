/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * This file is part of NNAGA.
 * NNAGA is free software under the GNU General Public License version 3.
 */
#pragma once
#if defined(__ANDROID__)
/* Android bionic accepts the mutex API but does not implement priority
 * protocols on all API levels. ysfx only uses the attribute as an optional
 * optimization, so leave it unchanged and report success. */
#define pthread_mutexattr_setprotocol(attr, protocol) (0)
#endif
