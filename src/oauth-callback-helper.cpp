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
        return 1;
    }

    log_helper_event("received callback URL argument");

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_helper_event("WSAStartup failed");
        return 2;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        log_helper_event("socket creation failed");
        WSACleanup();
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
        return 4;
    }

    const std::string payload = std::string("{\"cmd\":\"oauth_callback\",\"url\":\"") +
        json_escape(argv[1]) + "\"}\n";
    send(sock, payload.c_str(), static_cast<int>(payload.size()), 0);

    char buffer[512];
    recv(sock, buffer, sizeof(buffer), 0);
    log_helper_event("forwarded callback to OBS on port " + std::to_string(port));
    closesocket(sock);
    WSACleanup();
    return 0;
}
