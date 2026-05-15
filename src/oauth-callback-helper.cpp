#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <windows.h>

static void log_helper_event(const std::string &message)
{
    const char *temp = std::getenv("TEMP");
    if (!temp || !*temp)
        return;

    std::ofstream log(std::string(temp) + "\\CoreVideoOAuthCallback.log",
                      std::ios::app);
    if (log)
        log << message << "\n";
}

static std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

static std::string json_unescape(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    bool escaped = false;
    for (char c : in) {
        if (escaped) {
            switch (c) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += c; break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else {
            out += c;
        }
    }
    return out;
}

static std::string json_string_value(const std::string &json,
                                     const std::string &key)
{
    const std::string needle = "\"" + key + "\":\"";
    const size_t start = json.find(needle);
    if (start == std::string::npos)
        return {};

    std::string value;
    bool escaped = false;
    for (size_t i = start + needle.size(); i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            value += '\\';
            value += c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return json_unescape(value);
        } else {
            value += c;
        }
    }
    return {};
}

static int read_control_port()
{
    const char *appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata)
        return 19870;

    const std::string path = std::string(appdata) + "\\obs-studio\\global.ini";
    std::ifstream file(path);
    if (!file)
        return 19870;

    bool in_zoom_section = false;
    std::string line;
    while (std::getline(file, line)) {
        if (line == "[ZoomPlugin]") {
            in_zoom_section = true;
            continue;
        }
        if (in_zoom_section && !line.empty() && line[0] == '[')
            break;
        if (!in_zoom_section)
            continue;

        const std::string key = "ControlServerPort=";
        if (line.rfind(key, 0) != 0)
            continue;

        try {
            const int port = std::stoi(line.substr(key.size()));
            if (port >= 1024 && port <= 65535)
                return port;
        } catch (...) {
            return 19870;
        }
    }

    return 19870;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        log_helper_event("missing callback URL argument");
        MessageBoxA(nullptr,
                    "CoreVideo OAuth callback did not receive a callback URL.",
                    "CoreVideo Zoom OAuth",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }

    log_helper_event("received callback URL argument");

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_helper_event("WSAStartup failed");
        MessageBoxA(nullptr,
                    "CoreVideo OAuth callback could not initialize networking.",
                    "CoreVideo Zoom OAuth",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 2;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        log_helper_event("socket creation failed");
        WSACleanup();
        MessageBoxA(nullptr,
                    "CoreVideo OAuth callback could not create a local socket.",
                    "CoreVideo Zoom OAuth",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 3;
    }

    const int port = read_control_port();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        log_helper_event("connect failed on port " + std::to_string(port));
        closesocket(sock);
        WSACleanup();
        const std::string message =
            "Zoom returned to CoreVideo, but OBS was not listening on "
            "127.0.0.1:" + std::to_string(port) +
            ". Open OBS with the CoreVideo plugin loaded, then authorize again.";
        MessageBoxA(nullptr, message.c_str(), "CoreVideo Zoom OAuth",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 4;
    }

    const std::string payload = std::string("{\"cmd\":\"oauth_callback\",\"url\":\"") +
        json_escape(argv[1]) + "\"}\n";
    send(sock, payload.c_str(), static_cast<int>(payload.size()), 0);

    char buffer[2048];
    const int received = recv(sock, buffer, sizeof(buffer), 0);
    std::string response;
    if (received > 0)
        response.assign(buffer, buffer + received);

    log_helper_event("forwarded callback to OBS on port " + std::to_string(port));
    if (!response.empty())
        log_helper_event("OBS callback response: " + response);

    closesocket(sock);
    WSACleanup();

    if (response.find("\"ok\":true") != std::string::npos) {
        MessageBoxA(nullptr,
                    "Zoom authorization completed. Return to OBS to continue.",
                    "CoreVideo Zoom OAuth",
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }

    std::string error = json_string_value(response, "error");
    if (error.empty())
        error = "OBS did not report a successful OAuth callback.";
    MessageBoxA(nullptr, error.c_str(), "CoreVideo Zoom OAuth",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return 0;
}
