//
// Created by zcy on 2016-09-16.
//

#ifndef TESTCPP11_SOCKET_H_H
#define TESTCPP11_SOCKET_H_H

class SocketServer{
public:
    class ISocketCallback{
    public:
        virtual int connect_cb(unsigned int ip_addr,unsigned int ip_port) = 0;
        virtual int disconnect_cb(unsigned int ip_addr,unsigned int ip_port) = 0;
        virtual int recv_cb(unsigned int ip_addr,unsigned int ip_port,const char * buf) = 0;
        virtual int send_cb(unsigned int ip_addr,unsigned int ip_port,char * buf) = 0;
    };

};


#endif //TESTCPP11_SOCKET_H_H
