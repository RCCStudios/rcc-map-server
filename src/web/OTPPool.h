#pragma once

#include <uuid_v4.h>

#include <list>
#include <chrono>
#include <mutex>

using OTP = uint32_t;

class OTPPool {
    private:
    std::mutex mutex;

    uint32_t maxSize;
    uint32_t ttl;

    UUIDv4::UUIDGenerator<std::mt19937_64> randomGenerator;
    std::list<std::tuple<OTP, UUIDv4::UUID, std::time_t>> pool{};

    public:
    explicit OTPPool(uint32_t maxSize, uint32_t timeToLive);
    ~OTPPool();

    OTP getOTP(UUIDv4::UUID token);
    UUIDv4::UUID getToken(OTP otp);
};
