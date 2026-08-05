#include "OTPPool.h"

#include <cstring>

#include "../common.h"

OTPPool::OTPPool(const uint32_t maxSize, const uint32_t timeToLive) : maxSize(maxSize), ttl(timeToLive) {
}

OTPPool::~OTPPool() {
}

OTP OTPPool::getOTP(UUIDv4::UUID token) {
    std::lock_guard<std::mutex> lock(mutex);

    const std::time_t now = std::time(nullptr);

    while (true) {
        if (pool.empty()) {
            break;
        }
        if (auto entry = pool.back(); std::get<2>(entry) < now) {
            pool.pop_back();
            continue;
        }
        break;
    }

    if (pool.size() > maxSize) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_WARN, RM_LOG_AUTO_PREFIX, "OTP pool overloaded");
        return 0;
    }

    const UUIDv4::UUID randomKey = randomGenerator.getUUID();
    char bytes[16];
    randomKey.bytes(bytes);

    OTP otp;
    memcpy(&otp, bytes, 4);

    pool.emplace_front(otp, token, now + ttl);
    return otp;
}

UUIDv4::UUID OTPPool::getToken(OTP otp) {
    std::lock_guard<std::mutex> lock(mutex);

    UUIDv4::UUID token(0, 0);
    const std::time_t now = std::time(nullptr);

    for (auto entry : pool) {
        if (std::get<2>(entry) < now) {
            break;
        }
        if (std::get<0>(entry) == otp) {
            token = std::get<1>(entry);
            pool.remove(entry);
            break;
        }
    }

    return token;
}
