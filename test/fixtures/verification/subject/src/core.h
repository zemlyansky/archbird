#ifndef VERIFY_CORE_H
#define VERIFY_CORE_H

typedef enum {
  PORT_ADD = 0,
  PORT_TIMES,
  PORT_EXTRA,
  PORT_COUNT
} PortOps;

int port_value(PortOps op);

#endif
