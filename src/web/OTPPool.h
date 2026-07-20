#pragma once

#include <uuid_v4.h>

#include <list>
#include <chrono>

using OTP = uint32_t;

class OTPPool {
    private:
    uint32_t ttl;

    UUIDv4::UUIDGenerator<std::mt19937_64> randomGenerator;
    std::list<std::tuple<OTP, UUIDv4::UUID, std::time_t>> pool{};

    public:
    OTPPool(uint32_t timeToLive);
    ~OTPPool();

    OTP getOTP(UUIDv4::UUID token);
    UUIDv4::UUID getToken(OTP otp);
};
