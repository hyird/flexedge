#include <dlfcn.h>
#include <netdb.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Process-local test shim only. Native getaddrinfo allocates every result, so the real
// freeaddrinfo remains valid. Production binaries never link or load this target.
extern "C" int getaddrinfo(const char* host, const char* service, const addrinfo* hints, addrinfo** result) {
    using resolve = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
    static const auto native = reinterpret_cast<resolve>(dlsym(RTLD_NEXT, "getaddrinfo"));
    if (!host || std::strcmp(host, "flexedge-dns.test") != 0) {
        return native(host, service, hints, result);
    }
    const auto* path = std::getenv("FLEXEDGE_TEST_DNS_FILE");
    auto* file = path ? std::fopen(path, "r") : nullptr;
    if (!file) {
        return EAI_AGAIN;
    }
    char answers[256]{};
    const auto count = std::fread(answers, 1, sizeof(answers) - 1, file);
    std::fclose(file);
    if (count == 0 || std::strcmp(answers, "error") == 0) {
        return EAI_AGAIN;
    }
    if (std::strcmp(answers, "slow") == 0) {
        ::usleep(150000);
        return native("127.0.0.1", service, hints, result);
    }
    *result = nullptr;
    addrinfo** tail = result;
    char* next = answers;
    while (next && *next) {
        char* address = next;
        next = std::strchr(next, ',');
        if (next) {
            *next++ = '\0';
        }
        const auto status = native(address, service, hints, tail);
        if (status != 0) {
            if (*result) {
                freeaddrinfo(*result);
                *result = nullptr;
            }
            return status;
        }
        while (*tail) {
            tail = &(*tail)->ai_next;
        }
    }
    return 0;
}
