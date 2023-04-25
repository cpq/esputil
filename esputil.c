// Copyright (c) 2021-2022 Cesanta
// All rights reserved
//
// Use MSVC98 for _WIN32, thus ISO C90. MCVC98 links against un-versioned
// msvcrt.dll, therefore produced .exe works everywhere.

// Needed by MSVC
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32  // Windows includes
#include <direct.h>
#include <io.h>
#include <windows.h>
#include <winsock2.h>
#define strcasecmp(x, y) _stricmp((x), (y))
#define mkdir(x, y) _mkdir(x)
#if defined(_MSC_VER) && _MSC_VER < 1700
#define snprintf _snprintf
#define inline __inline
typedef unsigned __int64 uint64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef enum { false = 0, true = 1 } bool;
#else
#include <stdbool.h>
#include <stdint.h>
#endif
#else  // UNIX includes
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

#define DEFAULT_RESET_DELAY  50

enum { READY_STDIN = 1, READY_SERIAL = 2, READY_SOCK = 4 };
//#define ALIGN(a, b) (((a) + (b) -1) / (b) * (b))

// https://datatracker.ietf.org/doc/html/rfc1055
enum { END = 192, ESC = 219, ESC_END = 220, ESC_ESC = 221 };

// SLIP state machine
struct slip {
  unsigned char *buf;  // Buffer for the network mode
  size_t size;         // Buffer size
  size_t len;          // Number of currently buffered bytes
  int mode;            // Operation mode. 0 - serial, 1 - network
  unsigned char prev;  // Previously read character
};

struct chip {
  uint32_t id;  // Chip ID, stored in the ROM address 0x40001000
#define CHIP_ID_ESP32 0x00f01d83
#define CHIP_ID_ESP32_S2 0x000007c6
#define CHIP_ID_ESP32_C3_ECO_1_2 0x6921506f
#define CHIP_ID_ESP32_C3_ECO3 0x1b31506f
#define CHIP_ID_ESP8266 0xfff0c101
#define CHIP_ID_ESP32_S3_BETA2 0xeb004136
#define CHIP_ID_ESP32_S3_BETA3 0x9
#define CHIP_ID_ESP32_C6_BETA 0x0da1806f
  const char *name;  // Chip name, e.g. "ESP32-S2"
  uint32_t bla;      // Bootloader flash offset
};

struct ctx {
  struct slip slip;        // SLIP state machine
  const char *baud;        // Baud rate, e.g. "115200"
  const char *port;        // Serial port, e.g. "/dev/ttyUSB0"
  const char *fpar;        // Flash params, e.g. "0x220"
  const char *fspi;        // Flash SPI pins: CLK,Q,D,HD,CS. E.g. "6,17,8,11,16"
  bool verbose;            // Hexdump serial comms
  int fd;                  // Serial port file descriptor
  int sock;                // UDP socket for exchanging SLIP frames when monitor
  struct sockaddr_in sin;  // UDP sockaddr of the remote peer
  struct chip chip;        // Chip descriptor
};

static struct chip s_known_chips[] = {
    {0, "Unknown", 0},
    {CHIP_ID_ESP8266, "ESP8266", 0},
    {CHIP_ID_ESP32, "ESP32", 4096},
    {CHIP_ID_ESP32_C3_ECO_1_2, "ESP32-C3-ECO2", 0},
    {CHIP_ID_ESP32_C3_ECO3, "ESP32-C3-ECO3", 0},
    {CHIP_ID_ESP32_S2, "ESP32-S2", 4096},
    {CHIP_ID_ESP32_S3_BETA2, "ESP32-S3-BETA2", 0},
    {CHIP_ID_ESP32_S3_BETA3, "ESP32-S3-BETA3", 0},
    {CHIP_ID_ESP32_C6_BETA, "ESP32-C6-BETA", 0},
};

static int s_signo;

static void slip_send(const void *buf, size_t len,
                      void (*fn)(unsigned char, void *), void *arg) {
  const unsigned char *p = buf;
  size_t i;
  fn(END, arg);
  for (i = 0; i < len; i++) {
    if (p[i] == END) {
      fn(ESC, arg);
      fn(ESC_END, arg);
    } else if (p[i] == ESC) {
      fn(ESC, arg);
      fn(ESC_ESC, arg);
    } else {
      fn(p[i], arg);
    }
  }
  fn(END, arg);
}

// Process incoming byte `c`.
// In serial mode, do nothing, return 1.
// In network mode, append a byte to the `buf` and increment `len`.
// Return size of the buffered packet when switching to serial mode, or 0
static size_t slip_recv(unsigned char c, struct slip *slip) {
  size_t res = 0;
  if (slip->mode) {
    if (slip->prev == ESC && c == ESC_END) {
      slip->buf[slip->len++] = END;
    } else if (slip->prev == ESC && c == ESC_ESC) {
      slip->buf[slip->len++] = ESC;
    } else if (c == END) {
      res = slip->len;
    } else if (c != ESC) {
      slip->buf[slip->len++] = c;
    }
    if (slip->len >= slip->size) slip->len = 0;  // Silent overflow
  }
  slip->prev = c;
  // The "END" character flips the mode
  if (c == END) slip->len = 0, slip->mode = !slip->mode;
  return res;
}

void signal_handler(int signo) { s_signo = signo; }

static int fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}

static char *hexdump(const void *buf, size_t len, char *dst, size_t dlen) {
  const unsigned char *p = (const unsigned char *) buf;
  size_t i, idx, n = 0, ofs = 0;
  char ascii[17] = "";
  if (dst == NULL) return dst;
  memset(dst, ' ', dlen);
  for (i = 0; i < len; i++) {
    idx = i % 16;
    if (idx == 0) {
      if (i > 0 && dlen > n)
        n += (size_t) snprintf(dst + n, dlen - n, "  %s\n", ascii);
      if (dlen > n)
        n += (size_t) snprintf(dst + n, dlen - n, "%04x ", (int) (i + ofs));
    }
    if (dlen < n) break;
    n += (size_t) snprintf(dst + n, dlen - n, " %02x", p[i]);
    ascii[idx] = (char) (p[i] < 0x20 || p[i] > 0x7e ? '.' : p[i]);
    ascii[idx + 1] = '\0';
  }
  while (i++ % 16) {
    if (n < dlen) n += (size_t) snprintf(dst + n, dlen - n, "%s", "   ");
  }
  if (n < dlen) n += (size_t) snprintf(dst + n, dlen - n, "  %s\n", ascii);
  if (n > dlen - 1) n = dlen - 1;
  dst[n] = '\0';
  return dst;
}

static void dump(const char *label, const uint8_t *buf, size_t len) {
  size_t n = len * 5 + 100;  // Hexdump buffer len
  char *tmp = malloc(n);     // Hexdump buffer
  printf("%s [%d bytes]\n%s\n", label, (int) len, hexdump(buf, len, tmp, n));
  free(tmp);
}

static void uart_tx(unsigned char ch, void *arg) {
  int fd = *(int *) arg;
  if (write(fd, &ch, 1) != 1) fail("failed to write %d to fd %d\n", ch, fd);
}

static void usage(struct ctx *ctx) {
  printf("Defaults: BAUD=%s, PORT=%s\n", ctx->baud, ctx->port);
  printf("Usage:\n");
  printf("  esputil [-v] [-b BAUD] [-p PORT] info\n");
  printf("  esputil [-v] [-b BAUD] [-p PORT] [-udp PORT] monitor\n");
  printf("  esputil [-v] [-b BAUD] [-p PORT] readmem ADDR SIZE\n");
  printf("  esputil [-v] [-b BAUD] [-p PORT] readflash ADDR SIZE\n");
  printf("  esputil [-v] [-b BAUD] [-p PORT] [-fp FLASH_PARAMS] ");
  printf("[-fspi FLASH_SPI] flash ADDrESS1 FILE1.bin ...\n");
  printf("  esputil [-v] [-b BAUD] [-p PORT] [-fp FLASH_PARAMS] ");
  printf("[-fspi FLASH_SPI] flash FILE.HEX\n");
  printf("  esputil [-v] mkbin FIRMWARE.ELF FIRMWARE.BIN\n");
  printf("  esputil mkhex ADDRESS1 BINFILE1 ADDRESS2 BINFILE2 ...\n");
  printf("  esputil [-tmp TMP_DIR] unhex HEXFILE\n");
  exit(EXIT_FAILURE);
}

// clang-format off
static const char *ecode_to_str(int ecode) {
  switch (ecode) {
    case 5: return "Received message is invalid";
    case 6: return "Failed to act on received message";
    case 7: return "Invalid CRC in message";
    case 8: return "Flash write error";
    case 9: return "Flash read error" ;
    case 10: return "Flash read length error";
    case 11: return "Deflate error";
    default: return "Unknown error";
  }
}

static const char *cmdstr(int code) {
  switch (code) {
    case 2: return "FLASH_BEGIN";
    case 3: return "FLASH_DATA";
    case 4: return "FLASH_END";
    case 5: return "MEM_BEGIN";
    case 6: return "MEM_END" ;
    case 7: return "MEM_DATA";
    case 8: return "SYNC";
    case 9: return "WRITE_REG";
    case 10: return "READ_REG";
    case 11: return "SPI_SET_PARAMS";
    case 13: return "SPI_ATTACH";
    case 14: return "READ_FLASH_SLOW";
    case 15: return "CHANGE_BAUD_RATE";
    default: return "CMD_UNKNOWN";
  }
}
// clang-format on

static uint8_t checksum2(uint8_t v, const uint8_t *buf, size_t len) {
  while (len--) v ^= *buf++;
  return v;
}

static uint8_t checksum(const uint8_t *buf, size_t len) {
  return checksum2(0xef, buf, len);
}

#ifdef _WIN32  // Windows - specific routines
static void sleep_ms(int milliseconds) { Sleep(milliseconds); }

static void flushio(int fd) {
  PurgeComm((HANDLE) _get_osfhandle(fd), PURGE_RXCLEAR | PURGE_TXCLEAR);
}

static void change_baud(int fd, int baud, bool verbose) {
  DCB cfg = {sizeof(cfg)};
  HANDLE h = (HANDLE) _get_osfhandle(fd);
  if (GetCommState(h, &cfg)) {
    cfg.ByteSize = 8;
    cfg.Parity = NOPARITY;
    cfg.StopBits = ONESTOPBIT;
    cfg.fBinary = TRUE;
    cfg.fParity = TRUE;
    cfg.BaudRate = baud;
    SetCommState(h, &cfg);
  } else {
    fail("GetCommState(%x): %d\n", h, GetLastError());
  }
}

static int open_serial(const char *name, int baud, bool verbose) {
  char path[100];
  COMMTIMEOUTS ct = {1, 0, 1, 0, MAXDWORD};  // 1 ms read timeout
  int fd;
  // If serial port is specified as e.g. "COM3", prepend "\\.\" to it
  snprintf(path, sizeof(path), "%s%s", name[0] == '\\' ? "" : "\\\\.\\", name);
  fd = open(path, O_RDWR | O_BINARY);
  if (fd < 0) fail("open(%s): %s\n", path, strerror(errno));
  change_baud(fd, baud, verbose);
  SetCommTimeouts((HANDLE) _get_osfhandle(fd), &ct);
  return fd;
}

static bool is_ready(int fd) {
  DWORD errors = 0;
  COMSTAT cs = {0};
  ClearCommError((HANDLE) _get_osfhandle(fd), &errors, &cs);
  return cs.cbInQue > 0;
}

static int iowait(int fd, int sock, int ms) {
  DWORD errors, flags = 0;
  int i;
  for (i = 0; i < ms && flags == 0; i++) {
    if (is_ready(fd)) flags |= READY_SERIAL;
    if (is_ready(0)) flags |= READY_STDIN;
    if (flags == 0) sleep_ms(1);
  }
  return flags;
}

static void set_rts(int fd, bool value) {
  EscapeCommFunction((HANDLE) _get_osfhandle(fd), value ? SETRTS : CLRRTS);
}

static void set_dtr(int fd, bool value) {
  EscapeCommFunction((HANDLE) _get_osfhandle(fd), value ? SETDTR : CLRDTR);
}
#else   // UNIX - specific routines
static void set_rts(int fd, bool value) {
  int v = TIOCM_RTS;
  ioctl(fd, value ? TIOCMBIS : TIOCMBIC, &v);
}

static void set_dtr(int fd, bool value) {
  int v = TIOCM_DTR;
  ioctl(fd, value ? TIOCMBIS : TIOCMBIC, &v);
}

static void set_rts_and_dtr(int fd, bool rts_val, bool dtr_val) {
  int status;
  ioctl(fd, TIOCMGET, &status);
  if (rts_val)
    status |= TIOCM_RTS;
  else
    status &= ~TIOCM_RTS;
  if (dtr_val)
    status |= TIOCM_DTR;
  else
    status &= ~TIOCM_DTR;

  ioctl(fd, TIOCMSET, &status);
}

static void flushio(int fd) { tcflush(fd, TCIOFLUSH); }

static void sleep_ms(int milliseconds) { usleep(milliseconds * 1000); }

// clang-format off
static speed_t termios_baud(int baud) {
    switch (baud) {
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
#ifndef __APPLE__
    case 460800:  return B460800;
    case 500000:  return B500000;
    case 576000:  return B576000;
    case 921600:  return B921600;
    case 1000000: return B1000000;
    case 1152000: return B1152000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 2500000: return B2500000;
    case 3000000: return B3000000;
    case 3500000: return B3500000;
    case 4000000: return B4000000;
#endif
    default:      return B0;
    }
}
// clang-format on

static void change_baud(int fd, int baud, bool verbose) {
  struct termios tio;
  if (tcgetattr(fd, &tio) != 0)
    fail("Can't set fd %d to baud %d: %d\n", fd, baud, errno);
  cfsetospeed(&tio, termios_baud(baud));
  cfsetispeed(&tio, termios_baud(baud));
  tcsetattr(fd, TCSANOW, &tio);
  if (verbose) printf("fd %d set to baud %d\n", fd, baud);
}

static int open_serial(const char *name, int baud, bool verbose) {
  struct termios tio;
  int fd = open(name, O_RDWR | O_NOCTTY | O_SYNC);
  if (fd < 0) {
    fail("open(%s): %d (%s)\n", name, fd, strerror(errno));
  } else if (tcgetattr(fd, &tio) == 0) {
    tio.c_iflag = 0;                     // input mode
    tio.c_oflag = 0;                     // output mode
    tio.c_lflag = 0;                     // local flags
    tio.c_cflag = CLOCAL | CREAD | CS8;  // control flags
    // Order is important: setting speed must go after setting flags,
    // becase (depending on implementation) speed flags could reside in flags
    cfsetospeed(&tio, termios_baud(baud));
    cfsetispeed(&tio, termios_baud(baud));
    tcsetattr(fd, TCSANOW, &tio);
  }
  if (verbose) printf("Opened %s @ %d fd=%d\n", name, baud, fd);
  return fd;
}

// Return true if port is readable (has data), false otherwise
static int iowait(int fd, int sock, int ms) {
  int ready = 0;
  struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
  fd_set rset;
  FD_ZERO(&rset);
  FD_SET(0, &rset);   // Listen to stdin too
  FD_SET(fd, &rset);  // Listen to the UART fd
  if (sock > 0) FD_SET(sock, &rset);
  if (select((fd > sock ? fd : sock) + 1, &rset, 0, 0, &tv) < 0) FD_ZERO(&rset);
  if (FD_ISSET(0, &rset)) ready |= READY_STDIN;
  if (FD_ISSET(fd, &rset)) ready |= READY_SERIAL;
  if (sock > 0 && FD_ISSET(sock, &rset)) ready |= READY_SOCK;
  return ready;
}
#endif  // End of UNIX-specific routines

static void hard_reset(int fd) {
  set_dtr(fd, false);  // IO0 -> HIGH
  set_rts(fd, true);   // EN -> LOW
  sleep_ms(100);       // Wait
  set_rts(fd, false);  // EN -> HIGH
}

static void reset_to_bootloader_usb_jtag_serial(int fd) {
  set_rts(fd, false);
  set_dtr(fd, false);
  sleep_ms(100);
  set_dtr(fd, true);
  set_rts(fd, false);
  sleep_ms(100);
  set_rts(fd, true);
  set_dtr(fd, false);
  set_rts(fd, true);
  sleep_ms(100);
  set_dtr(fd, false);
  set_rts(fd, false);
}

static void reset_to_bootloader(int fd, int delay_ms) {
  sleep_ms(100);       // Wait
  set_dtr(fd, false);  // IO0 -> HIGH
  set_rts(fd, true);   // EN -> LOW
  sleep_ms(100);       // Wait
  set_dtr(fd, true);   // IO0 -> LOW
  set_rts(fd, false);  // EN -> HIGH
  sleep_ms(delay_ms);  // Wait
  set_dtr(fd, false);  // IO0 -> HIGH
}

// From `UnixTightReset()` method of `reset.py` of `esptool`.
// https://github.com/espressif/esptool/blob/8f8f50817eb36cadff1301b687789b6c6ebbd71e/esptool/reset.py#L76
static void unix_tight_reset_to_bootloader(int fd, int delay_ms) {
  set_rts_and_dtr(fd, false, false);
  set_rts_and_dtr(fd, true, true);
  set_rts_and_dtr(fd, true, false);   // IO0=HIGH & EN=LOW, chip in reset
  sleep_ms(100);
  set_rts_and_dtr(fd, false, true);   // IO0=LOW & EN=HIGH, chip out of reset
  sleep_ms(delay_ms);
  set_rts_and_dtr(fd, false, false);  // IO0=HIGH, done
  set_dtr(fd, false);                 // Needed in some environments to ensure IO0=HIGH
}

// From `_construct_reset_strategy_sequence()` method of `loader.py` of `esptool`.
// https://github.com/espressif/esptool/blob/8f8f50817eb36cadff1301b687789b6c6ebbd71e/esptool/loader.py#L574
static void reset_strategy(int fd) {
  static int count = 0;
  #ifdef _WIN32
  if (count > 2)  count = 0;

  if (count == 0) {
    reset_to_bootloader_usb_jtag_serial(fd);
  } else if (count == 1) {
    reset_to_bootloader(fd, DEFAULT_RESET_DELAY);
  } else {
    reset_to_bootloader(fd, DEFAULT_RESET_DELAY + 50);
  }

  #else // For UNIX
  if (count > 4)  count = 0;

  if (count == 0) {
    reset_to_bootloader_usb_jtag_serial(fd);
  } else if (count == 1) {
    unix_tight_reset_to_bootloader(fd, DEFAULT_RESET_DELAY);
  } else if (count == 2) {
    unix_tight_reset_to_bootloader(fd, DEFAULT_RESET_DELAY + 50);
  } else if (count == 3) {
    reset_to_bootloader(fd, DEFAULT_RESET_DELAY);
  } else {
    reset_to_bootloader(fd, DEFAULT_RESET_DELAY + 50);
  }
  #endif

  ++count;
}

// Execute serial command.
// Return 0 on sucess, or error code on failure
static int cmd(struct ctx *ctx, uint8_t op, void *buf, uint16_t len,
               uint32_t cs, int timeout_ms) {
  uint8_t tmp[8 + 16384];     // 8 is size of the header
  memset(tmp, 0, 8);          // Clear header
  tmp[1] = op;                // Operation
  memcpy(&tmp[2], &len, 2);   // Length
  memcpy(&tmp[4], &cs, 4);    // Checksum
  memcpy(&tmp[8], buf, len);  // Data

  slip_send(tmp, 8 + len, uart_tx, &ctx->fd);        // Send command
  if (ctx->verbose) dump(cmdstr(op), tmp, 8 + len);  // Hexdump if required

  for (;;) {
    int i, n, ready, eofs, ecode;
    ready = iowait(ctx->fd, ctx->sock, timeout_ms);  // Wait for data
    if (!(ready & READY_SERIAL)) return 1;           // Interrupted, fail
    n = read(ctx->fd, tmp, sizeof(tmp));             // Read from a device
    if (n <= 0) fail("Serial line closed\n");        // Doh. Unplugged maybe?
    // if (ctx->verbose) dump("--RAW_RESPONSE:", tmp, n);
    for (i = 0; i < n; i++) {
      size_t r = slip_recv(tmp[i], &ctx->slip);  // Pass to SLIP state machine
      // if (r == 0 && ctx->slip.mode == 0) putchar(tmp[i]);  // In serial mode
      if (r == 0) continue;
      if (ctx->verbose) dump("--SLIP_RESPONSE:", ctx->slip.buf, r);
      if (r < 10 || ctx->slip.buf[0] != 1 || ctx->slip.buf[1] != op) continue;
      // ESP8266's error indicator is in the 2 last bytes, ESP32's - last 4
      eofs =
          ctx->chip.id == 0 || ctx->chip.id == CHIP_ID_ESP8266 ? r - 2 : r - 4;
      ecode = ctx->slip.buf[eofs] ? ctx->slip.buf[eofs + 1] : 0;
      if (ecode) printf("error %d: %s\n", ecode, ecode_to_str(ecode));
      return ecode;
    }
  }
  return 42;
}

static int read32(struct ctx *ctx, uint32_t addr, uint32_t *value) {
  int ok = cmd(ctx, 10, &addr, sizeof(addr), 0, 100);
  if (ok == 0 && value != NULL) *value = *(uint32_t *) &ctx->slip.buf[4];
  return ok;
}

// Read chip ID from ROM and setup ctx->chip pointer
static void chip_detect(struct ctx *ctx) {
  size_t i, nchips;
  uint32_t chipid;
  if (read32(ctx, 0x40001000, &chipid)) fail("Error reading chip ID\n");
  nchips = sizeof(s_known_chips) / sizeof(s_known_chips[0]);
  for (i = 0; i < nchips; i++) {
    if (s_known_chips[i].id == chipid) {
      if (ctx->chip.id && ctx->chip.id != chipid) {
        fail("Chip specified (%s) does not match chip detected (%s)\n",
             ctx->chip.name, s_known_chips[i].name);
      }
      ctx->chip = s_known_chips[i];
      return;
    }
  }
  fail("Unknown chip ID: %08x\n", chipid);
}

static void set_chip_from_string(struct ctx *ctx, const char *name) {
  size_t i, nchips;
  nchips = sizeof(s_known_chips) / sizeof(s_known_chips[0]);
  for (i = 0; i < nchips; i++) {
    if (strcasecmp(s_known_chips[i].name, name) == 0) {
      ctx->chip = s_known_chips[i];
      return;
    }
  }
  fail("Unknown chip type: %s\n", name);
}

// Assume chip is rebooted and is in download mode.
// Send SYNC commands until success, and detect chip ID
static bool chip_connect(struct ctx *ctx) {
  int i, j;
  for (j = 0; j < 6; j++) {
    // Alternate different reset methods
    reset_strategy(ctx->fd);
    flushio(ctx->fd);
    for (i = 0; i < 2 + j; i++) {
      uint8_t data[36] = {7, 7, 0x12, 0x20};     // SYNC command
      memset(data + 4, 0x55, sizeof(data) - 4);  // Fill with 0x55
      if (cmd(ctx, 8, data, sizeof(data), 0, 100) == 0) {
        sleep_ms(50);
        flushio(ctx->fd);  // Discard all data
        chip_detect(ctx);
        return true;
      }
    }
  }
  return false;
}

static void monitor(struct ctx *ctx) {
  int i, ready = iowait(ctx->fd, ctx->sock, 1000);
  if (ready & READY_SERIAL) {
    uint8_t buf[BUFSIZ];
    int n = read(ctx->fd, buf, sizeof(buf));   // Read from a device
    if (n <= 0) fail("Serial line closed\n");  // If serial is closed, exit

    if (n > 0 && ctx->verbose) dump("READ", buf, n);
    for (i = 0; i < n; i++) {
      size_t len = slip_recv(buf[i], &ctx->slip);            // Pass to SLIP
      if (len == 0 && ctx->slip.mode == 0) putchar(buf[i]);  // In serial mode
      if (len <= 0) continue;
      if (len > 0 && ctx->slip.mode && ctx->sock)
        sendto(ctx->sock, ctx->slip.buf, ctx->slip.len, 0,
               (struct sockaddr *) &ctx->sin, sizeof(ctx->sin));
      if (ctx->verbose) dump("SR", ctx->slip.buf, len);
    }
    fflush(stdout);
  }
  if (ready & READY_STDIN) {  // Forward stdin to a device
    uint8_t buf[BUFSIZ];
    int n = read(0, buf, sizeof(buf));
    if (n > 0 && ctx->verbose) dump("WRITE", buf, n);
    for (i = 0; i < n; i++) uart_tx(buf[i], &ctx->fd);
  }
  if (ready & READY_SOCK) {  // Something in the UDP socket
    uint8_t buf[2048];
    unsigned sl = sizeof(ctx->sin);
    int n = recvfrom(ctx->sock, buf, sizeof(buf), 0,
                     (struct sockaddr *) &ctx->sin, &sl);
    // printf("GOT %d\n", n);
    if (n > 0) {
      if (ctx->verbose) dump("RSOCK", buf, n);
      slip_send(buf, n, uart_tx, &ctx->fd);  // Inject frame
    }
  }
}

static void info(struct ctx *ctx) {
  if (!chip_connect(ctx)) fail("Error connecting\n");
  printf("Chip ID: 0x%x (%s)\n", ctx->chip.id, ctx->chip.name);

  uint32_t efuse_base, reg_off_1, reg_off_2, mac0, mac1;
  uint32_t uart_clkdiv_reg, uart_clkdiv, xtal_clkdiv;
  float est_xtal;

  if (ctx->chip.id == CHIP_ID_ESP8266) {
    // For ESP8266
    xtal_clkdiv = 2;
  } else {
    // For ESP32*
    xtal_clkdiv = 1;
  }

  if (ctx->chip.id == CHIP_ID_ESP32_C3_ECO3) {
    efuse_base = 0x60008800;
    reg_off_1 = 0x44;
    reg_off_2 = 0x48;
    uart_clkdiv_reg = 0x60000014;
  } else if (ctx->chip.id == CHIP_ID_ESP32_S2) {
    efuse_base = 0x3F41A044;
    reg_off_1 = 0x44;
    reg_off_2 = 0x48;
    uart_clkdiv_reg = 0x3F400014;
  } else if (ctx->chip.id == CHIP_ID_ESP32_S3_BETA3) {
    efuse_base = 0x60007000;
    reg_off_1 = 0x44;
    reg_off_2 = 0x48;
    uart_clkdiv_reg = 0x60000014;
  } else if (ctx->chip.id == CHIP_ID_ESP32) {
    efuse_base = 0x3FF5A000;
    reg_off_1 = 0x04;
    reg_off_2 = 0x08;
    uart_clkdiv_reg = 0x3FF40014;
  } else {
    return;
  }

  read32(ctx, efuse_base + reg_off_1, &mac0);
  read32(ctx, efuse_base + reg_off_2, &mac1);
  printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", (mac1 >> 8) & 255,
          mac1 & 255, (mac0 >> 24) & 255, (mac0 >> 16) & 255,
          (mac0 >> 8) & 255, mac0 & 255);

  read32(ctx, uart_clkdiv_reg, &uart_clkdiv);
  uart_clkdiv &= 0xFFFFF;
  est_xtal = (atoi(ctx->baud) * uart_clkdiv) / 1e6 / xtal_clkdiv;
  printf("Detected xtal freq: %.2fMHz\n", (double)est_xtal);

  hard_reset(ctx->fd);
}

static void readmem(struct ctx *ctx, const char **args) {
  if (!chip_connect(ctx)) {
    fail("Error connecting\n");
  } else if (args[0] == NULL || args[1] == NULL) {
    usage(ctx);
  } else {
    uint32_t i, value, base = strtoul(args[0], NULL, 0),
                       size = strtoul(args[1], NULL, 0);
    for (i = 0; i < size; i += 4) {
      if (read32(ctx, base + i, &value) == 0) {
        fwrite(&value, 1, sizeof(value), stdout);
      } else {
        fprintf(stderr, "Error: mem read @ addr %#x\n", base + i);
        break;
      }
    }
  }

  hard_reset(ctx->fd);
}

static void spiattach(struct ctx *ctx) {
  uint32_t d3[] = {0, 0};
  uint32_t d4[] = {0, 4 * 1024 * 1024, 65536, 4096, 256, 0xffff};
  if (ctx->fspi != NULL) {
    // 6,17,8,11,16 -> 0xb408446, like esptool does
    unsigned a = 0, b = 0, c = 0, d = 0, e = 0;
    sscanf(ctx->fspi, "%u,%u,%u,%u,%u", &a, &b, &c, &e, &d);
    d3[0] = a | (b << 6) | (c << 12) | (d << 18) | (e << 24);
    // printf("-----> %u,%u,%u,%u,%u -> %x\n", a, b, c, d, e, pins);
  }
  if (cmd(ctx, 13, d3, sizeof(d3), 0, 250)) fail("SPI_ATTACH failed\n");
  // flash_id, flash size, block_size, sector_size, page_size, status_mask
  if (cmd(ctx, 11, d4, sizeof(d4), 0, 250)) fail("SPI_SET_PARAMS failed\n");
}

static void readflash(struct ctx *ctx, const char **args) {
  if (!chip_connect(ctx)) {
    fail("Error connecting\n");
  } else if (args[0] == NULL || args[1] == NULL) {
    usage(ctx);
  } else if (ctx->chip.id == CHIP_ID_ESP8266) {
    fail("Can't do it on esp8266\n");
  } else {
    uint32_t i = 0, base = strtoul(args[0], NULL, 0),
             size = strtoul(args[1], NULL, 0);
    spiattach(ctx);
    while (i < size) {
      uint32_t bs = size - i > 64 ? 64 : size - i;
      uint32_t d[] = {base + i, bs};
      if (cmd(ctx, 14, d, sizeof(d), 0, 500) != 0) {
        printf("Error: flash read @ addr %#x\n", base + i);
        break;
      } else {
        fwrite(&ctx->slip.buf[8], 1, bs, stdout);
        i += bs;
      }
    }
  }

  hard_reset(ctx->fd);
}

static inline unsigned long hex_to_ul(const char *s, int len) {
  unsigned long i = 0, v = 0;
  for (i = 0; i < (unsigned long) len; i++) {
    int c = s[i];
    if (i > 0) v <<= 4;
    v |= (c >= '0' && c <= '9')   ? c - '0'
         : (c >= 'A' && c <= 'F') ? c - '7'
                                  : c - 'W';
  }
  return v;
}

static int rmrf(const char *dirname) {
#ifdef _WIN32
  char tmp[MAX_PATH], path[MAX_PATH];
  WIN32_FIND_DATA data;
  HANDLE hFind;
  snprintf(tmp, sizeof(tmp), "%s\\*", dirname);
  hFind = FindFirstFile(tmp, &data);
  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      struct _stat st;
      snprintf(path, sizeof(path), "%s/%s", dirname, data.cFileName);
      if (data.cFileName[0] == '.') continue;
      if (_stat(path, &st) == 0 && (st.st_mode & S_IFDIR)) rmrf(path);
      remove(path);
    } while (FindNextFile(hFind, &data));
    FindClose(hFind);
  }
  RemoveDirectory(dirname);
  return _access(dirname, 0) != 0;
#else
  DIR *dp = opendir(dirname);
  if (dp != NULL) {
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
      struct stat st;
      char path[PATH_MAX];
      if (de->d_name[0] == '.') continue;
      snprintf(path, sizeof(path), "%s/%s", dirname, de->d_name);
      if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) rmrf(path);
      remove(path);
    }
    closedir(dp);
  }
  (void) rmdir(dirname);
  return access(dirname, 0) != 0;
#endif
}

// Unpack hex file into a given directory, as a collection of OFFSET.bin files
// If buf is not null, append all created file names to it.
static int unhex(const char *hexfile, const char *dir, char *buf, size_t bl) {
  char tmp[600];
  int c, n = 0, line = 0;
  FILE *in = fopen(hexfile, "rb"), *out = NULL;
  unsigned long upper = 0, next = 0;
  if (in == NULL) return fail("ERROR: cannot open %s\n", hexfile);
  if (rmrf(dir) == 0) return fail("Cannot delete dir %s\n", dir);
  mkdir(dir, 0755);
  buf[0] = '\0';
  while ((c = fgetc(in)) != EOF) {
    if (!isspace(c)) tmp[n++] = c;
    if (n >= (int) sizeof(tmp) || c == '\n') {
      int i, len = hex_to_ul(tmp + 1, 2);
      unsigned long lower = hex_to_ul(tmp + 3, 4);
      int type = hex_to_ul(tmp + 7, 2);
      unsigned long addr = upper | lower;
      if (tmp[0] != ':') return fail("line %d: no colon\n", line);
      if (n != 1 + 2 + 4 + 2 + len * 2 + 2)
        return fail("line %d: len %d, expected %d\n", n,
                    1 + 2 + 4 + 2 + len * 2 + 2);
      if (type == 0) {
        if (out == NULL || next != addr) {
          char path[200];
          snprintf(path, sizeof(path), "%s/%#lx.bin", dir, addr);
          if (out != NULL) fclose(out);
          out = fopen(path, "wb");
          if (out == NULL) return fail("Cannot open %s", path);
          // Append created filename to the list of created files
          snprintf(buf + strlen(buf), bl - strlen(buf), "%s%s",
                   buf[0] == '\0' ? "" : " ", path);
        }
        for (i = 0; i < len; i++) {
          int byte = hex_to_ul(tmp + 9 + i * 2, 2);
          fputc(byte, out);
        }
        next = addr + len;
      } else if (type == 1) {
        if (out != NULL) fclose(out);
        out = NULL;
      } else if (type == 4) {
        upper = hex_to_ul(tmp + 9, 4) << 16;
      }
      n = 0;
    }
  }
  fclose(in);
  if (out != NULL) fclose(out);
  return EXIT_SUCCESS;
}

static int has_suffix(const char *word, const char *suffix) {
  size_t word_len = strlen(word), suffix_len = strlen(suffix);
  return word_len > suffix_len &&
         strcasecmp(&word[word_len - suffix_len], suffix) == 0;
}

static void flashbin(struct ctx *ctx, uint16_t flash_params,
                     uint32_t flash_offset, const char *path) {
  FILE *fp = fopen(path, "rb");
  int i, n, size, seq = 0;
  uint32_t block_size = 4096, hs = 16, encrypted = 0, cs, tmp;
  uint8_t buf[16 + 4096];  // First 16 bytes are for serial cmd

  if (fp == NULL) fail("Cannot open %s: %s\n", path, strerror(errno));
  fseek(fp, 0, SEEK_END);
  size = ftell(fp);
  rewind(fp);

  memset(buf, 0, hs);  // Clear them

  printf("Erasing %d bytes @ %#x", size, flash_offset);
  fflush(stdout);

  {
    uint32_t num_blocks = (size + block_size - 1) / block_size;
    uint32_t d1[] = {size, num_blocks, block_size, flash_offset, encrypted};
    uint16_t d1size = sizeof(d1) - 4;
    // Flash begin. S2, S3, C3 chips have an extra 5th parameter.
    if (ctx->chip.id == CHIP_ID_ESP32_S2 ||
        ctx->chip.id == CHIP_ID_ESP32_S3_BETA2 ||
        ctx->chip.id == CHIP_ID_ESP32_S3_BETA3 ||
        ctx->chip.id == CHIP_ID_ESP32_C6_BETA ||
        ctx->chip.id == CHIP_ID_ESP32_C3_ECO_1_2 ||
        ctx->chip.id == CHIP_ID_ESP32_C3_ECO3)
      d1size += 4;
    if (cmd(ctx, 2, d1, d1size, 0, 15000)) fail("\nerase failed\n");
  }

  // Read from file into a buffer, but skip initial 16 bytes
  while ((n = fread(buf + hs, 1, block_size, fp)) > 0) {
    int oft = ftell(fp);
    for (i = 0; i < 100; i++) putchar('\b');
    printf("Writing %s, %d/%d bytes @ 0x%x (%d%%)", path, n, size,
           flash_offset + oft - n, oft * 100 / size);
    fflush(stdout);

    // Embed flash params into a bootloader image
    if (seq == 0 && flash_offset == ctx->chip.bla) {
      if (flash_params != 0) {
        buf[hs + 2] = (uint8_t) ((flash_params >> 8) & 255);
        buf[hs + 3] = (uint8_t) (flash_params & 255);
      }
      // Set chip type in the extended header at offset 4.
      // Common header is 8, plus extended header offset 4 = 12
      if (ctx->chip.id == CHIP_ID_ESP32_C3_ECO3) buf[hs + 12] = 5;
      if (ctx->chip.id == CHIP_ID_ESP32_C3_ECO_1_2) buf[hs + 12] = 5;
      if (ctx->chip.id == CHIP_ID_ESP32_S2) {
        buf[hs + 8] = 0;
        buf[hs + 12] = 2;
      }
    }

    // Align buffer to block_size and pad with 0xff
    // memset(buf + hs + n, 255, sizeof(buf) - hs - n);
    // n = ALIGN(n, block_size);

    // Flash write
    tmp = n, memcpy(&buf[0], &tmp, 4);      // Set buffer size
    tmp = seq++, memcpy(&buf[4], &tmp, 4);  // Set sequence number
    cs = checksum(buf + hs, n);
    if (cmd(ctx, 3, buf, (uint16_t) (hs + n), cs, 1500))
      fail("flash_data failed\n");
  }

  for (i = 0; i < 100; i++) printf("\b \b");
  printf("Written %s, %d bytes @ %#x\n", path, size, flash_offset);
  fclose(fp);
}

static const char *download(const char *url) {
  char cmd[2048];
  const char *slash = strrchr(url, '/');
  if (slash == NULL) fail("Invalid URL: %s\n", url);
  snprintf(cmd, sizeof(cmd), "curl -sL %s -o %s", url, slash + 1);
  printf("%s\n", cmd);
  if (system(cmd) != 0) fail("Download failed\n");
  return slash + 1;
}

static void flash(struct ctx *ctx, const char **args) {
  uint16_t flash_params = 0;
  if (!chip_connect(ctx)) fail("Error connecting\n");
  if (ctx->fpar != NULL) flash_params = (uint16_t) strtoul(ctx->fpar, NULL, 0);
  if (atoi(ctx->baud) > 115200) {
    uint32_t data[] = {atoi(ctx->baud), 0};
    if (cmd(ctx, 15, data, sizeof(data), 0, 50)) fail("SET_BAUD failed\n");
    change_baud(ctx->fd, atoi(ctx->baud), ctx->verbose);
  }

  // For non-ESP8266, SPI attach is mandatory
  if (ctx->chip.id != CHIP_ID_ESP8266) {
    spiattach(ctx);

    // Load first word from the bootloader - flash params are encoded there,
    // in the last 2 bytes, see README.md in the repo root
    if (ctx->fpar == NULL) {
      uint32_t d5[] = {ctx->chip.bla, 16};
      if (cmd(ctx, 14, d5, sizeof(d5), 0, 2000) != 0) {
        printf("Error: can't read bootloader @ addr %#x\n", ctx->chip.bla);
      } else if (ctx->slip.buf[8] != 0xe9) {
        printf("Wrong magic for bootloader @ addr %#x\n", ctx->chip.bla);
      } else {
        flash_params = (ctx->slip.buf[10] << 8) | ctx->slip.buf[11];
      }
    }
  }
  printf("Using flash params %#hx\n", flash_params);

  // Iterate over arguments: FLASH_OFFSET FILENAME ...
  while (args[0]) {
    if (has_suffix(args[0], ".hex")) {
      // A .hex file is fed to us. Unhex it first into a temp dir
      char file_list[8192], tmpdir[1024], *s = file_list;
      size_t n;

      bool is_url = (strncmp(args[0], "http", 4) == 0);
      if (is_url) args[0] = download(args[0]);

      snprintf(tmpdir, sizeof(tmpdir), "%s.tmp", args[0]);
      unhex(args[0], tmpdir, file_list, sizeof(file_list));
      // Now iterate over the unhexed files, and flash each
      while ((n = strcspn(s, " ")) > 0) {
        char *slash, *p = s + n;
        while (*p == ' ') *p++ = '\0';
        slash = strrchr(s, '/');
        flashbin(ctx, flash_params, strtoul(slash ? slash + 1 : s, NULL, 0), s);
        s = p;
      }
      if (is_url) remove(args[0]);  // Remove downloaded file
      rmrf(tmpdir);                 // Cleanup temp dir
      args += 1;                    // Move to next file
    } else if (args[1] != NULL) {
      bool is_url = (strncmp(args[0], "http", 4) == 0);
      if (is_url) args[1] = download(args[1]);
      flashbin(ctx, flash_params, strtoul(args[0], NULL, 0), args[1]);
      if (is_url) remove(args[0]);  // Remove downloaded file
      args += 2;
    }
  }

  {
    // Flash end
    uint32_t d3[] = {0};  // 0: reboot, 1: run user code
    if (cmd(ctx, 4, d3, sizeof(d3), 0, 250)) fail("flash_end failed\n");
  }

  hard_reset(ctx->fd);
}

static unsigned long align_to(unsigned long n, unsigned to) {
  return ((n + to - 1) / to) * to;
}

////////////////////////////////// mkbin command - ELF related functionality

struct mem {
  unsigned char *ptr;
  int len;
};

struct Elf32_Ehdr {
  unsigned char e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct Elf32_Phdr {
  uint32_t p_type, p_offset, p_vaddr, p_paddr;
  uint32_t p_filesz, p_memsz, p_flags, p_align;
};

static struct mem read_entire_file(const char *path) {
  struct mem mem;
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) fail("Cannot open %s: %s\n", path, strerror(errno));
  fseek(fp, 0, SEEK_END);
  mem.len = ftell(fp);
  rewind(fp);
  mem.ptr = malloc(mem.len);
  if (mem.ptr == NULL) fail("malloc(%d) failed\n", mem.len);
  if (fread(mem.ptr, 1, mem.len, fp) != (size_t) mem.len) {
    fail("fread(%s) failed: %s\n", path, strerror(errno));
  }
  fclose(fp);
  return mem;
}

static int elf_get_num_segments(const struct mem *elf) {
  struct Elf32_Ehdr *e = (struct Elf32_Ehdr *) elf->ptr;
  return e->e_phnum;
}

static uint32_t elf_get_entry_point(const struct mem *elf) {
  return ((struct Elf32_Ehdr *) elf->ptr)->e_entry;
}

static struct Elf32_Phdr elf_get_phdr(const struct mem *elf, int no) {
  struct Elf32_Ehdr *e = (struct Elf32_Ehdr *) elf->ptr;
  struct Elf32_Phdr *h = (struct Elf32_Phdr *) (elf->ptr + e->e_phoff);
  if (h->p_filesz == 0) no++;  // GCC-generated phdrs have empty 1st phdr
  return h[no];
}

static int mkbin(const char *elf_path, const char *bin_path, struct ctx *ctx) {
  struct mem elf = read_entire_file(elf_path);
  FILE *bin_fp = fopen(bin_path, "w+b");
  uint8_t common_hdr[] = {0xe9, 1, 0, 0};
  uint8_t extended_hdr[] = {0xee, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t i, j, cs = 0xef, zero = 0, num_segments = elf_get_num_segments(&elf);
  uint32_t entrypoint = elf_get_entry_point(&elf);

  // ESP32S2
  if (ctx->chip.id == 0x000007c6) {
    extended_hdr[0] = 0x00;
    extended_hdr[4] = 2;
  }

  if (bin_fp == NULL) fail("Cannot open %s: %s\n", bin_path, strerror(errno));
  if (elf.len < (int) sizeof(struct Elf32_Phdr)) fail("corrupt ELF file\n");
  if (elf.ptr[4] != '\x01') fail("Not ELF32: %d\n", elf.ptr[4]);

  // GCC generates 2 segments. TCC - 4, first two are .text and .data
  // num_segments = 2;
  common_hdr[1] = num_segments;
  fwrite(common_hdr, 1, sizeof(common_hdr), bin_fp);      // Common header
  fwrite(&entrypoint, 1, sizeof(entrypoint), bin_fp);     // Entry point
  fwrite(extended_hdr, 1, sizeof(extended_hdr), bin_fp);  // Extended header
  if (ctx->verbose)
    printf("%s: %d segments found\n", elf_path, (int) num_segments);

  // Iterate over segments
  for (i = 0; i < num_segments; i++) {
    struct Elf32_Phdr h = elf_get_phdr(&elf, i);
    uint32_t load_address = h.p_vaddr;
    uint32_t aligned_size = align_to(h.p_filesz, 4);
    if (ctx->verbose) printf("  addr %x size %u\n", load_address, aligned_size);
    fwrite(&load_address, 1, sizeof(load_address), bin_fp);
    fwrite(&aligned_size, 1, sizeof(aligned_size), bin_fp);
    fwrite(elf.ptr + h.p_offset, 1, h.p_filesz, bin_fp);
    for (j = 0; j < aligned_size - h.p_filesz; j++) fputc(zero, bin_fp);
    cs = checksum2(cs, elf.ptr + h.p_offset, h.p_filesz);
  }

  {
    // Pad to 16 bytes and write checksum
    long ofs = ftell(bin_fp), aligned_ofs = align_to(ofs + 1, 16);
    for (i = 0; i < aligned_ofs - ofs - 1; i++) fputc(zero, bin_fp);
    fputc(cs, bin_fp);
  }

  fclose(bin_fp);
  free(elf.ptr);
  return EXIT_SUCCESS;
}
///////////////////////////////////////////////// End of mkbin command

static void printhexline(int type, int len, int addr, int *buf) {
  unsigned i, cs = type + len + ((addr >> 8) & 255) + (addr & 255);
  printf(":%02x%04x%02x", len, addr & 0xffff, type);
  for (i = 0; i < (unsigned) len; i++) cs += buf[i], printf("%02x", buf[i]);
  printf("%02x\n", (~cs + 1) & 255);
}

static void printhexhiaddrline(unsigned long addr) {
  int buf[2] = {(addr >> 24) & 255, (addr >> 16) & 255};
  printhexline(4, 2, 0, buf);
}

static int mkhex(const char **args) {
  for (; args[0] && args[1]; args += 2) {
    unsigned long addr = strtoul(args[0], NULL, 0);
    FILE *fp = fopen(args[1], "rb");
    int c, n = 0, buf[16];
    if (fp == NULL) return fail("ERROR: cannot open %s\n", args[1]);
    // if (addr >= 0xffff && (addr & 0xffff)) printhexhiaddrline(addr);
    printhexhiaddrline(addr);
    for (;;) {
      if ((c = fgetc(fp)) != EOF) buf[n++] = c;
      if (n >= (int) (sizeof(buf) / sizeof(buf[0])) || c == EOF) {
        if (addr >= 0xffff && !(addr & 0xffff)) printhexhiaddrline(addr);
        if (n > 0) printhexline(0, n, addr & 0xffff, buf);
        addr += n;
        n = 0;
      }
      if (c == EOF) break;
    }
    fclose(fp);
  }
  printhexline(1, 0, 0, NULL);
  return EXIT_SUCCESS;
}

static int open_udp_socket(const char *portspec) {
  int sock;
  struct sockaddr_in sin;
  sin.sin_family = AF_INET;
  sin.sin_port = htons((unsigned short) atoi(portspec));
  sin.sin_addr.s_addr = 0;
  sock = socket(AF_INET, SOCK_DGRAM, 17);
  bind(sock, (struct sockaddr *) &sin, sizeof(sin));
  return sock;
}

int main(int argc, const char **argv) {
  const char *temp_dir = getenv("TMP_DIR");   // Temp dir for unhex
  const char *udp_port = getenv("UDP_PORT");  // Listening UDP port
  const char **command = NULL;                // Command to perform
  uint8_t slipbuf[32 * 1024];                 // Buffer for SLIP context
  struct ctx ctx = {0};                       // Program context
  int i;

  ctx.port = getenv("PORT");          // Serial port
  ctx.port = getenv("PORT");          // Serial port
  ctx.baud = getenv("BAUD");          // Serial port baud rate
  ctx.fpar = getenv("FLASH_PARAMS");  // Flash parameters
  ctx.fspi = getenv("FLASH_SPI");     // Flash SPI pins
  ctx.verbose = getenv("V") != NULL;  // Verbose output
  ctx.slip.buf = slipbuf;             // Set SLIP context - buffer
  ctx.slip.size = sizeof(slipbuf);    // Buffer size
  ctx.chip = s_known_chips[0];        // Set chip to unknown

#ifdef _WIN32
  if (ctx.port == NULL) ctx.port = "COM99";  // Non-existent default port
#elif defined(__APPLE__)
  if (ctx.port == NULL) ctx.port = "/dev/cu.usbmodem";
#else
  if (ctx.port == NULL) ctx.port = "/dev/ttyUSB0";
#endif

  if (ctx.baud == NULL) ctx.baud = "115200";  // Default baud rate
  if (temp_dir == NULL) temp_dir = "tmp";     // Default temp dir
  if (udp_port == NULL) udp_port = "1999";    // Default UDP_PORT

  // Parse options
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      ctx.baud = argv[++i];
    } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      ctx.port = argv[++i];
    } else if (strcmp(argv[i], "-fp") == 0 && i + 1 < argc) {
      ctx.fpar = argv[++i];
    } else if (strcmp(argv[i], "-fspi") == 0 && i + 1 < argc) {
      ctx.fspi = argv[++i];
    } else if (strcmp(argv[i], "-chip") == 0 && i + 1 < argc) {
      set_chip_from_string(&ctx, argv[++i]);
    } else if (strcmp(argv[i], "-tmp") == 0 && i + 1 < argc) {
      temp_dir = argv[++i];
    } else if (strcmp(argv[i], "-udp") == 0 && i + 1 < argc) {
      udp_port = argv[++i];
    } else if (strcmp(argv[i], "-v") == 0) {
      ctx.verbose = true;
    } else if (argv[i][0] == '-') {
      usage(&ctx);
    } else {
      command = &argv[i];
      break;
    }
  }
  if (!command || !*command) usage(&ctx);

  // Commands that do not require serial port
  if (strcmp(*command, "mkbin") == 0) {
    if (!command[1] || !command[2]) usage(&ctx);
    return mkbin(command[1], command[2], &ctx);
  } else if (strcmp(*command, "mkhex") == 0) {
    return mkhex(&command[1]);
  } else if (strcmp(*command, "unhex") == 0) {
    char file_list[500];
    return unhex(command[1], temp_dir, file_list, sizeof(file_list));
  }

  // Commands that require serial port. First, open serial.
  ctx.sock = open_udp_socket(udp_port);
  ctx.fd = open_serial(ctx.port, 115200, ctx.verbose);
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  if (strcmp(*command, "info") == 0) {
    info(&ctx);
  } else if (strcmp(*command, "flash") == 0) {
    flash(&ctx, &command[1]);
  } else if (strcmp(*command, "readmem") == 0) {
    readmem(&ctx, &command[1]);
  } else if (strcmp(*command, "readflash") == 0) {
    readflash(&ctx, &command[1]);
  } else if (strcmp(*command, "monitor") == 0) {
    if (atoi(ctx.baud) != 115200) {
      change_baud(ctx.fd, atoi(ctx.baud), ctx.verbose);
    }
    while (s_signo == 0) monitor(&ctx);
  } else {
    printf("Unknown command: %s\n", *command);
    usage(&ctx);
  }
  close(ctx.fd);
  return 0;
}
