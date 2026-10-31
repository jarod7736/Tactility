#include "agents/BridgeConfig.h"

#include <Tactility/file/PropertiesFile.h>

#include <charconv>
#include <map>

namespace agents {

static std::string_view trim(std::string_view text) {
    constexpr auto whitespace = " \t\r\n";
    auto start = text.find_first_not_of(whitespace);
    if (start == std::string_view::npos) {
        return {};
    }
    auto end = text.find_last_not_of(whitespace);
    return text.substr(start, end - start + 1);
}

static std::expected<BridgeConfig, ConfigError> makeConfig(std::string_view url, std::string_view token) {
    url = trim(url);
    token = trim(token);
    if (url.empty()) {
        return std::unexpected(ConfigError::Missing);
    }

    constexpr std::string_view scheme = "http://";
    if (!url.starts_with(scheme)) {
        return std::unexpected(ConfigError::BadUrl);
    }
    auto authority = url.substr(scheme.size());
    if (authority.ends_with('/')) {
        authority.remove_suffix(1);
    }
    auto colon = authority.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::unexpected(ConfigError::BadUrl);
    }
    auto host = authority.substr(0, colon);
    auto portText = authority.substr(colon + 1);
    unsigned int port = 0;
    auto [end, error] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (portText.empty() || error != std::errc() || end != portText.data() + portText.size() || port == 0 || port > 65535) {
        return std::unexpected(ConfigError::BadUrl);
    }

    if (token.empty()) {
        return std::unexpected(ConfigError::NoToken);
    }

    return BridgeConfig {
        .host = std::string(host),
        .port = static_cast<uint16_t>(port),
        .token = std::string(token)
    };
}

std::expected<BridgeConfig, ConfigError> parseBridgeConfig(std::string_view propertiesText) {
    std::string_view url, token;
    while (!propertiesText.empty()) {
        auto newline = propertiesText.find('\n');
        auto line = trim(propertiesText.substr(0, newline));
        propertiesText = newline == std::string_view::npos ? std::string_view() : propertiesText.substr(newline + 1);

        auto equals = line.find('=');
        if (line.starts_with('#') || equals == std::string_view::npos) {
            continue;
        }
        auto key = trim(line.substr(0, equals));
        auto value = line.substr(equals + 1);
        if (key == "url") {
            url = value;
        } else if (key == "token") {
            token = value;
        }
    }
    return makeConfig(url, token);
}

std::expected<BridgeConfig, ConfigError> loadBridgeConfig() {
    std::map<std::string, std::string> properties;
    if (!tt::file::loadPropertiesFile(BRIDGE_CONFIG_PATH, properties)) {
        return std::unexpected(ConfigError::Missing);
    }
    return makeConfig(properties["url"], properties["token"]);
}

const char* toMessage(ConfigError error) {
    switch (error) {
        case ConfigError::Missing:
            return "Bridge not configured";
        case ConfigError::NoToken:
            return "Check bridge.properties: token missing";
        case ConfigError::BadUrl:
            return "Check bridge.properties: url must be http://host:port";
    }
    return "Bridge not configured";
}

}
