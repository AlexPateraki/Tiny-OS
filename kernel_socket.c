#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_dev.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

socket_cb *PORT_MAP[MAX_PORT+1] = {NULL}; // make the port map for SCBs

int socket_read(void* this, char *buf, unsigned int size){
	
	socket_cb *scb = (socket_cb *)this;

	socket_cb *peer_scb = scb->peer_s.peer; // create the peer socket

	if(scb->type != SOCKET_PEER){
		return -1;
	}

	//  if there is a read pipe
	if (scb->peer_s.read_pipe == NULL){ 
		return -1;
	}

	//  if the writer is closed and there is no more data in pipe
	if (peer_scb->peer_s.write_pipe == NULL && scb->peer_s.read_pipe->numOfElem == 0){ 
		return 0; 
	}

	// read from pipe
	return pipe_read(scb->peer_s.read_pipe, buf, size);  
}

int socket_write(void* this, const char *buf, unsigned int size){
	
	socket_cb *scb = (socket_cb *)this;

	socket_cb *peer_scb = scb->peer_s.peer; // create the peer socket

	if(scb->type != SOCKET_PEER){
		return -1;
	}

	//  if there is write pipe
	if (scb->peer_s.write_pipe == NULL){ 
		return -1;
	}

	//  if the reader is closed
	if (peer_scb->peer_s.read_pipe == NULL){ 
		return -1;
	}

 	return pipe_write(scb->peer_s.write_pipe, buf, size);
}


int socket_close(void* this){
	socket_cb *scb = (socket_cb *)this; //create SCB

	if(scb != NULL){ 

		if(scb->type == SOCKET_PEER){
			//close reader and writer
			pipe_reader_close(scb->peer_s.read_pipe);
			pipe_writer_close(scb->peer_s.write_pipe);

		}else if(scb->type == SOCKET_LISTENER){
			// release its port map position
			PORT_MAP[scb->port] = NULL;
			
			// wake up every thread which is waiting before lisnener close
			CondVar *request_avail = &(scb->listener_s.req_available);
			
			// signal all the waiting threads
			kernel_broadcast(request_avail);
		}

		// free the socket
		free(scb);
		
		return 0;

	}else{
		return -1;
	}
}


file_ops socket_file_ops = {
  .Open = null_open,
  .Read = socket_read,
  .Write = socket_write,
  .Close = socket_close
};


Fid_t sys_Socket(port_t port)
{
	// port moves between 0 and MAX_PORT
	if(port < NOPORT || port > MAX_PORT){ 
		return NOFILE;
	}

	Fid_t fid[1];
	FCB *fcb[1];

	// reserve an fcb of the file table with an fid of process table
	if( FCB_reserve(1, fid, fcb) == 0 )  {
		return NOFILE; 
	}
	
	// initialize scb
	socket_cb *scb = (socket_cb*)xmalloc(sizeof(socket_cb));
	scb->refcount = 1;
	scb->fcb = fcb[0];
	scb->type = SOCKET_UNBOUND;
	scb->port = port;
	rlnode_init(&scb->unbound_s.unbound_socket, scb);
	
	// connect fcb with socket blocks
	fcb[0]->streamobj = scb; 
	fcb[0]->streamfunc = &socket_file_ops;


	return fid[0];
}

int sys_Listen(Fid_t sock)
{

	if (sock < 0 || sock >= MAX_FILEID) //  if fid is legal
	{ 
		return -1;
	}

	if(CURPROC->FIDT[sock] == NULL){ //  if given fid is NULL
		return -1;
	}

	socket_cb *scb = (socket_cb *) CURPROC->FIDT[sock]->streamobj;

	if(scb == NULL || scb->port == NOPORT){ //  if the socket is not bound to a port
		return -1;
	}

	if(PORT_MAP[scb->port] != NULL){ //  if the port of the listener is bounded by another listener
		return -1;
	}

	if(scb->port > MAX_PORT){
		return -1;
	}

	if(scb->type != SOCKET_UNBOUND){ //  if the socket has already been initialized
		return -1;
	}

	// initialize listener of the sock
	scb->type = SOCKET_LISTENER;
	PORT_MAP[scb->port] = scb;
	rlnode_init(&scb->listener_s.queue, NULL);
	scb->listener_s.req_available = COND_INIT;

	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
	if (lsock < 0 || lsock >= MAX_FILEID) //  if fid is legal
	{ 
		return NOFILE;
	}

	if(CURPROC->FIDT[lsock] == NULL){ //  if given fid is NULL
		return NOFILE;
	}

	socket_cb *scb = (socket_cb *) CURPROC->FIDT[lsock]->streamobj;

	if(scb == NULL || scb->port == NOPORT){ //  if the socket is not bound to a port
		return NOFILE;
	}

	if(scb->type != SOCKET_LISTENER){ //  if the socket is a listener
		return NOFILE;
	}


	scb->refcount++;// increase the SCB's refcount by one

	port_t lport= scb->port;// create a port for the listener socket

	//fprintf(stderr, "\n Before wait");

	// wait while request list is empty and Listener is not closed
	while(is_rlist_empty(&scb->listener_s.queue) && (PORT_MAP[lport] != NULL)){
		kernel_wait(&scb->listener_s.req_available, SCHED_IO);
	}		

	//fprintf(stderr, "\n After wait");

	if(PORT_MAP[lport] == NULL){ //  port still valid after the kernel_wait()
		//scb->refcount--;
		return NOFILE;
	}

	// next request
	rlnode *reqNode = rlist_pop_front(&scb->listener_s.queue);
	connection_request *request = reqNode->obj;
	request->admitted = 1; // now it is admitted

	socket_cb *client = request->peer; // the socket of the client

	// create a socket to serve the client
	Fid_t server_fid = sys_Socket(scb->port);
	
	if (server_fid == NOFILE) //  if sys_Socket failed
	{
		return NOFILE;
	}
	//fprintf(stderr, "\n After NOfile");

	socket_cb *server = (socket_cb *) CURPROC->FIDT[server_fid]->streamobj; // the socket of the server


	// connect the peers
	server->type = SOCKET_PEER;
	client->type = SOCKET_PEER;
	server->peer_s.peer = client;
	client->peer_s.peer = server; 

	// create two pipes
	pipe_cb *pipe1 = xmalloc(sizeof(pipe_cb));
	pipe_cb *pipe2 = xmalloc(sizeof(pipe_cb));
	
	// connect the fcbs with each pipe
	pipe1->reader = client->fcb;
	pipe1->writer = server->fcb;
	pipe2->reader = server->fcb;
	pipe2->writer = client->fcb;

	// initialize the remainnig fields of the pipes
	pipe1->has_space = pipe1->has_data = COND_INIT;
	pipe2->has_space = pipe2->has_data = COND_INIT;
	pipe1->w_position = pipe1->r_position = pipe1->numOfElem = 0;
	pipe2->w_position = pipe2->r_position = pipe2->numOfElem = 0;

	// connect the sockets with the pipes
	client->peer_s.read_pipe = pipe1;
	server->peer_s.write_pipe = pipe1;
	client->peer_s.write_pipe = pipe2;
	server->peer_s.read_pipe = pipe2;

	// signal the connection request
	kernel_signal(&request->connected_cv);

	scb->refcount--; // substract the refcount of SCB by one

	return server_fid; // return the fid of the socket server
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	if (sock < 0 || sock >= MAX_FILEID) //  if file id is legal
	{ 
		return -1;
	}

	if(CURPROC->FIDT[sock] == NULL){ //  if given fid is NULL
		return -1;
	}

	if(port <= NOPORT || port > MAX_PORT){ // port moves between 0 and MAX_PORT
		return -1;
	}

	socket_cb *scb = (socket_cb *) CURPROC->FIDT[sock]->streamobj; 

	if(PORT_MAP[port] == NULL){ 
		return -1;
	}

	if (scb == NULL || scb->type != SOCKET_UNBOUND || PORT_MAP[port]->type != SOCKET_LISTENER) //  if the socket is not unbounded return -1
	{
		return -1;
	}

	scb->refcount++;

	scb->port = port; // update the socket port from the argument

	// build the request 
	connection_request *request = xmalloc(sizeof(connection_request));
	//initialize every field
	request->admitted = 0;
	request->peer = scb;
	request->connected_cv = COND_INIT;

	// add to the request queue
	rlnode_init(&request->queue_node, request);

	// add the request to the tail of the listener's queue
	rlist_push_back(&PORT_MAP[port]->listener_s.queue, &request->queue_node);

	// signal that this request is waiting ready 
	kernel_signal(&PORT_MAP[port]->listener_s.req_available);

	// goes to sleep until admitted==1
	while(request->admitted == 0){
		if(kernel_timedwait(&request->connected_cv, SCHED_PIPE, 1000*timeout) == 0){ //  return -1, when timeout has ended without a successful connection.
			
			return -1;
		}
	}

	scb->refcount--; // substract the refcount of the SCB by one

	return 0;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	if (sock < 0 || sock >= MAX_FILEID) //  if fid is legal
	{ 
		return -1;
	}

	if(CURPROC->FIDT[sock] == NULL){ //  if given fid is NULL
		return -1;
	}

	socket_cb *scb = (socket_cb *) CURPROC->FIDT[sock]->streamobj;

	if (scb->type != SOCKET_PEER) //  if it is a peer socket
	{
		return -1;
	}

	switch(how){
		case SHUTDOWN_READ:
			//close the reader of the pipe of the socket
			pipe_reader_close(scb->peer_s.read_pipe);
			scb->peer_s.read_pipe = NULL;
			break;

		case SHUTDOWN_WRITE:
			//close the writer of the pipe of the socket
			pipe_writer_close(scb->peer_s.write_pipe);
			scb->peer_s.write_pipe = NULL;
			break;
		
		case SHUTDOWN_BOTH:
			//close both the reader and the writer of the pipe of the socket			
			pipe_reader_close(scb->peer_s.read_pipe);
			pipe_writer_close(scb->peer_s.write_pipe);
			scb->peer_s.read_pipe = NULL;
			scb->peer_s.write_pipe = NULL;
			break;
	}

	return 0;
}

