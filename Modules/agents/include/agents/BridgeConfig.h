#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace agents {

/** Location of the bridge settings on the SD card. Keys: url, token. */
constexpr auto BRIDGE_CONFIG_PATH = "/sdcard/settings/bridge.properties";

struct BridgeConfig {
    std::string host;
    uint16_t port;
    std::string token;
};

enum class ConfigError {
    Missing, // no file, or no url
    NoToken,
    BadUrl // url is not http://host:port
};

/** Parse the text of bridge.properties. Keys and values are trimmed (spaces, tabs, CR). */
std::expected<BridgeConfig, ConfigError> parseBridgeConfig(std::string_view propertiesText);

/** Read and parse BRIDGE_CONFIG_PATH. */
std::expected<BridgeConfig, ConfigError> loadBridgeConfig();

/** User-facing message for an error. */
const char* toMessage(ConfigError error);

}
