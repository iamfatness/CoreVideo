#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <string>

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

int main(int argc, char **argv)
{
    if (argc < 2) return 1;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 2;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return 3;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19870);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
        return 4;
    }

    const std::string payload = std::string("{\"cmd\":\"oauth_callback\",\"url\":\"") +
        json_escape(argv[1]) + "\"}\n";
    send(sock, payload.c_str(), static_cast<int>(payload.size()), 0);

    char buffer[512];
    recv(sock, buffer, sizeof(buffer), 0);
    closesocket(sock);
    WSACleanup();
    return 0;
}
