// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/common/version.h — project identification shown on the startup screen
// and in logs.

#ifndef G3_CORE_COMMON_VERSION_H
#define G3_CORE_COMMON_VERSION_H

#define G3_PROJECT_NAME "G3MacPi"
#define G3_VERSION_STRING "0.0.1-m0"

// G3_GIT_REV is passed in by the build system; fall back if absent.
#ifndef G3_GIT_REV
#define G3_GIT_REV "unknown"
#endif

#endif // G3_CORE_COMMON_VERSION_H
