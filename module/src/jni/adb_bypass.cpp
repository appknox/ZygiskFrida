#include "adb_bypass.h"

#include <dlfcn.h>
#include <dobby.h>

#include <cstdint>
#include <cstring>

#include "log.h"

struct binder_abi {
    unsigned long write_read;  // NOLINT(runtime/int)
    uint32_t transaction;
    uint32_t transaction_sg;
    size_t binder_wr_write_size_off;
    size_t binder_wr_write_buffer_off;
    size_t transaction_data_size_off;
    size_t transaction_buffer_off;
    size_t ptr_size;
};

// Binder protocol 8 uses 64-bit binder_size_t/binder_uintptr_t. Protocol 7
// uses the legacy 32-bit layout. The request code tells us which layout the
// current process is passing to the kernel.
static constexpr binder_abi kBinder64 = {
    0xc0306201UL,  // BINDER_WRITE_READ, sizeof(binder_write_read)=48
    0x40406300,   // BC_TRANSACTION, sizeof(binder_transaction_data)=64
    0x40486311,   // BC_TRANSACTION_SG, sizeof(binder_transaction_data_sg)=72
    0,            // binder_write_read.write_size
    16,           // binder_write_read.write_buffer
    32,           // binder_transaction_data.data_size
    48,           // binder_transaction_data.data.ptr.buffer
    8,
};

static constexpr binder_abi kBinder32 = {
    0xc0186201UL,  // BINDER_WRITE_READ, sizeof(binder_write_read)=24
    0x40286300,   // BC_TRANSACTION, sizeof(binder_transaction_data)=40
    0x402c6311,   // BC_TRANSACTION_SG, sizeof(binder_transaction_data_sg)=44
    0,            // binder_write_read.write_size
    8,            // binder_write_read.write_buffer
    24,           // binder_transaction_data.data_size
    32,           // binder_transaction_data.data.ptr.buffer
    4,
};

// Settings the dev-mode RASP reads via Settings.Global.getInt(name, 0).
static const char *const kNeedles[] = {
    "adb_enabled",
    "development_settings_enabled",
    "adb_wifi_enabled",
};

static int (*real_ioctl)(int, unsigned long, void *) = nullptr;  // NOLINT(runtime/int)

static uint64_t read_sized_uint(const uint8_t *ptr, size_t size) {
    if (size == sizeof(uint32_t)) {
        uint32_t value;
        memcpy(&value, ptr, sizeof(value));
        return value;
    }
    uint64_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static uint32_t read_u32(const void *ptr) {
    return static_cast<uint32_t>(
        read_sized_uint(static_cast<const uint8_t *>(ptr), sizeof(uint32_t)));
}

static const binder_abi *binder_abi_for_request(unsigned long request) {  // NOLINT(runtime/int)
    if (request == kBinder64.write_read) {
        return &kBinder64;
    }
    if (request == kBinder32.write_read) {
        return &kBinder32;
    }
    return nullptr;
}

// Every binder BC_* command is an ordinary _IOW-encoded ioctl request code,
// which packs its payload size into bits 16-29 (see _IOC_SIZE in
// <sys/ioctl.h>). Decoding that field directly is equivalent to enumerating
// every BC_* opcode by hand and also covers opcodes this file doesn't name.
static int bc_payload_len(uint32_t cmd) {
    return static_cast<int>((cmd >> 16) & 0x3FFF);
}

// Flip the first byte of each setting name in the parcel so the query asks for
// a non-existent setting -> getInt(name, 0) returns 0. The name may be
// marshalled as UTF-16LE (writeString16) or UTF-8 (writeString8).
static void neutralize(uint8_t *buf, size_t len) {
    for (const char *needle : kNeedles) {
        size_t n = strlen(needle);
        // stride 2 is UTF-16LE ('a' 00 'd' 00 ...), stride 1 is UTF-8/ASCII.
        static const size_t kStrides[] = {2, 1};
        for (size_t stride : kStrides) {
            size_t span = n * stride;
            if (len < span) continue;
            for (size_t i = 0; i + span <= len; i++) {
                bool ok = true;
                for (size_t j = 0; j < n; j++) {
                    if (buf[i + j * stride] != (uint8_t) needle[j] ||
                        (stride == 2 && buf[i + j * stride + 1] != 0)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    buf[i] = 'X';
                    LOGI("[adb-bypass] neutralized %s (%s)", needle,
                         stride == 2 ? "utf16le" : "utf8");
                }
            }
        }
    }
}

static int hooked_ioctl(int fd, unsigned long request, void *arg) {  // NOLINT(runtime/int)
    const binder_abi *abi = binder_abi_for_request(request);
    if (abi != nullptr && arg != nullptr) {
        auto *bwr = static_cast<uint8_t *>(arg);
        uint64_t write_size = read_sized_uint(
            bwr + abi->binder_wr_write_size_off, abi->ptr_size);
        uint64_t write_buffer = read_sized_uint(
            bwr + abi->binder_wr_write_buffer_off, abi->ptr_size);
        // The outgoing command stream is write_size bytes (write_consumed is
        // only set by the kernel on return).
        if (write_size > 0 && write_buffer != 0 && write_size < 0x4000) {
            auto *cmds = reinterpret_cast<uint8_t *>(write_buffer);
            size_t off = 0;
            size_t guard = 0;
            while (off + 4 <= write_size && guard++ < 256) {
                uint32_t cmd = read_u32(cmds + off);
                int len = bc_payload_len(cmd);
                if (off + 4 + static_cast<size_t>(len) > write_size) break;
                if (cmd == abi->transaction || cmd == abi->transaction_sg) {
                    uint8_t *btd = cmds + off + 4;
                    uint64_t data_size = read_sized_uint(
                        btd + abi->transaction_data_size_off, abi->ptr_size);
                    uint64_t buffer = read_sized_uint(
                        btd + abi->transaction_buffer_off, abi->ptr_size);
                    if (buffer != 0 && data_size > 0 && data_size < 0x10000) {
                        neutralize(reinterpret_cast<uint8_t *>(buffer), data_size);
                    }
                }
                off += 4 + static_cast<size_t>(len);
            }
        }
    }
    return real_ioctl(fd, request, arg);
}

void install_adb_bypass() {
    void *sym = dlsym(RTLD_DEFAULT, "ioctl");
    if (sym == nullptr) {
        LOGE("[adb-bypass] ioctl symbol not found");
        return;
    }
    if (DobbyHook(sym, reinterpret_cast<void *>(hooked_ioctl),
                  reinterpret_cast<void **>(&real_ioctl)) == 0) {
        LOGI("[adb-bypass] ioctl hook installed @ %p", sym);
    } else {
        LOGE("[adb-bypass] DobbyHook(ioctl) failed");
    }
}
