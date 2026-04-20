// POSIX stubs for symbols Arduino-ESP32's FS/VFS implementation
// references but the ESP32 newlib port doesn't supply.
//
// Arduino-ESP32's `libraries/FS/src/vfs_api.cpp` compiles
// `VFSImpl::rmdir()` unconditionally. Its vtable pulls the symbol
// into the final link even when application code never calls
// rmdir() — which is our case (capture.cpp only writes and rotates
// files, never removes directories). The other directory functions
// (closedir, opendir, readdir, rewinddir, seekdir, mkdir) ship as
// weak stubs in the xtensa newlib port and link cleanly with the
// "not implemented and will always fail" warning. rmdir was
// omitted from that set, which fails the final link.
//
// Provide a weak stub here so the link succeeds. If the stub is
// ever called at runtime it returns -1 with ENOSYS — matching the
// behaviour documented for the other unimplemented directory
// functions. The stub is weak so a future ESP-IDF upgrade that
// adds a real rmdir wins automatically.

#include <errno.h>

__attribute__((weak)) int rmdir(const char *path) {
  (void) path;
  errno = ENOSYS;
  return -1;
}
