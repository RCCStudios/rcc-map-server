#include "OTPPool.h"

#include <cstring>

OTPPool::OTPPool(const uint32_t timeToLive) : ttl(timeToLive) {
}

OTPPool::~OTPPool() {
}

OTP OTPPool::getOTP(UUIDv4::UUID token) {
    const UUIDv4::UUID randomKey = randomGenerator.getUUID();
    char bytes[16];
    randomKey.bytes(bytes);

    OTP otp;
    memcpy(&otp, bytes, 4);

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

    pool.emplace_front(otp, token, now + ttl);
    return otp;
}

UUIDv4::UUID OTPPool::getToken(OTP otp) {
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
