#pragma once

#include <fmt/core.h>
#include <fmt/chrono.h>
#include <fmt/color.h>

#define RM_LOG_AUTO_PREFIX __PRETTY_FUNCTION__

#define RM_LOG_LEVEL_PREFIX_TRACE " [TRACE] "
#define RM_LOG_LEVEL_PREFIX_DEBUG " [DEBUG] "
#define RM_LOG_LEVEL_PREFIX_INFO " [INFO]  "
#define RM_LOG_LEVEL_PREFIX_WARN fmt::format(fg(fmt::color::gold), " [WARN]  ")
#define RM_LOG_LEVEL_PREFIX_ERROR fmt::format(fg(fmt::color::indian_red), " [ERROR] ")
#define RM_LOG_LEVEL_PREFIX_FATAL fmt::format(fg(fmt::color::red), " [FATAL] ")

#define RM_LOG(__logLevelPrefix__, __prefix__, __message__)                                                             \
    do                                                                                                                  \
    {                                                                                                                   \
        fmt::print("[{}] {}[{}]: {}\n", std::chrono::system_clock::now(), __logLevelPrefix__, __prefix__, __message__); \
    } while (0)

#define RM_DECLARE_SINGLETON(__className__, ...)                   \
private:                                                           \
    static std::unique_ptr<__className__> singletonInstance;       \
    static bool isInitialized;                                     \
                                                                   \
public:                                                            \
    __className__() {}                                             \
    int init(__VA_ARGS__);                                         \
    int destroy();                                                 \
                                                                   \
    __className__(const __className__ &) = delete;                 \
    __className__ &operator=(const __className__ &) = delete;      \
                                                                   \
    static __className__ *getInstance()                            \
    {                                                              \
        if (!singletonInstance)                                    \
        {                                                          \
            singletonInstance = std::make_unique<__className__>(); \
        }                                                          \
        return singletonInstance.get();                            \
    }                                                              \
                                                                   \
    static void releaseInstance()                                  \
    {                                                              \
        singletonInstance.reset();                                 \
    }

#define RM_HTTP_CODE_OK 200
#define RM_HTTP_CODE_BAD_REQUEST 400
#define RM_HTTP_CODE_UNAUTHORIZED 418
#define RM_HTTP_CODE_INTERNAL_ERROR 500

#define RM_ERROR_CODE_THRESHOLD 600

#define RM_ERROR_CODE_FILE_NOT_FOUND 700

#define RM_ERROR_CODE_ALREADY_INITIALIZED 800
#define RM_ERROR_CODE_NOT_INITIALIZED 801

#define RM_ERROR_CODE_LIB_STD 900
#define RM_ERROR_CODE_LIB_YAML 901
#define RM_ERROR_CODE_LIB_SQLITE 902
#define RM_ERROR_CODE_LIB_HTTP 903
#define RM_ERROR_CODE_LIB_JSON 904

#define RM_ERROR_CODE_UNKNOWN 1000

inline void replaceInString(std::string &string, const std::string &from, const std::string &to) {
    size_t startPos = 0;

    while ((startPos = string.find(from, startPos)) != std::string::npos) {
        string.replace(startPos, from.length(), to);
        startPos += to.length();
    }
}

inline std::vector<std::string> splitString(std::string string, const char token) {
    std::vector<std::string> result;

    size_t pos = string.find(token);
    size_t initialPos = 0;

    while (pos != std::string::npos) {
        result.push_back(string.substr(initialPos, pos - initialPos));
        initialPos = pos + 1;

        pos = string.find(token, initialPos);
    }

    result.push_back(string.substr(initialPos, std::min(pos, string.size()) - initialPos));

    return result;
}

inline bool checkStringForValidHex(std::string string, const uint32_t len = 0) {
    if (len != 0 and string.size() != len) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse string (\"{}\") as a HEX string: failed length test: must be {}, got {}", string, string.size(), len));
        return false;
    }

    const std::vector<char> hexCharacters = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

    for (const char &character: string) {
        if (std::find(begin(hexCharacters), end(hexCharacters), character) == end(hexCharacters)) {
            RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse string (\"{}\") as a HEX string: invalid character '{}' for a HEX number", string, character));
            return false;
        }
    }
    return true;
}

inline bool checkStringForValidHexColor(std::string string) {
    if (string.empty() or string[0] != '#') {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse string (\"{}\") as a HEX color. Note: expected format is \"#rrggbb\"", string));
        return false;
    }
    return checkStringForValidHex(string.substr(1), 6);
}

template<class T>
inline bool hexStringToInt(std::string string, T &result, const uint32_t len = 0) {
    if (ceil(string.length() / 2.0f) > sizeof(T)) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse string (\"{}\") as a HEX: return type has not enough capacity for {} bytes of data (max {})", string, ceil(string.size() / 2.0f), sizeof(T)));
        return false;
    }

    if (not checkStringForValidHex(string, len)) {
        return false;
    }

    std::stringstream ss;
    ss << std::hex << string;
    ss >> result;

    return true;
}

template<class T>
inline bool intToHexString(T val, std::string &result, uint32_t len = 0) {
    if (len == 0) {
        len = sizeof(T) * 2;
    }
    if (len < 2 * sizeof(T)) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to convert int {} to a HEX string: required length is less than max possible value of given type (must be at least {})", val, sizeof(T) *2 ));
        return false;
    }

    std::stringstream stream;
    stream << std::setfill('0') << std::setw(len) << std::hex << val;
    result = std::string(stream.str());

    return true;
}

inline int64_t stringToTimeInterval(std::string string) {
    std::vector<std::string> values = splitString(string, ':');

    if (values.size() != 4) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse string (\"{}\") as a time interval: found {} values instead if 4. Note: expected format is \"DD:hh:mm:ss\"", string, values.size()));
        return -1;
    }

    return (std::time_t) (std::stoi(values[0]) * 86400 + std::stoi(values[1]) * 3600 + std::stoi(values[2]) * 60 + std::stoi(values[3]));
}

inline std::time_t iso8601ToUnixtime(std::string iso8601) {
    if (iso8601.length() != 20) {
        RM_LOG(RM_LOG_LEVEL_PREFIX_ERROR, RM_LOG_AUTO_PREFIX, fmt::format("Failed to parse string (\"{}\") as a datetime. Note: expected format is \"YYYY-MM-DDThh:mm:ssZ\" (see ISO 8601)", iso8601));
        return -1;
    }

    tm t{};
    if (!strptime(iso8601.c_str(), "%FT%TZ", &t)) {
        return -1;
    }
    return mktime(&t);
}

inline std::string unixtimeToIso8601(std::time_t unixtime) {
    char buffer[20];
    strftime(buffer, 20, "%FT%TZ", gmtime(&unixtime));
    return std::string(buffer) + "Z";
}
