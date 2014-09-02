/******************************************************************************
* echo_server.c                                                               *
*                                                                             *
* Description: This file contains the C source code for an echo server.  The  *
*              server runs on a hard-coded port and simply write back anything*
*              sent to it by connected clients.  It does not support          *
*              concurrent clients.                                            *
*                                                                             *
* Authors: Athula Balachandran <abalacha@cs.cmu.edu>,                         *
*          Wolf Richter <wolf@cs.cmu.edu>                                     *
*                                                                             *
*******************************************************************************/

#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "connClient.h"
#define ECHO_PORT 9999
#define BUF_SIZE 4096

//define the pool of client descriptors
typedef struct {
  int fd_max;
  fd_set read_set;
  fd_set readys;
  int num_ready;
  int index_max;
  connectClient* clients[FD_SETSIZE];
} client_pool;

int close_socket(int sock)
{
    if (close(sock))
    {
        fprintf(stderr, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}

//function to initialize the client pool
void init_client_pool(int listen_fd, client_pool* pool){
  //at start, no fd is connected
  int itr;
  pool->index_max = -1;
  for(itr = 0; itr < FD_SETSIZE; itr++){
    init_client(pool->clients[itr], -1);
  }
  pool->fd_max = listen_fd;
  FD_ZERO(&(pool->read_set));
  FD_SET(listen_fd, &(pool->read_set));
}

//function to add new connected client
void connectClient_add(int conn_fd, client_pool* pool){
  int itr;
  pool->num_ready = (pool->num_ready) - 1;
  for(itr = 0; itr < FD_SETSIZE; itr++){
    //find an idle slot
    if(((pool->clients[itr])->connect_fd) == -1){
      (pool->clients[itr])->connect_fd = conn_fd;

      //update the fd readset
      FD_SET(conn_fd, &(pool->read_set));

      //update max fd and max index
      if(itr > (pool->index_max)){
	pool->index_max = itr;
      }

      if(conn_fd > (pool->fd_max)){
	pool->fd_max = conn_fd;
      }
      break;
    } 
  }

  if(itr == FD_SETSIZE){
    fprintf(stderr, "the client list is full, cannot add moren\n");
  }
}

//function to echo to clients
void echo_clients(client_pool* pool, int sock){
  int itr = 0;
  int conn_fd;
  char buf[BUF_SIZE];
  ssize_t readret;
  while((itr <= (pool->index_max)) && ((pool->num_ready) > 0)){
    conn_fd = (pool->clients[itr])->connect_fd;
    if((conn_fd > 0) && (FD_ISSET(conn_fd, &(pool->readys)))){
      //if the connect fd is ready, start echo
      pool->num_ready = (pool->num_ready) - 1;
      readret = 0;

      if((readret = recv(conn_fd, buf, BUF_SIZE, 0)) > 1){
	if (send(conn_fd, buf, readret, 0) != readret){
	  close_socket(conn_fd);
	  close_socket(sock);
	  fprintf(stderr, "Error sending to client.\n");
	  return EXIT_FAILURE;
	}
	memset(buf, 0, BUF_SIZE);
      }

      else if (readret == -1){
	close_socket(conn_fd);
	close_socket(sock);
	fprintf(stderr, "Error reading from client socket.\n");
	return EXIT_FAILURE;
      }

      else if (readret == 0){
	(pool->clients[itr])->connect_fd = -1;
	FD_CLR(conn_fd, &(pool->read_set));
	if (close_socket(conn_fd)){
	  close_socket(sock);
	  fprintf(stderr, "Error closing client socket.\n");
	  return EXIT_FAILURE;
	}
      }
    }
    itr++;
  }

}

int main(int argc, char* argv[])
{
    int sock, client_sock;
    ssize_t readret;
    socklen_t cli_size;
    struct sockaddr_in addr, cli_addr;
    static client_pool pool;

    fprintf(stdout, "----- Echo Server -----\n");
    
    /* all networked programs must create a socket */
    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1)
    {
        fprintf(stderr, "Failed creating socket.\n");
        return EXIT_FAILURE;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    /* servers bind sockets to ports---notify the OS they accept connections */
    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)))
    {
        close_socket(sock);
        fprintf(stderr, "Failed binding socket.\n");
        return EXIT_FAILURE;
    }


    if (listen(sock, 5))
    {
        close_socket(sock);
        fprintf(stderr, "Error listening on socket.\n");
        return EXIT_FAILURE;
    }

    init_client_pool(sock, &pool);

    /* finally, loop waiting for input and then write it back */
    while (1){
      pool.readys = pool.read_set;
      pool.num_ready = select(pool.fd_max + 1, &(pool.readys), NULL, NULL,
			      NULL);
      if(pool.num_ready < 0){
	close(sock);
	fprintf(stderr, "Error in select method.\n");
	return EXIT_FAILURE;
      }

      //check readiness of listening fd
      if(FD_ISSET(sock, &(pool.readys))){
	cli_size = sizeof(cli_addr);
        if ((client_sock = accept(sock, (struct sockaddr *) &cli_addr,
				  &cli_size)) == -1){
	  close(sock);
	  fprintf(stderr, "Error accepting connection.\n");
	  return EXIT_FAILURE;
	}
	connectClient_add(client_sock, &pool);
      }
      echo_clients(&pool, sock);
    }

    close_socket(sock);

    return EXIT_SUCCESS;
}
