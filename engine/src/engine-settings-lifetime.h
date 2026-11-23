#pragma once

// SDK 7.1.5 may return a nonfunctional audio context after destroying and
// recreating ISettingService. Share one service across auth, inventory, and
// joins; explicitly reset only at SDK shutdown, never on meeting leave.
template<class Service>
class EngineSettingsLifetime {
public:
    EngineSettingsLifetime() = default;
    EngineSettingsLifetime(const EngineSettingsLifetime &) = delete;
    EngineSettingsLifetime &operator=(const EngineSettingsLifetime &) = delete;
    template<class Create> Service *get(int &result, Create create) {
        result = 0;
        if (!service_) result = create(&service_);
        return service_;
    }
    template<class Destroy> void reset(Destroy destroy) {
        if (service_) destroy(service_);
        service_ = nullptr;
    }
private:
    Service *service_ = nullptr;
};
