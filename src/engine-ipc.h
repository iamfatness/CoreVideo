#pragma once
#define IPC_CMD_INIT        "init"
#define IPC_CMD_JOIN        "join"
#define IPC_CMD_LEAVE       "leave"
#define IPC_CMD_SUBSCRIBE   "subscribe"
#define IPC_CMD_UNSUBSCRIBE "unsubscribe"
#define IPC_CMD_QUIT        "quit"
#define IPC_EVT_READY       "ready"
#define IPC_EVT_AUTH_OK     "auth_ok"
#define IPC_EVT_AUTH_FAIL   "auth_fail"
#define IPC_EVT_JOINED      "joined"
#define IPC_EVT_LEFT        "left"
#define IPC_EVT_FRAME       "frame"
#define IPC_EVT_AUDIO       "audio"
#define IPC_EVT_ERROR       "error"
#define IPC_SHM_PREFIX "ZoomObsPlugin_"
static constexpr const char *PIPE_P2E = "\\\\.\\pipe\\ZoomObsPlugin_P2E";
static constexpr const char *PIPE_E2P = "\\\\.\\pipe\\ZoomObsPlugin_E2P";
