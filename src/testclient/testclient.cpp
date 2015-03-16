#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string>
#include <set>
#include <list>
#include <vector>
#include <map>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <iconv.h>
#include <string.h>
#include <assert.h>


int main(int argc, char **argv)
{
    if (argc != 4)
    {
        printf("Usage: %s svr_ip channelid subscribe_num \n",argv[0]);
        return -1;
    }

	int subscribe_num = atol(argv[3]); // 所有客户端都传一样的，先测都在一个channel
    
    char _ip[16];
    snprintf(_ip, sizeof(_ip), "%s", argv[1]);
	int _port = 8000;
    int _port2 = 8100;
    int _fd;
    int _channel = atol(argv[2]);
    
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(client_socket < 0)
    {
        fprintf(stderr, "Create Socket Failed!error:[%s]\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    if(inet_aton(_ip, &server_addr.sin_addr) == 0)
    {
        fprintf(stderr, "Server IP Address Error!\nerror:[%s]\n", strerror(errno));
        return -1;
    }
    server_addr.sin_port = htons(_port);
    socklen_t server_addr_length = sizeof(server_addr);
    
    
    if(::connect(client_socket, (struct sockaddr *) &server_addr, server_addr_length) < 0)
    {
        fprintf(stderr, "fd[%d] can not connect error is [%s]\n", client_socket, strerror(errno));
        ::close(client_socket);
        return -1;
    }
    _fd = client_socket;
    
    char buffer[1024] = {0};
    
    //先用内部端口sign出一个channel，并拿到token。注：正式情况sign要由业务服务器来做，客户端sub之前先直接去业务服务器get channel。业务服务器负责分配
    //sign channel
    snprintf(buffer,sizeof(buffer),"GET /sign?cname=%d HTTP/1.1\r\nHost: channel.client.xunlei.com\r\nAccept: */*\r\n\r\n",_channel);
    int writenum = ::write(_fd, buffer, strlen(buffer));
    if (writenum < 0)
    {
    	printf("fd[%d]send data Failed!\nerror:[%s]\n", _fd,strerror(errno));
    	::close(_fd);
        return -1;
    }

    bzero(buffer,sizeof(buffer));
    int length = ::read(_fd,buffer,sizeof(buffer));

    if(length < 0)
    {
        printf("fd[%d]recieve data Failed!\nerror:[%s]\n",_fd,strerror(errno));
        ::close(_fd);
        return -1;
    }
    //拿到token
    std::string _token = "";
    std::string package = buffer;
    int i = package.find("\r\n\r\n");
    if(i == -1)
    {
    	printf("fd[%d]wrong package!get [%s]\n",_fd,buffer);
    	::close(_fd);
    	return -1;
    }
    else
    {
    	std::string body = package.substr(i + 4);
    	int token_s = body.find("token\":");
    	int token_e = body.find("\",\"expires");
    	if(token_s > 0 && token_e >token_s)
    	{
    	    _token = body.substr(token_s + sizeof("token\":"), token_e - token_s - sizeof("token\":"));
    	}
    	else {
    	    printf("fd[%d]find no token in resp[%s]\n",_fd,body.c_str());
    	    ::close(_fd);
    	    return -1;
    	}
	}
	printf("fetch token:%s\n", _token.c_str());
	::close(_fd);

    int _epfd =::epoll_create(subscribe_num);
    if(-1 == _epfd)
    {
        printf("epoll_create return -1...errno[%d] errinfo[%s]\n", errno, strerror(errno));
        return -1;
    }
    int idx;
    for(idx = 0; idx < subscribe_num; idx++)
    {
        int client_socket2 = socket(AF_INET, SOCK_STREAM, 0);
        if(client_socket2 < 0)
        {
            fprintf(stderr, "count[%d]::Create Socket Failed!error:[%s]\n", idx, strerror(errno));
            ::close(_epfd);
            return -1;
        }
    
        struct sockaddr_in server_addr2;
        bzero(&server_addr2, sizeof(server_addr2));
        server_addr2.sin_family = AF_INET;
        if(inet_aton(_ip, &server_addr2.sin_addr) == 0)
        {
            fprintf(stderr, "count[%d]::Server IP Address Error!\nerror:[%s]\n", idx, strerror(errno));
            ::close(_epfd);
            return -1;
        }
        server_addr2.sin_port = htons(_port2);
        socklen_t server_addr_length2 = sizeof(server_addr2);
        
        /*去掉超时永不关闭
        //struct timeval timeout;
        //timeout.tv_sec = 1;
        //timeout.tv_usec = 0;
        //setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *) &timeout, sizeof(struct timeval));
        */
        
        if(::connect(client_socket2, (struct sockaddr *) &server_addr2, server_addr_length2) < 0)
        {
            fprintf(stderr, "count[%d]::fd[%d] can not connect error is [%s]\n", idx, client_socket2, strerror(errno));
            ::close(client_socket2);
            ::close(_epfd);
            return -1;
        }
        
        //提交sub
        snprintf(buffer,sizeof(buffer), "GET /stream?cname=%d&seq=%d&token=%s HTTP/1.1\r\nHost: channel.client.xunlei.com\r\nAccept: */*\r\n\r\n", _channel, 0, _token.c_str());
        int writenum = ::write(client_socket2, buffer, strlen(buffer));
        if (writenum < 0)
        {
        	printf("count[%d]::fd[%d]send data Failed!\nerror:[%s]\n", idx, client_socket2,strerror(errno));
        	::close(client_socket2);
        	::close(_epfd);
            return -1;
        }
        
        int flags = fcntl(client_socket2, F_GETFL, 0);
        fcntl(client_socket2, F_SETFL, flags | O_NONBLOCK);
        
        struct epoll_event tmpEvent;
        tmpEvent.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLET;
        tmpEvent.data.fd = client_socket2;
        int ret =::epoll_ctl(_epfd, EPOLL_CTL_ADD, client_socket2, &tmpEvent);
        if(0 != ret)
        {
            printf("count[%d]::fd[%d]epoll_ctl(%d,EPOLL_CTL_ADD,%d,tmpEvent[EPOLLOUT,%d]) read failed, errno[%d] errinfo[%s]",
					idx, client_socket2, _epfd, client_socket2, client_socket2, errno, strerror(errno));
            ::close(client_socket2);
        	::close(_epfd);
            return -1;
        }
		//添加进度信息
		fprintf(stderr, "%05.2f%%\b\b\b\b\b\b", idx/float(subscribe_num) * 100);
    }
	fprintf(stderr, "%05.2f%%\n", idx/float(subscribe_num) * 100);

    printf("connect compeleted, waiting for msg from icomet svr......\n");
    struct epoll_event _events[subscribe_num + 1];
    
	char sizebuff[16];
	char *pos = NULL;
	char *pos2 = NULL;
	int success_cnt = 0;
	int failed_cnt = 0;
	int total_cnt = 0;
    while(1)
    {
        int nfds =::epoll_wait(_epfd, _events, subscribe_num, -1);
        for(int i = 0; i < nfds; ++i)
        {
            struct epoll_event &_event = _events[i];
            if((_event.events & EPOLLERR) == EPOLLERR || (_event.events & EPOLLHUP) == EPOLLHUP)
            {
				printf("warning! fd[%d] ::epoll_ctl(%d,EPOLL_CTL_DEL,%d,tmpEvent[EPOLLIN,%d]) read failed, errno[%d] errinfo[%s]", 
						_event.data.fd, _epfd, _event.data.fd, _event.data.fd, errno, strerror(errno));
				++failed_cnt; 
                continue;
            }
            else if((_event.events & EPOLLIN) == EPOLLIN)
            {
                total_cnt++;
                int n =::read(_event.data.fd, buffer, sizeof(buffer));
				buffer[n] = 0;
				pos = strstr(buffer, "\r\n");
				memcpy(sizebuff, buffer, pos - buffer);
				sizebuff[pos - buffer] = 0;
				int size = strtol(sizebuff, NULL, 16);
				if (size == 0)
					continue;
				pos2 = strstr(pos + 2, "\r\n");
				if (size != pos2 - pos - 2)
				{
					printf("fd[%d]:content size not match, %d, %d\n", _event.data.fd, size, pos2 - pos -2);
					printf("buffer = [%s]\n", buffer);
				}
				else
				{
					++success_cnt;
				}
					;//printf("fd[%d]:content size match\n", _event.data.fd);
            }
            else
            {
                printf("!important, epoll core error:  errno[%d], errinfo[%s]\n", errno, strerror(errno));
                ::close(_event.data.fd);
            	::close(_epfd);
                return -1;
            }
        }

        if (total_cnt >= subscribe_num)
        {
            printf("本次响应成功%d个, 失败%d\n", success_cnt, failed_cnt);
            total_cnt = 0;
			success_cnt = 0;
			failed_cnt = 0;
        }
    }
    
    return 0;
}
