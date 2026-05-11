#pragma once
#include <string>
struct ZoomPluginSettings {
    std::string sdk_key, sdk_secret, jwt_token;
    static ZoomPluginSettings load();
    void save() const;
};
