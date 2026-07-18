#include "core.h"

static const int op_values[PORT_COUNT] = {
    [PORT_ADD] = 1,
    [PORT_TIMES] = 9,
    [PORT_EXTRA] = PORT_EXTRA,
};

#define OPSET(name, ...) static const PortOps name[] = {__VA_ARGS__}
OPSET(PORT_REQUIRED, PORT_ADD, PORT_EXTRA);

int port_value(PortOps op) { return op_values[op]; }
