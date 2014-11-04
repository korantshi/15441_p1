

#include "peer.h"

void peer_run(bt_config_t *config);

int main(int argc, char **argv)
{
  bt_config_t config;

  bt_init(&config, argc, argv);

  DPRINTF(DEBUG_INIT, "peer.c main beginning\n");

  config.identity = 1; // your group number here
  strcpy(config.chunk_file, "chunkfile");
  strcpy(config.has_chunk_file, "haschunks");

  bt_parse_command_line(&config);

  bt_dump_config(&config);

  init(&config);
  peer_run(&config);
  return 0;
}

void init(bt_config_t *config)
{
  int i;
  fillChunkList(&masterChunk, MASTER, config->chunk_file);
  fillChunkList(&hasChunk, HAS, config->has_chunk_file);
  fillPeerList(config);

  //I assume there is only one sending host on each machine.
  //So the file is not open on 'append' mode. Because if it is,
  //multiple hosts can mess up the indentation while writing to 
  //this file at the same time
  if((log_file = fopen("problem2-peers.txt", "w")) == NULL) { 
    fprintf(stderr, "Failed to open logging file."
	    "All messages will redirect to stderr\n");
  };

  maxConn = config->max_conn;
  nonCongestQueue = newqueue();
  
  for(i = 0; i < peerInfo.numPeer; i++) {
    int peerID = peerInfo.peerList[i].peerID;
    uploadPool[peerID].dataQueue = newqueue();
    uploadPool[peerID].ackWaitQueue = newqueue();
    uploadPool[peerID].connID = 0;
    downloadPool[peerID].getQueue = newqueue();
    downloadPool[peerID].timeoutQueue = newqueue();
    downloadPool[peerID].ackSendQueue = newqueue();
    downloadPool[peerID].connected = 0;
    downloadPool[peerID].cache = NULL;
    initWindows(&(downloadPool[peerID].rw), &(uploadPool[peerID].sw));
  }
  printInit();
}

void printInit()
{
  printChunk(&masterChunk);
  printChunk(&hasChunk);
}
void printChunk(chunkList *list)
{
  int i;
  fprintf(stderr,"ListType: %d\n", list->type);
  for(i = 0; i < list->numChunk; i++) {
    char buf[50];
    bzero(buf, 50);
    chunkLine *line = &(list->list[i]);
    binary2hex(line->hash, 20, buf);
    fprintf(stderr,"%d: %s\n", line->seq, buf);
  }
}

void fillChunkList(chunkList *list, enum chunkType type, char *filename)
{
  FILE *fPtr = NULL;
  char lineBuf[MAX_LINE_SIZE];
  char *linePtr;
  int numChunk = 0;
  int chunkIdx = 0;
  bzero(list, sizeof(list));
  list->type = type;
  
  switch (type) {
  case MASTER:
    if((fPtr = fopen(filename, "r")) == NULL) {
      fprintf(stderr,"Open file %s failed\n", filename);
      exit(1);
    }
    fgets(lineBuf, MAX_LINE_SIZE, fPtr);
    if(strncmp(lineBuf, "File: ", 6) != 0) {
      fprintf(stderr,"Error parsing masterchunks\n");
      exit(1);
    } else {
      FILE *masterFile;
      char *newline = strrchr(lineBuf, '\n');
      if(newline) {
	*newline = '\0';
      }
      linePtr = &(lineBuf[6]);
      if((masterFile = fopen(linePtr, "r")) == NULL) {
	fprintf(stderr,"Error open master data file: <%s>\n", linePtr);
	exit(1);
      }
      list->filePtr = masterFile;
    }
    //Skip "Chunks:" line
    fgets(lineBuf, MAX_LINE_SIZE, fPtr);
  case GET:
    list->getChunkFile = calloc(strlen(filename) + 1, 1);
    strcpy(list->getChunkFile, filename);
  case HAS:
    if(fPtr == NULL) {
      if((fPtr = fopen(filename, "r")) == NULL) {
	fprintf(stderr, "Open file %s failed\n", filename);
	exit(-1);
      } else {
	fprintf(stderr,"Opened %s\n", filename);
      }
    }
    while(!feof(fPtr)) {
      char *hashBuf;
      if(fgets(lineBuf, MAX_LINE_SIZE, fPtr) == NULL) {
	break;
      }
      if(2 != sscanf(lineBuf, "%d %ms", &chunkIdx, &hashBuf)) {
	fprintf(stderr,"Error parsing hash\n");
	exit(1);
      }
      chunkLine *cPtr = &(list->list[numChunk]);
      cPtr->seq = chunkIdx;
      hex2binary(hashBuf, 2 * SHA1_HASH_SIZE, cPtr->hash);
      free(hashBuf);
      numChunk++;
    }
    list->numChunk = numChunk;
    break;
  default:
    fprintf(stderr,"WTF\n");
    exit(1);
  }
  fclose(fPtr);
}


void fillPeerList(bt_config_t *config)
{
  bt_peer_t *peer = config->peers;
  peerList_t *peerList = peerInfo.peerList;
  int numPeer = 0;
  while(peer != NULL) {
    if(peer->id == config->identity) {
      peerList[numPeer].isMe = 1;
    } else {
      peerList[numPeer].isMe = 0;

    }
    peerList[numPeer].peerID = peer->id;
    memcpy(&(peerList[numPeer].addr), &(peer->addr), sizeof(struct sockaddr_in));
    peer = peer->next;
    numPeer++;
  }
  peerInfo.numPeer = numPeer;
}

void handlePacket(Packet *pkt)
{
  if(verifyPacket(pkt)) {
    int type = getPacketType(pkt);
    switch(type) {
    case 0: { //WHOHAS
      fprintf(stderr,"->WHOHAS\n");
      Packet *pktIHAVE = newPacketIHAVE(pkt);
      enqueue(nonCongestQueue, (void *)pktIHAVE);
      break;
    }
    case 1: { //IHAVE
      fprintf(stderr,"->IHAVE\n");
      int peerIndex = searchPeer(&(pkt->src));
      int peerID = peerInfo.peerList[peerIndex].peerID;
      newPacketGET(pkt, downloadPool[peerID].getQueue);
      idle = 0;
      break;
    }
    case 2: { //GET
      fprintf(stderr,"->GET\n");
      if(numConnUp == maxConn){//Cannot allow more connections
	fprintf(stderr,"->GET request denied.\n");
	freePacket(pkt);
	break;
      }
      numConnUp++;
      int peerIndex = searchPeer(&(pkt->src));
      int peerID = peerInfo.peerList[peerIndex].peerID;
      /*
	If the sender has not received the final ack from the receiver yet but
	the receiver is already sending out a new GET, assert the receiver has received
	the last packet so abort the wait queue and re-initialize
      */
      if(downloadPool[peerID].connected == 0){
	clearQueue(uploadPool[peerID].ackWaitQueue);
	initWindows(&(downloadPool[peerID].rw), &(uploadPool[peerID].sw));
	newPacketDATA(pkt, uploadPool[peerID].dataQueue);
	//set start time
	uploadPool[peerID].connID++;
	gettimeofday(&(uploadPool[peerID].startTime), NULL);
      } else {    
	fprintf(stderr,"Only one-way connection is allowed.\n");
	freePacket(pkt);
      }
      break;
    }
    case 3: { //DATA
      fprintf(stderr,"->DATA");
      int peerIndex = searchPeer(&(pkt->src));
      int peerID = peerInfo.peerList[peerIndex].peerID;
      if(1 == updateGetSingleChunk(pkt, peerID)) {
	downloadPool[peerID].connected = 0;
	numConnDown--;
	updateGetChunk();
      }
      break;
    }
    case 4: { //ACK
      fprintf(stderr,"->ACK\n");
      int peerIndex = searchPeer(&(pkt->src));
      int peerID = peerInfo.peerList[peerIndex].peerID;
      updateACKQueue(pkt, peerID);
      break;
    }
    case 5://DENIED not used
    default:
      fprintf(stderr,"Type=WTF\n");
    }
  } else {
    fprintf(stderr,"Invalid packet\n");
  }
  freePacket(pkt);
  return;
}

void updateACKQueue(Packet *pkt, int peerID)
{
  sendWindow *sw = &(uploadPool[peerID].sw);
  uint32_t ack = getPacketAck(pkt);
  queue *ackWaitQueue = uploadPool[peerID].ackWaitQueue;
  queue *dataQueue = uploadPool[peerID].dataQueue;
  Packet *ackWait = peek(ackWaitQueue);
  struct timeval cur_time;
  gettimeofday(&cur_time, NULL);
  logger(peerID, uploadPool[peerID].connID, diffTimevalMilli(&cur_time, &(uploadPool[peerID].startTime)), sw->ctrl.windowSize);
  expandWindow(&(sw->ctrl));
  uploadPool[peerID].timeoutCount = 0;
  fprintf(stderr,"Received ACK %d. Last acked %d. Next in ackWaitQueue: %d\n", ack, sw->lastPacketAcked, ackWait == NULL ? 65535 : getPacketSeq(ackWait));
  if(ackWait != NULL) {
    if(ack >= getPacketSeq(ackWait)) {
      sw->dupCount = 0;
      while(ackWait != NULL && ack >= getPacketSeq(ackWait)) {
	dequeue(ackWaitQueue);
	freePacket(ackWait);
	ackWait = peek(ackWaitQueue);
      }
      sw->lastPacketAcked = ack;
      updateSendWindow(sw);
      //This is a hack but could be fine
      //Sender resets sending window if the whole chunk has been sent
      if(ack == BT_CHUNK_SIZE / PACKET_DATA_SIZE + 1) {
	fprintf(stderr,"Sender finished sending current chunk");
	numConnUp--;
	initWindows(&(downloadPool[peerID].rw), &(uploadPool[peerID].sw));
	assert(dequeue(ackWaitQueue) == NULL);
      }
    } else {//unexpected ACK ack number
      if(ack == sw->lastPacketAcked) { //dupliate ACK
	sw->dupCount++;
	fprintf(stderr,"Received duplicate packets %d\n", ack);
	if(sw->dupCount == MAX_DUPLICATE) { //trigger fast retransmit
	  fprintf(stderr,"Received 3 duplicates acks %d retransmitting\n", ack);
	  sw->dupCount = 0;
	  mergeAtFront(ackWaitQueue, dataQueue);
	  shrinkWindow(&(sw->ctrl));
	}
      }
    }
  }
}

int updateGetSingleChunk(Packet *pkt, int peerID)
{
  recvWindow *rw = &(downloadPool[peerID].rw);
  int dataSize = getPacketSize(pkt) - 16;
  uint8_t *dataPtr = pkt->payload + 16;
  uint32_t seq = (uint32_t)getPacketSeq(pkt);
  downloadPool[peerID].timeoutCount = 0;
  fprintf(stderr,"Got pkt %d expecting %d\n", seq, rw->nextPacketExpected);
  if(seq >= rw->nextPacketExpected) {
    if((seq > rw->nextPacketExpected) && ((seq - rw->nextPacketExpected) <= INIT_THRESH)) { //TODO: change this!
      insertInOrder(&(downloadPool[peerID].cache), newFreePacketACK(seq), seq);
      //ASSERSION: under all cases the queue should be empty when this happens
      newPacketACK(rw->nextPacketExpected - 1, downloadPool[peerID].ackSendQueue);
    } else if(seq - rw->nextPacketExpected <= INIT_THRESH) { //TODO: change this!
      newPacketACK(seq, downloadPool[peerID].ackSendQueue);
      rw->nextPacketExpected =
	flushCache(rw->nextPacketExpected, downloadPool[peerID].ackSendQueue, &(downloadPool[peerID].cache));
    }
    rw->lastPacketRead = seq;
    rw->lastPacketRcvd = seq;
    
    int curChunk = downloadPool[peerID].curChunkID;
    long offset = (seq - 1) * PACKET_DATA_SIZE + BT_CHUNK_SIZE * curChunk;
    FILE *of = getChunk.filePtr;
    fprintf(stderr,"DataIn %d [%ld-%ld]\n", seq, offset, offset + dataSize);
    if(of != NULL) {
      fseek(of, offset, SEEK_SET);
      fwrite(dataPtr, sizeof(uint8_t), dataSize, of);
    }
    
    /*Check if this GET finished */
    if(rw->nextPacketExpected > BT_CHUNK_SIZE / PACKET_DATA_SIZE + 1){
      clearQueue(downloadPool[peerID].timeoutQueue);
      fprintf(stderr,"Asserting chunk finished downloading: next expected %d thresh %d\n", rw->nextPacketExpected, BT_CHUNK_SIZE / PACKET_DATA_SIZE + 1);
      getChunk.list[curChunk].fetchState = 1;
      downloadPool[peerID].state = 0;
      clearQueue(downloadPool[peerID].timeoutQueue);
      initWindows(&(downloadPool[peerID].rw), &(uploadPool[peerID].sw));
      fprintf(stderr,"Chunk %d fetched\n", curChunk);
      fprintf(stderr,"%d More GETs in queue\n", downloadPool[peerID].getQueue->size);
      return 1;//this GET is done
    } else {
      return 0;
    }
  } else { //packet seq smaller than expected. Just send back ack.
    fprintf(stderr,"Received unexpected packet."
	    "Expecting %d received %d",
	    rw->nextPacketExpected, seq);
    newPacketACK(seq, downloadPool[peerID].ackSendQueue);
    return 0;
  }
}
/* Check if all GETs are done */
void updateGetChunk()
{
  fflush(log_file);
  int i = 0;
  int done = 1;
  for(i = 0; i < getChunk.numChunk; i++) {
    if(getChunk.list[i].fetchState != 1) {
      done = 0;
      fprintf(stderr,"Still missing chunk %d\n", i);
    }
  }
  if(done) {
    fclose(getChunk.filePtr);
    getChunk.filePtr = NULL;
    printf("GOT %s\n", getChunk.getChunkFile);
    free(getChunk.getChunkFile);
    bzero(&getChunk, sizeof(getChunk));
    idle = 1;
  }
}

int searchPeer(struct sockaddr_in *src)
{
  int i = 0;
  for(i = 0; i < peerInfo.numPeer; i++) {
    struct sockaddr_in *entry = &(peerInfo.peerList[i].addr);
    //Compare sin_port & sin_addr.s_addr
    //somehow packt redirectd from spiffy router does not have sin_addr so
    //in that case I compare only the port (this could be a glitch)
    int isSame = entry->sin_port == src->sin_port
      && ((entry->sin_addr.s_addr == src->sin_addr.s_addr) || (src->sin_addr.s_addr == 0));
    if(isSame) {
      return i;
    }
  }
  return -1;
}

void process_inbound_udp(int sock)
{
#define BUFLEN 1500
  struct sockaddr_in from;
  socklen_t fromlen;
  char buf[BUFLEN];

  fromlen = sizeof(from);
  spiffy_recvfrom(sock, buf, BUFLEN, 0, (struct sockaddr *) &from, &fromlen);

  Packet *newPkt = newPacketFromBuffer(buf);
  memcpy(&(newPkt->src), &from, fromlen);
  handlePacket(newPkt);

}


void process_get(char *chunkfile, char *outputfile)
{
  fprintf(stderr,"Handle GET (%s, %s)\n", chunkfile, outputfile);
  if((fopen(chunkfile, "r") == NULL)){
    fprintf(stderr, "Open file %s failed. (Mis-spelled?)\n", chunkfile);
    return;
  }
  fillChunkList(&getChunk, GET, chunkfile);
  if((getChunk.filePtr = fopen(outputfile, "w")) == NULL) {
    fprintf(stderr, "Open file %s failed.\n", outputfile);
    exit(-1);
  }
  printChunk(&getChunk);
  newPacketWHOHAS(nonCongestQueue);
}

void handle_user_input(char *line, void *cbdata)
{
  char chunkf[128], outf[128];
  cbdata = cbdata;
  bzero(chunkf, sizeof(chunkf));
  bzero(outf, sizeof(outf));
  
  if (sscanf(line, "GET %120s %120s", chunkf, outf)) {
    if (strlen(outf) > 0) {
      process_get(chunkf, outf);
    }
  }
}

void peer_run(bt_config_t *config)
{
  int sock;
  struct sockaddr_in myaddr;
  struct user_iobuf *userbuf;

  if ((userbuf = create_userbuf()) == NULL) {
    perror("peer_run could not allocate userbuf");
    exit(-1);
  }

  if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)) == -1) {
    perror("peer_run could not create socket");
    exit(-1);
  }

  bzero(&myaddr, sizeof(myaddr));
  myaddr.sin_family = AF_INET;
  myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  myaddr.sin_port = htons(config->myport);

  if (bind(sock, (struct sockaddr *) &myaddr, sizeof(myaddr)) == -1) {
    perror("peer_run could not bind socket");
    exit(-1);
  }

  spiffy_init(config->identity, (struct sockaddr *)&myaddr, sizeof(myaddr));
  struct timeval timeout;
  while (1) {
    int nfds;
    fd_set readfds;
    FD_ZERO(&readfds);
    if(idle == 1)
      FD_SET(STDIN_FILENO, &readfds);
    FD_SET(sock, &readfds);

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    nfds = select(sock + 1, &readfds, NULL, NULL, &timeout);
    
    if (nfds > 0) {
      if (FD_ISSET(sock, &readfds)) {
	process_inbound_udp(sock);
      }
      if (FD_ISSET(STDIN_FILENO, &readfds) && idle == 1) {
	process_user_input(STDIN_FILENO,
			   userbuf,
			   handle_user_input,
			   "Currently unused");
      }
    }
    flushQueue(sock, nonCongestQueue);
    flushUpload(sock);
    flushDownload(sock);
  }
}

void flushUpload(int sock)
{
  int i = 0;
  Packet *pkt;
  connUp *pool = uploadPool;
  for(i = 0; i < peerInfo.numPeer; i++) {
    int peerID = peerInfo.peerList[i].peerID;
    //Pacekt lies within the sending window
    Packet *ack = peek(pool[peerID].ackWaitQueue);
    if(ack != NULL) {
      struct timeval curTime;
      gettimeofday(&curTime, NULL);
      long dt = diffTimeval(&curTime, &(ack->timestamp));
      if(dt > DATA_TIMEOUT_SEC) {//detected timeout
	pool[peerID].timeoutCount++;
	fprintf(stderr,"data timeout. waiting for ack %d\n", getPacketSeq(ack));
	if(pool[peerID].timeoutCount == 3) {
	  fprintf(stderr,"Receiver ID %d timed out 3 times. Closing connection\n", peerID);
	  numConnUp--;
	  cleanUpConnUp(&(pool[peerID]));
	  continue;
	}
	fprintf(stderr,"Data timed out. Shrinking window.\n");
	shrinkWindow(&(pool[peerID].sw.ctrl));
	mergeAtFront(pool[peerID].ackWaitQueue, pool[peerID].dataQueue);
      }
    }
    pkt = peek(pool[peerID].dataQueue);
    while(pkt != NULL &&
	  (getPacketSeq(pkt) <= pool[peerID].sw.lastPacketAvailable)) {
      peerList_t *p = &(peerInfo.peerList[i]);
      int retVal = spiffy_sendto(sock,
				 pkt->payload,
				 getPacketSize(pkt),
				 0,
				 (struct sockaddr *) & (p->addr),
				 sizeof(p->addr));
      setPacketTime(pkt);
      fprintf(stderr,"Sent data %d. last available %d\n", getPacketSeq(pkt), pool[peerID].sw.lastPacketAvailable);
      if(retVal == -1) { //DATA lost
	fprintf(stderr,"spiffy_sendto() returned -1. Re-enqueuing data packet\n");
	enqueue(pool[peerID].dataQueue, dequeue(pool[peerID].dataQueue));
      } else {
	dequeue(pool[peerID].dataQueue);
	pool[peerID].sw.lastPacketSent = getPacketSeq(pkt);
	enqueue(pool[peerID].ackWaitQueue, pkt);
	pkt = peek(pool[peerID].dataQueue);
      }
    }
  }
}

void flushDownload(int sock)
{
  int i = 0;
  int idx;
  uint8_t *hash;
  Packet *pkt;
  connDown *pool = downloadPool;
  for(i = 0; i < peerInfo.numPeer; i++) {
    int peerID = peerInfo.peerList[i].peerID;
    /* Send ACK */
    Packet *ack = peek(pool[peerID].ackSendQueue);
    //Maybe should use 'if' instead of 'while' here
    /*ASSUMPTION:
      does not need to check window boundary here
      because it is checked on the DATA sending end
      and the rate at which ACKs get sent should always
      be slower than the DATA being sent
    */
    while(ack != NULL) {
      peerList_t *p = &(peerInfo.peerList[i]);
      fprintf(stderr,"Sending ack %d\n", getPacketAck(ack));
      int retVal = spiffy_sendto(sock,
				 ack->payload,
				 getPacketSize(ack),
				 0,
				 (struct sockaddr *) & (p->addr),
				 sizeof(p->addr));
      fprintf(stderr,"Sent ack %d\n", getPacketAck(ack));
      if(retVal == -1) {
	fprintf(stderr,"spiffy_sendto() returned -1. Re-enqueing ack packet.\n");
	enqueue(pool[peerID].ackSendQueue, dequeue(pool[peerID].ackSendQueue));
      } else {
	dequeue(pool[peerID].ackSendQueue);
	freePacket(ack);
	ack = dequeue(pool[peerID].ackSendQueue);
      }
    }
    /* Send GET */
    switch(pool[peerID].state) {
    case 0://Ready for next
      pkt = dequeue(pool[peerID].getQueue);
      while(pkt != NULL) {
	hash = getPacketHash(pkt, 0);
	printHash(hash);
	idx = searchHash(hash, &getChunk, 0);
	if(idx == -1) { //someone else is sending or has sent this chunk
	  freePacket(pkt);
	  pkt = dequeue(pool[peerID].getQueue);
	} else if(numConnDown < maxConn){
	  getChunk.list[idx].fetchState = 2;
	  if(downloadPool[peerID].connected == 1)
	    fprintf(stderr,"NOT SUPPOSED TO BE CONNECTEED! \n\n\n\n\n\n");
	  downloadPool[peerID].connected = 1;
	  numConnDown++;
	  break;
	} else {//Cannot allow more download connections
	  fprintf(stderr,"->No more download connection allowed\n");
	  pool[peerID].state = 2;
	  break;
	}
      }
      if(pool[peerID].state == 2)
	break;
      
      if(pkt != NULL) {
	fprintf(stderr,"Sending a GET\n");
	peerList_t *p = &(peerInfo.peerList[i]);
	hash = pkt->payload + 16;
	char buf[50];
	bzero(buf, 50);
	binary2hex(hash, 20, buf);
	fprintf(stderr,"GET hash:%s\n", buf);
	pool[peerID].curChunkID = searchHash(hash, &getChunk, -1);
	//Send get
	int retVal = spiffy_sendto(sock,
				   pkt->payload,
				   getPacketSize(pkt),
				   0,
				   (struct sockaddr *) & (p->addr),
				   sizeof(p->addr));

	if(retVal == -1) {
	  fprintf(stderr,"spiffy_snetto() returned -1. Re-flushing the network with WHOHAS.\n");
	  newPacketWHOHAS(nonCongestQueue);
	  freePacket(pkt);
	  cleanUpConnDown(&(pool[peerID]));
	  numConnDown--;
	  return;
	}
	//Mark time
	setPacketTime(pkt);
	//Put it in timeoutQueue
	enqueue(pool[peerID].timeoutQueue, pkt);
	pool[peerID].state = 1;
      }
      break;
    case 1: {//Downloading
      pkt = peek(pool[peerID].timeoutQueue);
      struct timeval curTime;
      gettimeofday(&curTime, NULL);
      long dt = diffTimeval(&curTime, &(pkt->timestamp));
      if(dt > GET_TIMEOUT_SEC) {
	pool[peerID].timeoutCount++;
	fprintf(stderr,"Get requset timed out %d times\n", pool[peerID].timeoutCount);
	setPacketTime(pkt);
	if(pool[peerID].timeoutCount == 3) {
	  getChunk.list[pool[peerID].curChunkID].fetchState = 0;
	  pool[peerID].state = 0;
	  newPacketWHOHAS(nonCongestQueue);
	  freePacket(pkt);
	  cleanUpConnDown(&(pool[peerID]));
	  numConnDown--;
	}
      }
      break;
    }
    case 2: {
      break;
    }
    default:
      break;
    }

  }
}

void flushQueue(int sock, queue *sendQueue)
{
  int i = 0;
  int retVal = -1;
  int count = sendQueue->size;
  int noLoss = 1;
  Packet *pkt = dequeue(sendQueue);
  if(pkt == NULL) {
    return;
  }
  peerList_t *list = peerInfo.peerList;
  while(count > 0) {
    if(pkt->dest != NULL) { //IHAVE packets have specific destinations
      retVal = spiffy_sendto(sock,
			     pkt->payload,
			     getPacketSize(pkt),
			     0,
			     (struct sockaddr *) (pkt->dest),
			     sizeof(*(pkt->dest)));
    } else {//WHOHAS packets 
      for(i = 0; i < peerInfo.numPeer; i++) {
	if(list[i].isMe == 0) {
	  retVal = spiffy_sendto(sock,
				 pkt->payload,
				 getPacketSize(pkt),
				 0,
				 (struct sockaddr *) & (list[i].addr),
				 sizeof(list[i].addr));
	  if(retVal == -1) {
	    enqueue(sendQueue, pkt);
	    noLoss = 1;
	  }
	}
      }
    }
    if(noLoss == 1) {
      fprintf(stderr,"There has been nonCongest packet loss (spiffy_sendto() returned -1.)"
	     "New attempts will be made later");
    } 
    freePacket(pkt);
    pkt = dequeue(sendQueue);
    count--;
  }
}



long diffTimeval(struct timeval *t1, struct timeval *t2)
{
  return t1->tv_sec - t2->tv_sec;
}

int diffTimevalMilli(struct timeval *t1, struct timeval *t2)
{
  return (t1->tv_sec - t2->tv_sec) * 1000 + (t1->tv_usec - t2->tv_usec) / 1000.0;
}

int checkTimer(struct timeval *tv, time_t sec){
  struct timeval curTime;
  gettimeofday(&curTime, NULL);
  if(diffTimeval(&curTime, tv) >= sec){
    gettimeofday(tv, NULL); 
    return 1;
  }
  return 0;
}
