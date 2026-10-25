#ifndef AUTH_ENV_H
#define AUTH_ENV_H
/* What kernel/auth.c calls in the kernel: the VFS, the window and font, keys and clicks, ticks, entropy and the log.
   tools/auth-host/main.c defines AUTH_HOST and its own stand-ins for these, so tools/checks/auth-check.sh compiles
   the real auth.c on the host. */
#include "libc.h"
#ifndef AUTH_HOST
#include "serial.h"
#include "app.h"
#include "window.h"
#include "font.h"
#include "vfs.h"
#include "task.h"
#include "mouse.h"
#include "entropy.h"
#include "klog.h"
#endif
#endif
