#include "tinyos.h"
#include "util.h"
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
/*
SCB* PORT_TABLE[MAX_PORT];

void port_tb_init()
{
    for(int i=0; i<MAX_PORT; i++)
    {
        PORT_TABLE[i] = NULL;
    }
}

file_ops socket_fops = {
        .Open = NULL,
        .Read = socket_read,
        .Write = socket_write,
        .Close = socket_close
};*/

Fid_t sys_Socket(port_t port)
{
/*
    kernel_lock();

	FCB* fcb;
	Fid_t fid;
	PCB* pcb = CURPROC;


	if((port<0)||(port>MAX_PORT)||(port==NOPORT)||(!FCB_reserve(1, &fid, &fcb)))
	{
        kernel_unlock();
        return NOFILE;
	}
	else
	{
		SCB* socket = (SCB*) xmalloc(sizeof(SCB));

		socket->sid = fid;
		socket->port = port;
		socket->state = UNBOUND;
		//socket->fcb = fcb;
		//socket->pcb = CURPROC;
        socket->pipe_to_write = NULL;
        socket->pipe_to_read = NULL;
        //socket->peer = NULL;

		fcb->streamobj = socket;
		fcb->streamfunc = &socket_fops;
	}

    kernel_unlock();
	return fid;*/
	return -1;
}

int sys_Listen(Fid_t sock)
{
/*
    SCB* current_socket;

	if(!((sock==NOFILE)||(sock<0)||(sock>MAX_FILEID)||(CURPROC->FIDT[sock]==NULL)||CURPROC->FIDT[sock]->streamfunc!=&socket_fops))
    {
        current_socket = CURPROC->FIDT[sock]->streamobj;

        if((current_socket->port<0)||(current_socket->port>MAX_PORT)||(current_socket->port==NOPORT)||(current_socket->state==UNBOUND)||(PORT_TABLE[current_socket->port]==NULL))
        {
            current_socket->state = LISTENER;
            PORT_TABLE[current_socket->port] = current_socket;

            kernel_unlock();
            return 0;
        }
    }*/

    return -1;
}



Fid_t sys_Accept(Fid_t lsock)
{
	return NOFILE;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	return -1;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	return -1;
}

