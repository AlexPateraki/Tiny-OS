
#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_dev.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

void *null_open(uint minor){
	return NULL; 
}
int null_write(void* this, const char* buf, unsigned int size){
	return -1;
}
int null_read(void* this, char *buf, unsigned int size){
	return -1;
}


file_ops reader_file_ops = {
  .Open = null_open,
  .Read = pipe_read,
  .Write = null_write,
  .Close = pipe_reader_close
};

file_ops writer_file_ops = {
  .Open = null_open,
  .Read = null_read,
  .Write = pipe_write,
  .Close = pipe_writer_close
};

int pipe_read(void* this, char *buf, unsigned int size){
pipe_cb *pp = (pipe_cb *)this;//create a pipe

	//if pipe reader exists and if there is not space in the buffer 
	while(pp->reader != NULL && pp->numOfElem == 0){
		if(pp->writer != NULL){
			kernel_signal(&pp->has_space); // signal the writer
			kernel_wait(&pp->has_data, SCHED_PIPE); // make reader sleep
		}
		else{
			return 0;
		}
	}

	// checking if pipe reader exists after waiting 
	if (pp->reader == NULL){
		return -1;
	}

	int Bytes_Read = 0;

	int elemInBuffer;
	//reading until the given size
 		if (pp->numOfElem < size){
 			elemInBuffer = pp->numOfElem;
 		}
 		else{
			elemInBuffer = size;
		}

	// read the elements from pipe
	for(int i=0; i<elemInBuffer; i++){

		buf[i] = pp->BUFFER[pp->r_position];
		pp->r_position = (pp->r_position+1)%PIPE_BUFFER_SIZE; // matching the read position of the buffer
		pp->numOfElem--;
		Bytes_Read++;
	}

	// signal the writer after completing reading
	kernel_signal(&pp->has_space);

	return Bytes_Read;
}

int pipe_write(void* this, const char* buf, unsigned int size){

	pipe_cb *pp = (pipe_cb *)this;


	//number free positions of the buffer 
	int free_pos_buffer = PIPE_BUFFER_SIZE - pp->numOfElem;

	//if pipe reader exists and if there is not space in the buffer 
	while(pp->writer != NULL && pp->reader != NULL && free_pos_buffer == 0){

		kernel_signal(&pp->has_data); // signal the reader
		kernel_wait(&pp->has_space, SCHED_PIPE); // writer goes to sleep 
		free_pos_buffer = PIPE_BUFFER_SIZE-pp->numOfElem; // update the free positions of buffer after waiting
	}

	// check if pipe reader and writer are still effective after waiting 
	if (pp->reader == NULL || pp->writer == NULL){
		return -1;
	}

	//the size of the written bytes
	int Bytes_Written = 0;
	// checking not to write more than the given size
	int bToWrite;
	if (free_pos_buffer < size){
 			 bToWrite = free_pos_buffer;
 		}
 		else{
			bToWrite = size;
		}

	// write the elements to the pipe
	for(int j=0; j<bToWrite; j++){

		pp->BUFFER[pp->w_position] = buf[j];
		pp->w_position = (pp->w_position+1)%PIPE_BUFFER_SIZE; // fixing the write position of circular buffer
		pp->numOfElem++;
		Bytes_Written++;		
	}

	// signal the reader after completing the writing
	kernel_signal(&pp->has_data);
	
	return Bytes_Written;
}

int pipe_reader_close(void* this){
	pipe_cb *pp = (pipe_cb *)this; 

	if(pp != NULL){ 
		pp->reader = NULL; // close the end of reader

		if(pp->writer == NULL){ // case: end of writer is NULL 
			free(pp); // free the pipe
		}else{
			kernel_broadcast(&pp->has_space); //signal the writer
		}

	}else{
		return -1; 
	}

	return 0;
}

int pipe_writer_close(void* this){
	pipe_cb *pp = (pipe_cb *)this; 

	if(pp != NULL){ 
		pp->writer = NULL;// close the end of writer

		if(pp->reader == NULL){ // case: the end of reader is NULL 
			free(pp); // free the  pipe
		}else{
			kernel_broadcast(&pp->has_data); //signal the reader
		}

	}else{
		return -1;
	}

	return 0;
}
int sys_Pipe(pipe_t* pipe)
{
	FCB *fcb[2];
	Fid_t fid[2];
	//use FCB_reserve function to reserve a fcb
	//connect fid to the corresponding fcb
	//return -1 if FIDT is not connected to FCB
	if(FCB_reserve(2, fid, fcb) == 0) {
		return -1;
	}
	
	*pipe = (pipe_t){
	  .read = fid[0],
	  .write = fid[1]
	};

	pipe_cb *pp = xmalloc(sizeof(pipe_cb));

	//attach fcbs with pipe_cbs
	pp->reader = fcb[0];
	pp->writer = fcb[1];

	// initialize the condition variables
	pp->has_space = pp->has_data = COND_INIT;
	pp->w_position = pp->r_position = pp->numOfElem = 0;

	// connect fcbs wiwth the pipe_cb
	fcb[0]->streamobj = pp;
	fcb[1]->streamobj = pp;

	// connect the fcbs to file_ops 
	fcb[0]->streamfunc = &reader_file_ops;
	fcb[1]->streamfunc = &writer_file_ops;
	
	return 0;

}

