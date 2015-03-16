/*
Copyright (c) 2012-2014 The icomet Authors. All rights reserved.
Use of this source code is governed by a BSD-style license that can be
found in the LICENSE file.
*/
#include "subscriber.h"
#include "channel.h"
#include "server.h"
#include "util/log.h"
#include "server_config.h"
#include <http-internal.h>

static std::string iframe_header = "<html><head><meta http-equiv='Content-Type' content='text/html; charset=utf-8'><meta http-equiv='Cache-Control' content='no-store'><meta http-equiv='Cache-Control' content='no-cache'><meta http-equiv='Pragma' content='no-cache'><meta http-equiv=' Expires' content='Thu, 1 Jan 1970 00:00:00 GMT'><script type='text/javascript'>window.onError = null;try{document.domain = window.location.hostname.split('.').slice(-2).join('.');}catch(e){};</script></head><body>";
static std::string iframe_chunk_prefix = "<script>parent.icomet_cb(";
static std::string iframe_chunk_suffix = ");</script>";

Subscriber::Subscriber(){
	req = NULL;
}

Subscriber::~Subscriber(){
}

static void on_sub_disconnect(struct evhttp_connection *evcon, void *arg){
	log_debug("subscriber disconnected");
	Subscriber *sub = (Subscriber *)arg;
	sub->close();
}

void Subscriber::start(){
	bufferevent_enable(req->evcon->bufev, EV_READ);
	evhttp_connection_set_closecb(req->evcon, on_sub_disconnect, this);
	evhttp_add_header(req->output_headers, "Connection", "keep-alive");
	//evhttp_add_header(req->output_headers, "Cache-Control", "no-cache");
	//evhttp_add_header(req->output_headers, "Expires", "0");
	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	evhttp_send_reply_start(req, HTTP_OK, "OK");

	if(this->type == POLL){
		//
	}else if(this->type == IFRAME){
		struct evbuffer *buf = evhttp_request_get_output_buffer(this->req);
		evbuffer_add_printf(buf, "%s\n", iframe_header.c_str());
		evhttp_send_reply_chunk(this->req, buf);
	}
	
	if(this->seq_next == 0){
		this->seq_next = channel->seq_next;
		if(channel->seq_next != 0){
			this->sync_next_seq();
		}
	}else{
		// send buffered messages
		if(!channel->msg_list.empty() && channel->seq_next != this->seq_next){
			this->send_old_msgs();
		}
	}
}

void Subscriber::send_old_msgs(){
	std::vector<std::string>::iterator it = channel->msg_list.end();
	int msg_seq_min = channel->seq_next - channel->msg_list.size();
	if(Channel::SEQ_GT(this->seq_next, channel->seq_next) || Channel::SEQ_LT(this->seq_next, msg_seq_min)){
		this->seq_next = msg_seq_min;
	}
	log_debug("send old msg: [%d, %d]", this->seq_next, channel->seq_next - 1);
	it -= (channel->seq_next - this->seq_next);

	struct evbuffer *buf = evhttp_request_get_output_buffer(this->req);
	if(this->type == POLL){
		if(!this->callback.empty()){
			evbuffer_add_printf(buf, "%s(", this->callback.c_str());
		}
		evbuffer_add_printf(buf, "[");
		for(/**/; it != channel->msg_list.end(); it++, this->seq_next++){
			std::string &msg = *it;
			evbuffer_add_printf(buf,
				"{\"type\":\"data\",\"cname\":\"%s\",\"seq\":%d,\"content\":\"%s\"}",
				this->channel->name.c_str(),
				this->seq_next,
				msg.c_str());
			if(this->seq_next != channel->seq_next - 1){
				evbuffer_add(buf, ",", 1);
			}
		}
		evbuffer_add_printf(buf, "]");
		if(!this->callback.empty()){
			evbuffer_add_printf(buf, ");");
		}
		evbuffer_add_printf(buf, "\n");
		evhttp_send_reply_chunk(this->req, buf);
		this->close();
	}else if(this->type == IFRAME || this->type == STREAM){
		for(/**/; it != channel->msg_list.end(); it++, this->seq_next++){
			std::string &msg = *it;
			this->send_chunk(this->seq_next, "data", msg.c_str());
		}
	}
}

void Subscriber::close(){
	if(req->evcon){
		evhttp_connection_set_closecb(req->evcon, NULL, NULL);
	}
	evhttp_send_reply_end(req);
	channel->serv->sub_end(this);
}

void Subscriber::noop(){
	this->send_chunk(this->seq_noop, "noop", "");
}

void Subscriber::sync_next_seq(){
	log_debug("%s:%d sync_next_seq: %d", req->remote_host, req->remote_port, seq_next);
	this->send_chunk(seq_next, "next_seq", NULL);
}

//edit just for test. write immediately can be mauch faster when 1000K conn in one channel pub msg.
void Subscriber::send_chunk(int seq, const char *type, const char *content){
	if(content == NULL){
		content = "";
	}

	struct evbuffer *buf = evhttp_request_get_output_buffer(this->req);
	char data_buf[1024] = {0}; 
	char out_buf[1152] = {0}; 
	
	if(this->type == POLL){
		if(!this->callback.empty()){
			snprintf(data_buf,sizeof(data_buf), "%s(", this->callback.c_str());
		}
	}else if(this->type == IFRAME){
		snprintf(data_buf,sizeof(data_buf), "%s",iframe_chunk_prefix.c_str());
	}
    
    snprintf(data_buf+strlen(data_buf),sizeof(data_buf)-strlen(data_buf),
		"{\"type\":\"%s\",\"cname\":\"%s\",\"seq\":%d,\"content\":\"%s\"}",
		type, this->channel->name.c_str(), seq, content);
    
	if(this->type == POLL){
		if(!this->callback.empty()){
			evbuffer_add_printf(buf, ");");
			snprintf(data_buf+strlen(data_buf),sizeof(data_buf)-strlen(data_buf), ");");
		}
	}else if(this->type == IFRAME){
		snprintf(data_buf+strlen(data_buf),sizeof(data_buf)-strlen(data_buf), "%s",iframe_chunk_suffix.c_str());
	}

	evbuffer_add_printf(buf, "\n");
	//snprintf(data_buf,sizeof(data_buf), "\n");
	strncat(data_buf, "\n", sizeof(data_buf));
	
	if ((this->req)->chunked) {
		snprintf(out_buf, sizeof(out_buf), "%x\r\n%s\r\n", strlen(data_buf), data_buf);
	}
	
	evhttp_connection * thisconn = (this->req)->evcon;
	int thisfd = thisconn->fd;
	unsigned int tosend_len = strlen(out_buf);
	ssize_t immediate_sendedlen = ::write(thisfd, out_buf, tosend_len);
	if(immediate_sendedlen < tosend_len ){
	    log_debug("wujianjiang:: after write[%s], immediate_sendedlen is %d, ",out_buf, immediate_sendedlen);
	    evbuffer_add_printf(buf, "%s", data_buf);
        evhttp_send_reply_chunk(this->req, buf);
	}
	this->idle = 0;
	if(this->type == POLL){
		this->close();
	}
}

void Subscriber::send_error_reply(int sub_type, struct evhttp_request *req, const char *cb, const std::string &cname, const char *type, const char *content){
	struct evbuffer *buf = evhttp_request_get_output_buffer(req);
	
	if(sub_type == POLL){
		evbuffer_add_printf(buf, "%s(", cb);
	}else if(sub_type == IFRAME){
		evbuffer_add_printf(buf, "%s", iframe_chunk_prefix.c_str());
	}
	
	evbuffer_add_printf(buf,
		"{\"type\":\"%s\",\"cname\":\"%s\",\"seq\":%d,\"content\":\"%s\"}",
		type, cname.c_str(), 0, content);

	if(sub_type == POLL){
		evbuffer_add_printf(buf, ");");
	}else if(sub_type == IFRAME){
		evbuffer_add_printf(buf, "%s", iframe_chunk_suffix.c_str());
	}

	evbuffer_add_printf(buf, "\n");
	evhttp_send_reply(req, HTTP_OK, "OK", buf);
}

