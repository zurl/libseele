#include <iostream>
#include <algorithm>
#include <thread>
#include <future>
#include <utility>
#include <tuple>
#include <memory>
#include "helper.h"
#include <map>
#include <unordered_map>
#include "IOCPModel.h"
#include "socket.h"

int printf_w(const char *format, ...);

extern CRITICAL_SECTION cs_send;
extern CRITICAL_SECTION cs_printf;
int printf_wnod(const char *format, ...) {

    ::EnterCriticalSection(&cs_printf);
    va_list ap;
    va_start(ap, format);
    //vprintf(format, ap);
    va_end(ap);
    ::LeaveCriticalSection(&cs_printf);

}

class HTTPServer : public IOCPModel::ISocketCallback {
private:

    unsigned long long __hash_socket_id(unsigned int ip_addr, unsigned int ip_port) {
        return (((unsigned long long) ip_addr) << 32) | ip_port;
    }


    IOCPModel iocp;

public:

    class HTTPHeader{
    private:

    public:
        class HTTPHeaderFormatError{
            int infocode;
        public:
            HTTPHeaderFormatError(int c):infocode(c){}
        };
        std::unordered_map<std::string,std::string> field;
        void set_header(std::string && key, std::string && value){
            field[key] = value;
        }
        void set_header(std::string && key, const std::string & value){
            field[key] = value;
        }
    };

    class HTTPResponseHeader : public HTTPHeader {
    public:
        std::string http_version = "HTTP/1.1";
        std::string status_code  = "200"     ;
        std::string message      = "OK"      ;
        HTTPResponseHeader(){
            set_header("Server","libseele/0.7(Windows NT 10.0, x64)");
        }


        std::string get_str(){
            std::string ret = http_version + " " + status_code + " " + message + "\r\n";
            for( auto & x : field){
                ret += x.first + ": " + x.second + "\r\n";
            }
            ret += "\r\n";
            return ret;
        }
    };

    class HTTPResponse{
    public:
        std::unique_ptr<HTTPResponseHeader> httpResponseHeader
                = std::unique_ptr<HTTPResponseHeader>(new HTTPResponseHeader());
        std::string body = "test";
        std::string get_str(){
            httpResponseHeader->set_header("Content-Length",Helper::itos(body.length()));
            httpResponseHeader->set_header("Connection","close");
            return httpResponseHeader->get_str() + body;
        }
    };

    class HTTPRequestHeader : public HTTPHeader{
    public:
        std::string method;
        std::string path;
        std::string http_version;
        HTTPRequestHeader(const char * buffer){
            //Header Format Check
            const char * saved = buffer;
            //first line
            int flag = 0;
            while( *buffer != '\r' || *(buffer + 1) != '\n'){
                if(*buffer == ' '){
                    flag ++;
                    if( flag == 1 )method = std::string(saved, buffer - saved);
                    if( flag == 2 )path = std::string(saved, buffer - saved);
                    saved = buffer + 1;
                }
                buffer++;
            }
            if(flag != 2) throw HTTPHeaderFormatError(1);
            http_version = std::string(saved, buffer - saved);
            buffer += 2;
            saved = buffer;
            //Args List
            while( *buffer != '\r' || *(buffer + 1) != '\n' ){
                const char * saved_back = nullptr;
                while( *buffer != '\r' || *(buffer + 1) != '\n'){
                    if(*buffer == ':' && *(buffer + 1) == ' '){
                        saved_back = buffer;
                    }
                    buffer++;
                }
                if( saved_back == nullptr )throw HTTPHeaderFormatError(2);
                field.emplace( std::piecewise_construct,
                               std::forward_as_tuple(saved, saved_back - saved),
                               std::forward_as_tuple(saved_back + 2, buffer - saved_back - 2));
                buffer += 2;
                saved = buffer;
            }
        }
    };

    class HTTPRequest {
        friend class HTTPServer;
    public:
        std::unique_ptr<HTTPRequestHeader> httpRequestHeader = nullptr;
        bool is_keep_alive(){
            return keep_alive;
        }
    private:
        enum class Status {
            RECV_HEADER_ON, RECV_DATA_ON, RECV_UP, RECV_ERROR
        };
        std::string buffer = "";
        int header_end_pos = -1;
        int content_length_rest = -1;
        bool keep_alive = false;

        Status status = Status::RECV_HEADER_ON;
        void append_buffer(const char * buf){
            buffer += std::string(buf);
        }
        bool check_header(){
            for(int i=header_end_pos+1;i<=(int)buffer.length()-4;i++){
                    if(buffer[i] == '\r' && buffer[i+1] == '\n'
                   && buffer[i+2] == '\r' && buffer[i+3] == '\n'){
                    header_end_pos = i+3;
                    status = Status::RECV_DATA_ON;
                    return true;
                }
            }
            header_end_pos =(int)(buffer.length() - 1);
            status = Status::RECV_HEADER_ON;
            return false;
        }
        bool is_ready(){
            return status == Status::RECV_UP;
        }
        bool accept_buf(const char *buf){
            switch(status){
                case HTTPRequest::Status::RECV_HEADER_ON:
                    append_buffer(buf);
                    if(check_header()){
                        try{
                            httpRequestHeader = std::make_unique<HTTPRequestHeader>(buf);
                            auto cn = httpRequestHeader->field.find("Connection");
                            if( cn == httpRequestHeader->field.end() || cn->second != "keep-alive"){
                                //short-connection
                                status = HTTPRequest::Status::RECV_DATA_ON;
                                return false;
                            }else{
                                //Keep-alive
                                keep_alive = true;
                                if(httpRequestHeader->method == "GET" || httpRequestHeader->method == "HEAD"){
                                    //No body
                                    status = HTTPRequest::Status::RECV_UP;
                                    return true;
                                }else{
                                    auto cl = httpRequestHeader->field.find("Content-Length");
                                    if( cl == httpRequestHeader->field.end())throw HTTPHeader::HTTPHeaderFormatError(3);
                                    content_length_rest = Helper::stoi(cl->second);
                                    if( content_length_rest + header_end_pos >= buffer.size()){
                                        status = HTTPRequest::Status::RECV_UP;
                                        return true;
                                    }else{
                                        status = HTTPRequest::Status::RECV_DATA_ON;
                                        return false;
                                    }
                                }
                            }
                        }catch(HTTPHeader::HTTPHeaderFormatError e){
                            status = HTTPRequest::Status::RECV_ERROR;
                            return true;
                        }
                    }
                    break;
                case HTTPRequest::Status::RECV_DATA_ON:
                    append_buffer(buf);
                    if(content_length_rest == -1){
                        //short-connection
                        return false;
                    }else{
                        if( content_length_rest + header_end_pos >= buffer.size()){
                            status = HTTPRequest::Status::RECV_UP;
                            return true;
                        }else{
                            return false;
                        }
                    }
                    break;
                case HTTPRequest::Status::RECV_UP:
                    break;
            }
        }

    };

    class UserContext{
    public:
        HTTPRequest httpRequest;
        HTTPResponse httpResponse;
    };

    class IHTTPRequestHandler{
    public:
        virtual void req_cb(HTTPServer::HTTPRequest & req,HTTPServer::HTTPResponse & res) = 0;
        virtual void req_cb_err(HTTPServer::HTTPRequest & req,HTTPServer::HTTPResponse & res) = 0;
    };
    HTTPServer() {}

    void register_http_request_callback(IHTTPRequestHandler * httpRequestHandlerPtr){
        httpRequestHandler = httpRequestHandlerPtr;
    }

    void listen(int port) {
        iocp.LoadSocketLib();
        iocp.SetListenPort(port);
        iocp.RegisterCallback((SocketServer::ISocketCallback *) this);
        iocp.Start();
    }

private:
    int recv_cb(unsigned int ip_addr, unsigned int ip_port, const char *buf) override {
        ::EnterCriticalSection(&cs_send);
        auto &ctx = userContext[__hash_socket_id(ip_addr, ip_port)];
        ::LeaveCriticalSection(&cs_send);
        printf_wnod("client %d:%d infomation:.%s\n", ip_addr, ip_port,buf);
        if(ctx.httpRequest.accept_buf(buf)){
            if(ctx.httpRequest.status == HTTPRequest::Status::RECV_ERROR){
                httpRequestHandler->req_cb_err(ctx.httpRequest,ctx.httpResponse);
            }else{
                httpRequestHandler->req_cb(ctx.httpRequest,ctx.httpResponse);
            }
            return 0;
        }else{
            return 1;
        }
    }

    int send_cb(unsigned int ip_addr, unsigned int ip_port, char *buf) override {
        ::EnterCriticalSection(&cs_send);
        auto &ctx = userContext[__hash_socket_id(ip_addr, ip_port)];
        ::LeaveCriticalSection(&cs_send);
        strcpy(buf,ctx.httpResponse.get_str().c_str());
        ::EnterCriticalSection(&cs_send);
        userContext[__hash_socket_id(ip_addr, ip_port)] = UserContext();
        ::LeaveCriticalSection(&cs_send);
        printf_wnod("client %d:%d send! len %s\n", ip_addr, ip_port, buf);
        //strcpy(buf, "Hello World!");
        return 0;
    }

    int connect_cb(unsigned int ip_addr, unsigned int ip_port) override {
        static int cnt_cnt = 0;
        ::EnterCriticalSection(&cs_send);
        userContext.emplace(std::piecewise_construct,
                            std::forward_as_tuple(__hash_socket_id(ip_addr, ip_port)),
                            std::forward_as_tuple());
        ::LeaveCriticalSection(&cs_send);


        cnt_cnt ++;
        printf_wnod("connect:%d!\n",cnt_cnt);
    }

    int disconnect_cb(unsigned int ip_addr, unsigned int ip_port) override {
        ::EnterCriticalSection(&cs_send);
        auto &ctx = userContext[__hash_socket_id(ip_addr, ip_port)];
        ::LeaveCriticalSection(&cs_send);
        if(ctx.httpRequest.is_ready()){
            httpRequestHandler->req_cb(ctx.httpRequest,ctx.httpResponse);
        }else{
            httpRequestHandler->req_cb_err(ctx.httpRequest,ctx.httpResponse);
        }
        ::EnterCriticalSection(&cs_send);
        userContext.erase(__hash_socket_id(ip_addr, ip_port));
        ::LeaveCriticalSection(&cs_send);
        printf_wnod("dis\n");
    }
    std::map<unsigned long long, UserContext> userContext;
    IHTTPRequestHandler * httpRequestHandler = nullptr;
};

class MyHTTPRequestHandler : public HTTPServer::IHTTPRequestHandler{
    virtual void req_cb(HTTPServer::HTTPRequest & req,HTTPServer::HTTPResponse & res){
        res.httpResponseHeader->set_header("My-Daye","XGS");
        res.body = "<html><body><h1>Hello World</h1></body></html>";
        req.httpRequestHeader;
    }
    virtual void req_cb_err(HTTPServer::HTTPRequest & req,HTTPServer::HTTPResponse & res){
        res.httpResponseHeader->status_code = "400";
        res.httpResponseHeader->message     = "Bad Request";
    }
};





int main(){
    HTTPServer http;
    http.register_http_request_callback(new MyHTTPRequestHandler());
    http.listen(1234);
    while (1);
    return 0;
}