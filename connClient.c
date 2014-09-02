//function to initialize a client which contains
//the buffer and the connected fd

#include "connClient.h"
void init_client(connectClient* client, int fd_conn){
  client->connect_fd = fd_conn;
}
