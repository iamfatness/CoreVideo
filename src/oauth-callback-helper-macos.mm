#import <Cocoa/Cocoa.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
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

static int read_control_port()
{
    const char *home = std::getenv("HOME");
    if (!home || !*home)
        return 19870;

    const std::string path =
        std::string(home) + "/Library/Application Support/obs-studio/global.ini";
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

static void forward_url(NSURL *url)
{
    if (!url) return;

    const char *raw = [[url absoluteString] UTF8String];
    if (!raw || !*raw) return;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(read_control_port()));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
        const std::string payload = std::string("{\"cmd\":\"oauth_callback\",\"url\":\"") +
            json_escape(raw) + "\"}\n";
        send(fd, payload.c_str(), payload.size(), 0);
        char buffer[512];
        recv(fd, buffer, sizeof(buffer), 0);
    }

    close(fd);
}

@interface CoreVideoOAuthCallbackDelegate : NSObject <NSApplicationDelegate>
@end

@implementation CoreVideoOAuthCallbackDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
}

- (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls
{
    (void)application;
    for (NSURL *url in urls)
        forward_url(url);
    [NSApp terminate:nil];
}

@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        CoreVideoOAuthCallbackDelegate *delegate =
            [[CoreVideoOAuthCallbackDelegate alloc] init];
        [app setDelegate:delegate];

        for (int i = 1; i < argc; ++i) {
            NSString *arg = [NSString stringWithUTF8String:argv[i]];
            if ([arg hasPrefix:@"corevideo://"]) {
                forward_url([NSURL URLWithString:arg]);
                return 0;
            }
        }

        [app run];
    }
    return 0;
}
