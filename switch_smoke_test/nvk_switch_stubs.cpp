// libnvk.a / libnvk_support.a (Mesa util code) reference a handful of POSIX
// APIs devkitA64's newlib doesn't implement, plus expat (XML) for Mesa's
// driconf per-app workaround config, which we don't ship on Switch. Most of
// this is unreachable from the actual Vulkan instance/device path this smoke
// test exercises - it lives in optional disk-cache/driconf machinery - so a
// safe "not available" stub is fine. sysconf() is the one exception:
// nvk_physical_device.c calls sysconf(_SC_PHYS_PAGES)/_SC_PAGESIZE to size
// its memory heaps and hard-fails VK_ERROR_INITIALIZATION_FAILED if that
// comes back negative (see switch-port-notes.md), so it needs a real answer.

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <csignal>
#include <dirent.h>
#include <malloc.h>
#include <pwd.h>
#include <regex.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

extern "C" {

// NAK's Rust code backs its RNG with this; svc-backed CSRNG makes it real
// rather than a stub (matches nxvk's own nvk_compat.c).
ssize_t getrandom(void* buf, size_t buflen, unsigned int flags) {
    (void)flags;
    randomGet(buf, buflen);
    return static_cast<ssize_t>(buflen);
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (alignment % sizeof(void*) != 0 || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    void* p = memalign(alignment, size);
    if (!p) {
        return ENOMEM;
    }
    *memptr = p;
    return 0;
}

long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE: // == _SC_PAGE_SIZE
        return 0x1000;
    case _SC_PHYS_PAGES: {
        u64 totalMemory = 0;
        if (R_FAILED(svcGetInfo(&totalMemory, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
            return -1;
        }
        return static_cast<long>(totalMemory / 0x1000);
    }
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
        // Horizon gives an application three cores (the fourth is reserved for
        // the system). Returning -1 here made std::thread::hardware_concurrency()
        // report 0, and every worker pool sized from it collapsed to a single
        // thread - including the shader compiler's.
        return 3;
    default:
        return -1;
    }
}

uid_t geteuid() { return 0; }
uid_t getuid() { return 0; }
gid_t getegid() { return 0; }
gid_t getgid() { return 0; }

int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
    (void)uid; (void)pwd; (void)buf; (void)buflen;
    *result = nullptr;
    return 0;
}

int flock(int fd, int operation) {
    // Single-process on Switch: no real contention, so pretend every lock succeeds.
    (void)fd; (void)operation;
    return 0;
}

int dirfd(DIR* dirp) {
    (void)dirp;
    errno = ENOSYS;
    return -1;
}

int fstatat(int fd, const char* path, struct stat* buf, int flag) {
    (void)fd; (void)path; (void)buf; (void)flag;
    errno = ENOSYS;
    return -1;
}

int regcomp(regex_t* preg, const char* pattern, int cflags) {
    // Reports "compiled fine, matches nothing" rather than "invalid pattern" -
    // matches nxvk's own nvk_compat.c, which callers (driconf app-name
    // matching, unreachable here anyway) are written to tolerate.
    (void)pattern; (void)cflags;
    if (preg) preg->re_nsub = 0;
    return 0;
}

int regexec(const regex_t* preg, const char* string, size_t nmatch, regmatch_t* pmatch, int eflags) {
    (void)preg; (void)string; (void)nmatch; (void)pmatch; (void)eflags;
    return REG_NOMATCH;
}

void regfree(regex_t* preg) { (void)preg; }

int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
    (void)how; (void)set;
    if (oldset) {
        sigemptyset(oldset);
    }
    return 0;
}

// --- expat (driconf XML parsing) ---
//
// nvk_CreateInstance unconditionally calls into Mesa's driconf loader
// (nvk_parse_dri_options -> driParseConfigFiles -> parseOneConfigFile),
// which does NOT null-check the parser handle - a null XML_ParserCreate
// return crashed inside Mesa itself (see the "raw vk probe" investigation
// in switch-port-notes.md). Since we don't ship a driconf.xml on Switch and
// don't need real XML parsing, hand back a real (trivial) allocation so
// every other call has a valid, dereferenceable-but-content-irrelevant
// pointer, and report success so Mesa's happy path completes.

void* XML_ParserCreate(const void* encoding) {
    (void)encoding;
    return std::malloc(1);
}
void XML_SetElementHandler(void* parser, void* start, void* end) { (void)parser; (void)start; (void)end; }
void XML_SetUserData(void* parser, void* userData) { (void)parser; (void)userData; }
int XML_ParseBuffer(void* parser, int len, int isFinal) {
    (void)parser; (void)len; (void)isFinal;
    return 1; // XML_STATUS_OK
}
void* XML_GetBuffer(void* parser, int len) {
    (void)parser;
    return len > 0 ? std::malloc(static_cast<size_t>(len)) : nullptr;
}
void XML_ParserFree(void* parser) { std::free(parser); }
int XML_GetErrorCode(void* parser) { (void)parser; return 0; }
const char* XML_ErrorString(int code) { (void)code; return "no error"; }

// newlib has no pipes or ownership changes on Horizon; callers only need the
// symbols to exist and to see a normal "unsupported" failure.
int pipe(int fds[2]) { (void)fds; errno = ENOSYS; return -1; }
int fchown(int fd, uid_t owner, gid_t group) { (void)fd; (void)owner; (void)group; errno = ENOSYS; return -1; }

} // extern "C"

// NVK refuses to enumerate any device unless this is set before the first
// Vulkan call (see the smoke test's README-derived note). Setting it in a
// constructor makes every binary that links NVK safe without each `main`
// having to remember.
__attribute__((constructor)) static void NvkAllowExperimentalDriver() {
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
}
