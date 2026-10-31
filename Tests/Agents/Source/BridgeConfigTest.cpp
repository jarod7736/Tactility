#include "doctest.h"
#include <agents/BridgeConfig.h>

using namespace agents;

TEST_CASE("parseBridgeConfig reads url and token") {
    auto config = parseBridgeConfig("url=http://192.168.1.118:8765\ntoken=abc\n");
    REQUIRE(config.has_value());
    CHECK_EQ(config->host, "192.168.1.118");
    CHECK_EQ(config->port, 8765);
    CHECK_EQ(config->token, "abc");
}

TEST_CASE("parseBridgeConfig trims CRLF line endings and trailing spaces") {
    auto config = parseBridgeConfig("url = http://192.168.1.118:8765  \r\ntoken=abc \t\r\n");
    REQUIRE(config.has_value());
    CHECK_EQ(config->host, "192.168.1.118");
    CHECK_EQ(config->port, 8765);
    CHECK_EQ(config->token, "abc");
}

TEST_CASE("parseBridgeConfig skips comments and blank lines") {
    auto config = parseBridgeConfig("# bridge\n\nurl=http://host:1\ntoken=t\n");
    REQUIRE(config.has_value());
    CHECK_EQ(config->host, "host");
    CHECK_EQ(config->port, 1);
}

TEST_CASE("parseBridgeConfig reports a missing token") {
    CHECK_EQ(parseBridgeConfig("url=http://192.168.1.118:8765\n").error(), ConfigError::NoToken);
    CHECK_EQ(parseBridgeConfig("url=http://192.168.1.118:8765\ntoken=  \r\n").error(), ConfigError::NoToken);
}

TEST_CASE("parseBridgeConfig rejects a url without http://") {
    CHECK_EQ(parseBridgeConfig("url=192.168.1.118:8765\ntoken=abc\n").error(), ConfigError::BadUrl);
}

TEST_CASE("parseBridgeConfig rejects a url without a valid port") {
    CHECK_EQ(parseBridgeConfig("url=http://192.168.1.118\ntoken=abc\n").error(), ConfigError::BadUrl);
    CHECK_EQ(parseBridgeConfig("url=http://192.168.1.118:\ntoken=abc\n").error(), ConfigError::BadUrl);
    CHECK_EQ(parseBridgeConfig("url=http://192.168.1.118:abc\ntoken=abc\n").error(), ConfigError::BadUrl);
    CHECK_EQ(parseBridgeConfig("url=http://192.168.1.118:0\ntoken=abc\n").error(), ConfigError::BadUrl);
    CHECK_EQ(parseBridgeConfig("url=http://192.168.1.118:65536\ntoken=abc\n").error(), ConfigError::BadUrl);
    CHECK_EQ(parseBridgeConfig("url=http://:8765\ntoken=abc\n").error(), ConfigError::BadUrl);
    CHECK_EQ(parseBridgeConfig("url=http://192.168.1.118:8765/x\ntoken=abc\n").error(), ConfigError::BadUrl);
}

TEST_CASE("parseBridgeConfig reports empty text as missing") {
    CHECK_EQ(parseBridgeConfig("").error(), ConfigError::Missing);
    CHECK_EQ(parseBridgeConfig("token=abc\n").error(), ConfigError::Missing);
}

TEST_CASE("toMessage gives the user-facing text") {
    CHECK_EQ(std::string(toMessage(ConfigError::Missing)), "Bridge not configured");
    CHECK_EQ(std::string(toMessage(ConfigError::NoToken)), "Check bridge.properties: token missing");
    CHECK_EQ(std::string(toMessage(ConfigError::BadUrl)), "Check bridge.properties: url must be http://host:port");
}
