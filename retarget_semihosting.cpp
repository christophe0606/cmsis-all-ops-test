// Minimal semihosting retarget so printf/puts produce output on the FVP
// console. newlib-nano links _write/_read/_close stubs that return -1
// silently; with this override printf actually reaches the host.
//
// Uses ARM semihosting (BKPT 0xAB) directly, no .specs file or extra libs.
// Documented in "Semihosting for AArch32 and AArch64" (Arm DUI 0471).
//
// SYS_WRITE (0x05) needs a file handle opened via SYS_OPEN; the bare integer
// fd=1 newlib hands us is NOT such a handle. We lazily SYS_OPEN ":tt" with
// mode 4 ("w") on the first write to get a console handle, and fall back to
// the per-byte SYS_WRITEC if that fails.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr uint32_t kSysOpen = 0x01;
constexpr uint32_t kSysClose = 0x02;
constexpr uint32_t kSysWriteC = 0x03;
constexpr uint32_t kSysWrite0 = 0x04;
constexpr uint32_t kSysWrite = 0x05;
constexpr uint32_t kSysRead = 0x06;

// SYS_OPEN mode codes (Arm semihosting spec): 0=r, 4=w, 8=a, ... (in binary
// variants add 1 for "b"). We want "w" -> 4.
constexpr uint32_t kModeWrite = 4;

int semihosting_call(uint32_t op, const void* args) {
  register uint32_t r0 asm("r0") = op;
  register const void* r1 asm("r1") = args;
  asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
  return static_cast<int>(r0);
}

int g_stdout_handle = -1;

int ensure_stdout_handle() {
  if (g_stdout_handle >= 0) {
    return g_stdout_handle;
  }
  static const char kTt[] = ":tt";
  uint32_t args[3] = {
      reinterpret_cast<uint32_t>(kTt),
      kModeWrite,
      sizeof(kTt) - 1,  // length excluding NUL
  };
  int h = semihosting_call(kSysOpen, args);
  if (h >= 0) {
    g_stdout_handle = h;
  }
  return g_stdout_handle;
}

void semihosting_write_byte(char c) {
  uint32_t b = static_cast<uint8_t>(c);
  semihosting_call(kSysWriteC, &b);
}

}  // namespace

extern "C" int _write(int /*fd*/, const char* buf, int count) {
  int h = ensure_stdout_handle();
  if (h >= 0) {
    uint32_t args[3] = {
        static_cast<uint32_t>(h),
        reinterpret_cast<uint32_t>(buf),
        static_cast<uint32_t>(count),
    };
    int not_written = semihosting_call(kSysWrite, args);
    if (not_written <= count) {
      return count - not_written;
    }
    // SYS_WRITE failed -- fall through to per-byte fallback.
  }
  for (int i = 0; i < count; ++i) {
    semihosting_write_byte(buf[i]);
  }
  return count;
}

extern "C" int _read(int /*fd*/, char* buf, int count) {
  uint32_t args[3] = {
      0u,
      reinterpret_cast<uint32_t>(buf),
      static_cast<uint32_t>(count),
  };
  int not_read = semihosting_call(kSysRead, args);
  if (not_read < 0 || not_read > count) {
    return 0;
  }
  return count - not_read;
}

extern "C" int _close(int) { return 0; }
extern "C" int _isatty(int) { return 1; }
extern "C" off_t _lseek(int, off_t, int) { return 0; }
extern "C" int _fstat(int, struct stat* st) {
  st->st_mode = S_IFCHR;
  return 0;
}
