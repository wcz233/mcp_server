#ifndef MCP_EMBEDDED_MEP_H
#define MCP_EMBEDDED_MEP_H

#include <stdint.h>

#define MCP_MEP_VERSION 1u
#define MCP_MEP_SOF 0xA55Au

enum mcp_mep_frame_type {
    MCP_MEP_FRAME_REQ = 1,
    MCP_MEP_FRAME_RESP = 2,
    MCP_MEP_FRAME_EVENT = 3,
    MCP_MEP_FRAME_ACK = 4,
    MCP_MEP_FRAME_NAK = 5,
    MCP_MEP_FRAME_HELLO = 6,
    MCP_MEP_FRAME_CAPS = 7,
};

enum mcp_mep_command {
    MCP_MEP_CMD_PING = 0x01,
    MCP_MEP_CMD_GET_VERSION = 0x03,
    MCP_MEP_CMD_GET_STATUS = 0x04,
    MCP_MEP_CMD_GET_DEVICE_INFO = 0x10,
    MCP_MEP_CMD_GET_CAPABILITIES = 0x11,
    MCP_MEP_CMD_READ_REGISTER = 0x20,
    MCP_MEP_CMD_WRITE_REGISTER = 0x21,
    MCP_MEP_CMD_TOOL_CALL = 0x40,
    MCP_MEP_CMD_TOOL_LIST = 0x41,
    MCP_MEP_CMD_GET_TASKS = 0x50,
    MCP_MEP_CMD_CONTROL_TASK = 0x53,
};

enum mcp_mep_status {
    MCP_MEP_STATUS_OK = 0x00,
    MCP_MEP_STATUS_INVALID_CMD = 0x01,
    MCP_MEP_STATUS_INVALID_PARAM = 0x02,
    MCP_MEP_STATUS_NOT_FOUND = 0x03,
    MCP_MEP_STATUS_DENIED = 0x04,
    MCP_MEP_STATUS_TIMEOUT = 0x05,
    MCP_MEP_STATUS_BUSY = 0x06,
    MCP_MEP_STATUS_UNSUPPORTED = 0x08,
    MCP_MEP_STATUS_INTERNAL_ERROR = 0x09,
};

struct mcp_mep_header {
    uint16_t sof;
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint8_t src;
    uint8_t dst;
    uint8_t command;
    uint16_t length;
};

#endif
