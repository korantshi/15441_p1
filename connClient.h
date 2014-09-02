
#include "connClient.h"
#define BUF_SIZE 4096
//header file to define a connected client
typedef struct {
  int connect_fd;
}connectClient;

void init_client(connectClient* client, int fd_conn);
