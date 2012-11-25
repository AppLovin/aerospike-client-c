/*
 * A good, basic C client for the Aerospike protocol
 * Creates a library which is linkable into a variety of systems
 *
 * First attempt is a very simple non-threaded blocking interface
 * currently coded to C99 - in our tree, GCC 4.2 and 4.3 are used
 *
 * Brian Bulkowski, 2009
 * All rights reserved
 */

#include <sys/types.h>
#include <sys/socket.h> // socket calls
#include <stdio.h>
#include <errno.h> //errno
#include <stdlib.h> //fprintf
#include <unistd.h> // close
#include <string.h>
#include <fcntl.h>

#include "citrusleaf_event/evcitrusleaf.h"
#include "citrusleaf_event/evcitrusleaf-internal.h"
#include "citrusleaf_event/cl_cluster.h"
#include "citrusleaf/proto.h"
#include "citrusleaf/cf_clock.h"

//
// Globals to track transaction counts to clean up properly
//

int g_cl_info_transactions = 0;



// debug
extern  void sockaddr_in_dump(int level,char *prefix, struct sockaddr_in *sa_in);

#ifdef CLDEBUG_VERBOSE
// this is broken with new logging system. need to dump into a temp buf then log that out.
static void
dump_buf(char *info, uint8_t *buf, size_t buf_len)
{
	CL_LOG(CL_VERBOSE, "dump_buf: %s\n",info);
	uint i;
	for (i=0;i<buf_len;i++) {
		if (i % 16 == 8)
			CL_LOG( CL_VERBOSE," :");
		if (i && (i % 16 == 0))
			CL_LOG(CL_VERBOSE, "\n");
		CL_LOG( "%02x ",buf[i]);
	}
	CL_LOG(CL_VERBOSE, "\n");
}
#endif

void
info_request_destroy(cl_info_request *cir)
{
	
	if (cir->rd_buf)	free(cir->rd_buf);
	if (cir->wr_buf) {
		if (cir->wr_buf != cir->wr_tmp)
			free(cir->wr_buf);
	}
	free(cir);
}




/*
** when you expect a single result back, info result into just that string
*/

int
citrusleaf_info_parse_single(char *values, char **value)
{
	while (*values && (*values != '\t'))
		values++;
	if (*values == 0)	return(-1);
	values++;
	*value = values;
	while (*values && (*values != '\n'))
		values++;
	if (*values == 0)	return(-1);
	*values = 0;
	return(0);
	
}

int
info_make_request(cl_info_request *cir, char *names)
{
	cir->wr_buf_size = sizeof(cl_proto);
	if (names) {
		uint32_t nameslen = strlen(names);
		cir->wr_buf_size += nameslen;
		if (names[nameslen-1] != '\n')
			cir->wr_buf_size++;
	}

	// set up the buffer pointer
	if (cir->wr_buf_size > sizeof(cir->wr_tmp)) {
		cir->wr_buf = malloc( cir->wr_buf_size );
		if (!cir->wr_buf)	return(-1);
	} else {
		cir->wr_buf = cir->wr_tmp;
	}

	// do byte-by-byte so we can convert :-(	
	if (names) {
		char *src = names;
		char *dst = (char *) (cir->wr_buf + sizeof(cl_proto));
		while (*src) {
			if ((*src == ';') || (*src == ':') || (*src == ',')) 
				*dst = '\n';
			else 
				*dst = *src;
			src++;
			dst++;
		}
		if ( src[-1] != '\n')	*dst = '\n';
	}

	cl_proto *proto = (cl_proto *) cir->wr_buf;
	proto->sz = cir->wr_buf_size - sizeof(cl_proto); 
	proto->version = CL_PROTO_VERSION;
	proto->type = CL_PROTO_TYPE_INFO;
	cl_proto_swap(proto);
	return(0);
}


void
info_event_fn(int fd, short event, void *udata)
{
	cl_info_request *cir = (cl_info_request *)udata;
	int rv;
	
	g_cl_stats.info_events++;
	
	uint64_t _s = cf_getms();
	

	CL_LOG(CL_VERBOSE, "info_event: fd %d event %x cir %p\n",fd,(int)event,cir);
	CL_LOG(CL_VERBOSE, "  -- wrbufpos %zu wrbufsize %zu\n",cir->wr_buf_pos,cir->wr_buf_size);
	CL_LOG(CL_VERBOSE, "  -- rdbufpos %zu rdbufsize %zu rdheaderpos %zu\n",cir->rd_buf_pos,cir->rd_buf_size,cir->rd_header_pos);
	
	if (event & EV_WRITE) {
		if (cir->wr_buf_pos < cir->wr_buf_size) {
			rv = send(fd, &cir->wr_buf[cir->wr_buf_pos], cir->wr_buf_size - cir->wr_buf_pos, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (rv > 0) {
				cir->wr_buf_pos += rv;
				if (cir->wr_buf_pos == cir->wr_buf_size) {
					// changing from WRITE to READ requires redoing the set then the add 
					event_set(&cir->network_event, fd, EV_READ, info_event_fn, cir);		
				}
			}
			else if (rv == 0) {
				CL_LOG(CL_DEBUG, "write info failed: illegal send return 0: errno %d\n",errno);				
				goto Fail;
			}
			else if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
				CL_LOG(CL_DEBUG, "write info failed: rv %d errno %d\n",rv, errno);
				goto Fail;
			}
		}
	}

	if (event & EV_READ) {
		if (cir->rd_header_pos < sizeof(cl_proto) ) {
			rv = recv(fd, &cir->rd_header_buf[cir->rd_header_pos], sizeof(cl_proto) - cir->rd_header_pos, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (rv > 0) {
				cir->rd_header_pos += rv;
			}				
			else if (rv == 0) {
				CL_LOG(CL_INFO, "read info failed: remote close: rv %d errno %d\n",rv,errno);
				goto Fail;
			}
			else if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
				CL_LOG(CL_INFO, "read info failed: unknown error: rv %d errno %d\n",rv,errno);
				goto Fail;
			}
		}
		if (cir->rd_header_pos == sizeof(cl_proto)) {

			CL_LOG( CL_VERBOSE, "read: read first part, now read last part rd size %zd\n",cir->rd_buf_size);
//			dump_buf("event:read:proto",cir->rd_header_buf,sizeof(cl_proto));
			
			if (cir->rd_buf_size == 0) {
				// calculate msg size
				cl_proto *proto = (cl_proto *) cir->rd_header_buf;
				cl_proto_swap(proto);
				
				// set up the read buffer
				cir->rd_buf = malloc(proto->sz + 1);
				if (!cir->rd_buf) {
					CL_LOG(CL_WARNING, "cl info malloc fail\n");
					goto Fail;
				}
				cir->rd_buf[proto->sz] = 0;
				cir->rd_buf_pos = 0;
				cir->rd_buf_size = proto->sz;
			}
			if (cir->rd_buf_pos < cir->rd_buf_size) {
				rv = recv(fd, &cir->rd_buf[cir->rd_buf_pos], cir->rd_buf_size - cir->rd_buf_pos, MSG_NOSIGNAL | MSG_DONTWAIT);
				if (rv > 0) {
					cir->rd_buf_pos += rv;
					if (cir->rd_buf_pos >= cir->rd_buf_size) {
						
						CL_LOG(CL_VERBOSE, "info completed! fd %d cir %p\n", fd, cir);
						
						// caller frees rdbuf
						(*cir->user_cb) ( 0 /*return value*/, (void *)cir->rd_buf , cir->rd_buf_size ,cir->user_data );
						cir->rd_buf = 0;
						event_del(&cir->network_event); // WARNING: this is not necessary. BOK says it is safe: maybe he's right, maybe wrong.

						close(fd);
						info_request_destroy(cir);
						cir = 0;
						g_cl_stats.info_complete++;
						g_cl_info_transactions--;
						
						uint64_t delta = cf_getms() - _s;
						if (delta > CL_LOG_DELAY_WARN) CL_LOG(CL_WARNING," CL_DELAY cl_info event OK fn: %"PRIu64"\n",delta);

						return;
					}
				}
				else if (rv == 0) {
					CL_LOG(CL_INFO, "info failed: remote termination fd %d cir %p rv %d errno %d\n", fd, cir, rv, errno);
					goto Fail;
				}
				else if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
					CL_LOG(CL_INFO, "info failed: connection has unknown error fd %d cir %p rv %d errno %d\n", fd, cir, rv, errno);
					goto Fail;
				}
			}
		}
	}

	event_add(&cir->network_event, 0 /*timeout*/);					
	
	uint64_t delta = cf_getms() - _s;
	if (delta > CL_LOG_DELAY_WARN) CL_LOG(CL_WARNING," CL_DELAY cl_info event again fn: %"PRIu64"\n",delta);

	return;
	
Fail:
	(*cir->user_cb) ( -1, 0 , 0,cir->user_data );
	event_del(&cir->network_event); // WARNING: this is not necessary. BOK says it is safe: maybe he's right, maybe wrong.
	close(fd);
	info_request_destroy(cir);
	g_cl_stats.info_complete++;
	g_cl_info_transactions--;
	
	delta = cf_getms() - _s;
	if (delta > CL_LOG_DELAY_WARN) CL_LOG(CL_WARNING,"  CL_DELAY: cl_info event fail OK took %"PRIu64"\n",delta);

	return;
	
}



//
// Request the info of a particular sockaddr_in,
// used internally for host-crawling as well as supporting the external interface
//

int
evcitrusleaf_info_host(struct sockaddr_in *sa_in, char *names, int timeout_ms,
	evcitrusleaf_info_callback cb, void *udata) 
{
	
	uint64_t _s = cf_getms();
	
	g_cl_stats.info_host_requests++;
	
	cl_info_request *cir = malloc(sizeof(cl_info_request));
	if (!cir)	return(-1);
	memset(cir, 0, sizeof(cl_info_request));
	
	cir->user_cb = cb;
	cir->user_data = udata;

	CL_LOG(CL_VERBOSE, "info: host: request for: %s\n",names);

	// Create the socket a little early, just in case
	int fd;
	if (-1 == (fd = socket ( AF_INET, SOCK_STREAM, 0 ))) {

		CL_LOG(CL_INFO, "could not allocate socket errno %d\n",errno);

		info_request_destroy(cir);
		
		uint64_t delta = cf_getms() - _s;
		if (delta > CL_LOG_DELAY_WARN) CL_LOG(CL_WARNING," CL_DELAY: info host no socket: %"PRIu64"\n",delta);
		
		return(-1);
	}

	// set nonblocking
	int flags;
	if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
		flags = 0;
	if (-1 == fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {

		CL_LOG(CL_WARNING, "info host fcntl failed\n");
		info_request_destroy(cir);
		close(fd);
		
		uint64_t delta = cf_getms() - _s;
		if (delta > CL_LOG_DELAY_WARN) CL_LOG(CL_WARNING," CL_DELAY: info host bad fcntl: %"PRIu64"\n",delta);
		
		return(-1);
	}

	sockaddr_in_dump(CL_VERBOSE, "  info connect to: ",sa_in);

	// Actually do the connect
	if (0 != connect(fd, (struct sockaddr *) sa_in, sizeof( *sa_in ) ))
	{
		if (errno != EINPROGRESS) {

			if (errno == ECONNREFUSED) {
				CL_LOG(CL_INFO, "host is refusing connections\n");
			} else {
				CL_LOG(CL_INFO, "info: connect request failed errno %d\n", errno);
				sockaddr_in_dump( CL_VERBOSE, "   sockaddr was ",sa_in);
			}
			
			info_request_destroy(cir);
			close(fd);

			uint64_t delta = cf_getms() - _s;
			if (delta > CL_LOG_DELAY_WARN) CL_LOG(CL_WARNING," CL_DELAY: info host no connect: %"PRIu64"\n",delta);

			return(-1);
		}
	}
	
	// fill the buffer while I'm waiting
	if (0 != info_make_request(cir, names)) {

		CL_LOG(CL_WARNING, "buffer fill failed\n");
		
		info_request_destroy(cir);
		close(fd);
		
		uint64_t delta = cf_getms() - _s;
		if (delta > CL_LOG_DELAY_WARN) CL_LOG(CL_WARNING," CL_DELAY: info host bad request: %"PRIu64"\n",delta);
		
		return(-1);
	}
	
	// setup for event
	event_set(&cir->network_event, fd, EV_WRITE | EV_READ, info_event_fn, (void *) cir);
	event_add(&cir->network_event, 0/*timeout*/);
	
	g_cl_info_transactions++;
	
	uint64_t delta = cf_getms() - _s;
	if (delta > CL_LOG_DELAY_WARN) CL_LOG(CL_WARNING," CL_DELAY: info host standard: %"PRIu64"\n",delta);

	
	return(0);
}

typedef struct {
	evcitrusleaf_info_callback cb;
	void *udata;
	char *names;
	uint32_t	timeout_ms;
} info_resolve_state;

// Got resolution - callback!
// 
// WARNING! It looks like a bug to have the possibilities fo multiple callbacks
// fired from this resolve function.
//

void
info_resolve_cb(int result, cf_vector *sockaddr_in_v, void *udata)
{
	info_resolve_state *irs = (info_resolve_state *)udata;
	if (result != 0) {
		CL_LOG(CL_INFO, "info resolution: async fail %d\n",result);
		(irs->cb) ( -1 /*return value*/, 0, 0 ,irs->udata );
		goto Done;
	}		
	for (uint i=0; i < cf_vector_size(sockaddr_in_v) ; i++) 
	{
		struct sockaddr_in  sa_in;
		cf_vector_get(sockaddr_in_v, i, &sa_in);

		if (0 != evcitrusleaf_info_host(&sa_in, irs->names, irs->timeout_ms, irs->cb, irs->udata )) {
			CL_LOG( CL_INFO, "info resolution: can't start infohost after resolve just failed\n");

			(irs->cb) ( -1 /*return value*/, 0, 0 ,irs->udata );
			goto Done;
		}
	}
Done:	
	g_cl_info_transactions--;
	free(irs->names);
	free(irs);
	
}

//
// External function is helper which goes after a particular hostname.
//
// TODO: timeouts are wrong here. If there are 3 host names, you'll end up with
// 3x timeout_ms
//

int
evcitrusleaf_info(char *host, short port, char *names, int timeout_ms,
	evcitrusleaf_info_callback cb, void *udata)
{
	int rv = -1;
	info_resolve_state *irs = 0;
	
	g_cl_stats.info_host_requests++;
	
	struct sockaddr_in sa_in;
	// if we can resolve immediate, jump directly to resolution
	if (0 == cl_lookup_immediate(host, port, &sa_in)) {
		if (0 == evcitrusleaf_info_host(&sa_in, names, timeout_ms, cb, udata )) {
			rv = 0;
			goto Done;
		}
	}
	else {
		irs = malloc( sizeof(info_resolve_state) );
		if (!irs)	goto Done;
		irs->cb = cb;
		irs->udata = udata;
		if (names) { 
			irs->names = strdup(names);
			if (!irs->names) goto Done;
		}
		else irs->names = 0;
		irs->timeout_ms = timeout_ms;
		if (0 != cl_lookup(host, port, info_resolve_cb, irs)) 
			goto Done;
		irs = 0;
		
		g_cl_info_transactions++;
		
	}
	
	
Done:
	if (irs) {
		if (irs->names)	free(irs->names);
		free(irs);
	}
	return(rv);
}

//
// When shutting down the entire module, need to make sure that
// all info requests pending are also shut down
//


void
evcitrusleaf_info_shutdown()
{
	while ( ( g_cl_info_transactions > 0 ) &&
		    (event_loop(EVLOOP_ONCE) == 0) )
	
	    ;
	return;
	
}


