#include "tinyos.h"
#include "util.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_sched.h"


int err(void* obj, const char* buf, unsigned int size){
	return -1;
}


static file_ops readops = {
		//.Open = NULL,
		.Read = pipe_read,
		.Write = (void*)err,
		.Close =  pipe_reader_close
};

static file_ops writeops = {
		//.Open = NULL,
		.Read = (void*)err,
		.Write = pipe_write,
		.Close =  pipe_writer_close
};

int write_flag = 0;
int read_flag = 0;
int full = 0;

int pipe_read(void* pipei,char *buf,unsigned int size)
{
	PipeCB* pipe = (PipeCB*)pipei;
	if(pipe->reader == NULL) return -1;
	read_flag = 1;
	preempt_off;            /* Stop preemption */

	if( (pipe->numofel == 0) & (pipe->writer != NULL)){  /* Read sleeps when write is open and there are no elements to read*/
		kernel_broadcast(& pipe->hasspace);
		kernel_wait(& pipe->hasdata,SCHED_PIPE);
	}

	uint count = 0;
	for(int num = pipe->read; count<size; num++)
	{
		if(num == pipe->write) break;		/*If the write position is reached then no more elements can be read*/
		
		buf[count] = pipe->buffer[num];
		count++;
		pipe->read++;
		pipe->numofel--;
		if (!pipe->numofel) { /*If all data is read then end function*/
			if (write_flag) kernel_broadcast(& pipe->hasspace); /*If Write is waiting for Read then signal CV hasspace*/
		}else break;
	}
	if( (count<size) & (pipe->writer != NULL) ){           /*If Read didnt finish then Write must be woken up*/
		kernel_broadcast(& pipe->hasspace);
		kernel_wait(& pipe->hasdata,SCHED_PIPE);/*If read didnt finish wake up write*/
	}
	if (full) {
		pipe->write = 0;
		pipe->read = 0;
	}
	read_flag = 0;
	preempt_on;
	return count;
}



int pipe_write(void* pipei,const char *buf,unsigned int size){

	PipeCB* pipe = (PipeCB*)pipei;
	if( (pipe->writer == NULL) || (pipe->reader == NULL) ) return -1;

	int count =0;
	for(int num=pipe->write; count<size; num++)
	{
		if (num == BUFSIZE-1)
		{
			full = 1;
			if ( (pipe->reader != NULL) && (num<size) )
			{
				write_flag = 1;
				kernel_broadcast(&pipe->hasdata);
				kernel_wait(&pipe->hasspace,SCHED_PIPE);
			}
			else
			{
				fprintf(stderr,"\nPipe is Full");
				return count;
			}
		}
		pipe->buffer[num] = buf[count];
		pipe->write++;
		count ++;
		pipe->numofel++;
	}
	write_flag = 0;
	if (read_flag) kernel_broadcast(& pipe->hasdata);

	return count;

}


int sys_Pipe(pipe_t* pipe)
{
	/* Memory Allocation */
	PCB* curp = CURPROC;
	PipeCB* pipec = (PipeCB*)xmalloc(sizeof(pipe));
	if (pipec == NULL)
	{
		printf("ERROR\n");
		return -1;
	}

	pipec->hasspace = COND_INIT;
	pipec->hasdata = COND_INIT;
	buf_init(pipec->buffer);
	pipec->read= 0;
	pipec->write = 0;
	pipec->numofel = 0;

	Fid_t pipefds[2];
	FCB* rfcb;
	if (! FCB_reserve(1,&pipefds[0],&rfcb)) return -1;
	rfcb->streamfunc = &readops;
	//rfcb->streamobj = &pipec->buffer;
	rfcb->streamobj = pipec;
	pipec->read = pipe->read;

	FCB* wfcb;
	if (! FCB_reserve(1,&pipefds[1],&wfcb)) return -1;
	wfcb->streamfunc = &writeops;
	//wfcb->streamobj = &pipec->buffer;
	wfcb->streamobj = pipec;
	pipec->write= pipe->write;

	pipec->reader = rfcb;
	pipec->writer = wfcb;

	curp->pipe = pipec;

	return 0;
}


int pipe_reader_close(void* pipei){
	PipeCB* pipe = (PipeCB*)pipei;
	pipe->reader = NULL;
	kernel_broadcast(& pipe->hasspace);
	if (pipe->writer == NULL) free(pipe);
	return 0;
}

int pipe_writer_close(void* pipei){
	PipeCB* pipe = (PipeCB*)pipei;
	pipe->writer = NULL;
	kernel_broadcast(& pipe->hasdata);
	if (pipe->reader == NULL) free(pipe);
	return 0;
}

void buf_init(char buffer[BUFSIZE]){
	for(int i=0;i<BUFSIZE;i++){
		buffer[i] = 0;
	}
	return;
}