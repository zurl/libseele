#include <winsock2.h>
#include <MSWSock.h>
#include <vector>
#include <iostream>
#include <string>
#include <functional>
#include "socket.h"

/*  本文件高度参考 http://blog.csdn.net/piggyxp/article/details/6922277
 *
 *  原作者：PiggyXP【小猪】 http://blog.csdn.net/PiggyXP
 *
 *  本人对其进行改写以支持更多功能
 */

class Exception{};

// 缓冲区长度 (1024*8)
// 之所以为什么设置8K，也是一个江湖上的经验值
// 如果确实客户端发来的每组数据都比较少，那么就设置得小一些，省内存
#define MAX_BUFFER_LEN        8192  
// 默认端口
#define DEFAULT_PORT          12345
// 默认IP地址
#define DEFAULT_IP            "127.0.0.1"

//////////////////////////////////////////////////////////////////
// 在完成端口上投递的I/O操作的类型
typedef enum _OPERATION_TYPE  
{  
	ACCEPT_POSTED,                     // 标志投递的Accept操作
	SEND_POSTED,                       // 标志投递的是发送操作
	RECV_POSTED,                       // 标志投递的是接收操作
	NULL_POSTED                        // 用于初始化，无意义
}OPERATION_TYPE;

//====================================================================================
//
//				单IO数据结构体定义(用于每一个重叠操作的参数)
//
//====================================================================================

typedef struct _PER_IO_CONTEXT
{
	OVERLAPPED     m_Overlapped;                               // 每一个重叠网络操作的重叠结构(针对每一个Socket的每一个操作，都要有一个)              
	SOCKET         m_sockAccept;                               // 这个网络操作所使用的Socket
	WSABUF         m_wsaBuf;                                   // WSA类型的缓冲区，用于给重叠操作传参数的
	char           m_szBuffer[MAX_BUFFER_LEN];                 // 这个是WSABUF里具体存字符的缓冲区
	OPERATION_TYPE m_OpType;                                   // 标识网络操作的类型(对应上面的枚举)
    char           m_isFinal;
    char           m_isContinue;
	// 初始化
	_PER_IO_CONTEXT()
	{
		ZeroMemory(&m_Overlapped, sizeof(m_Overlapped));  
		ZeroMemory( m_szBuffer,MAX_BUFFER_LEN );
		m_sockAccept = INVALID_SOCKET;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = MAX_BUFFER_LEN;
		m_OpType     = NULL_POSTED;
        m_isFinal    = 0;
        m_isContinue = 0;
	}
	// 释放掉Socket
	~_PER_IO_CONTEXT()
	{
		if( m_sockAccept!=INVALID_SOCKET )
		{
			closesocket(m_sockAccept);
			m_sockAccept = INVALID_SOCKET;
		}
	}
	// 重置缓冲区内容
	void ResetBuffer()
	{
		ZeroMemory( m_szBuffer,MAX_BUFFER_LEN );
	}

} PER_IO_CONTEXT, *PPER_IO_CONTEXT;


//====================================================================================
//
//				单句柄数据结构体定义(用于每一个完成端口，也就是每一个Socket的参数)
//
//====================================================================================

typedef struct _PER_SOCKET_CONTEXT
{  
	SOCKET      m_Socket;                                  // 每一个客户端连接的Socket
	SOCKADDR_IN m_ClientAddr;                              // 客户端的地址
	std::vector<_PER_IO_CONTEXT*> m_arrayIoContext;             // 客户端网络操作的上下文数据，
	                                                       // 也就是说对于每一个客户端Socket，是可以在上面同时投递多个IO请求的

	// 初始化
	_PER_SOCKET_CONTEXT()
	{
		m_Socket = INVALID_SOCKET;
		memset(&m_ClientAddr, 0, sizeof(m_ClientAddr)); 
	}

	// 释放资源
	~_PER_SOCKET_CONTEXT()
	{
		if( m_Socket!=INVALID_SOCKET )
		{
			closesocket( m_Socket );
		    m_Socket = INVALID_SOCKET;
		}
		// 释放掉所有的IO上下文数据
		for( unsigned i=0;i<m_arrayIoContext.size();i++ )
		{
			delete m_arrayIoContext[i];
		}
		m_arrayIoContext.clear();
	}

	// 获取一个新的IoContext
	_PER_IO_CONTEXT* GetNewIoContext()
	{
		_PER_IO_CONTEXT* p = new _PER_IO_CONTEXT;

		m_arrayIoContext.emplace_back( p );

		return p;
	}

	// 从数组中移除一个指定的IoContext
	void RemoveContext( _PER_IO_CONTEXT* pContext )
	{
		if( pContext==NULL ){
            throw Exception();
        }

		for( int i=0;i<m_arrayIoContext.size();i++ )
		{
			if( pContext==m_arrayIoContext[i] )
			{
				delete pContext;
				pContext = NULL;
				m_arrayIoContext.erase(m_arrayIoContext.begin() + i);
				break;
			}
		}
	}

} PER_SOCKET_CONTEXT, *PPER_SOCKET_CONTEXT;




//====================================================================================
//
//				CIOCPModel类定义
//
//====================================================================================

// 工作者线程的线程参数
class IOCPModel;
typedef struct _tagThreadParams_WORKER
{
	IOCPModel* pIOCPModel;                                   // 类指针，用于调用类中的函数
	int         nThreadNo;                                    // 线程编号

} THREADPARAMS_WORKER,*PTHREADPARAM_WORKER; 

// CIOCPModel类
class IOCPModel : public SocketServer
{
public:
	IOCPModel(void);
	~IOCPModel(void);

public:

	// 启动服务器
	bool Start();

	//	停止服务器
	void Stop();

	// 加载Socket库
	bool LoadSocketLib();

	// 卸载Socket库，彻底完事
	void UnloadSocketLib() { WSACleanup(); }

	// 设置监听端口
	void SetListenPort( const int& nPort ) { m_nPort=nPort; }
    //设置监听IP
    void SetListenIP (std::string & strIP) { m_strIP=strIP; }

    bool RegisterCallback( ISocketCallback * cb){
        m_pIOCPCallback = cb;
    }


protected:

	// 初始化IOCP
	bool _InitializeIOCP();

	// 初始化Socket
	bool _InitializeListenSocket();

	// 最后释放资源
	void _DeInitialize();

	// 投递Accept请求
	bool _PostAccept( PER_IO_CONTEXT* pAcceptIoContext ); 

	// 投递接收数据请求
	bool _PostRecv( PER_IO_CONTEXT* pIoContext);

    // 投递发送数据请求
    bool _PostSend(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

	// 在有客户端连入的时候，进行处理
	bool _DoAccpet( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext );

	// 在有接收的数据到达的时候，进行处理
	bool _DoRecv( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext );

    // 在有发送的数据就绪的时候，进行处理
    bool _DoSend( PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext );

	// 将客户端的相关信息存储到数组中
	void _AddToContextList( PER_SOCKET_CONTEXT *pSocketContext );

	// 将客户端的信息从数组中移除
	void _RemoveContext( PER_SOCKET_CONTEXT *pSocketContext );

	// 清空客户端信息
	void _ClearContextList();

	// 将句柄绑定到完成端口中
	bool _AssociateWithIOCP( PER_SOCKET_CONTEXT *pContext);

	// 处理完成端口上的错误
	bool HandleError( PER_SOCKET_CONTEXT *pContext,const DWORD& dwErr );

	// 线程函数，为IOCP请求服务的工作者线程
	static DWORD WINAPI _WorkerThread(LPVOID lpParam);

	// 获得本机的处理器数量
	int _GetNoOfProcessors();

	// 判断客户端Socket是否已经断开
	bool _IsSocketAlive(SOCKET s);



private:
	ISocketCallback *              m_pIOCPCallback;

	HANDLE                       m_hShutdownEvent;              // 用来通知线程系统退出的事件，为了能够更好的退出线程

	HANDLE                       m_hIOCompletionPort;           // 完成端口的句柄

	HANDLE*                      m_phWorkerThreads;             // 工作者线程的句柄指针

	int		                     m_nThreads;                    // 生成的线程数量

	std::string                  m_strIP;                       // 服务器端的IP地址

	int                          m_nPort;                       // 服务器端的监听端口

	CRITICAL_SECTION             m_csContextList;               // 用于Worker线程同步的互斥量

	std::vector<PER_SOCKET_CONTEXT*>  m_arrayClientContext;     // 客户端Socket的Context信息

	PER_SOCKET_CONTEXT*          m_pListenContext;              // 用于监听的Socket的Context信息

	LPFN_ACCEPTEX                m_lpfnAcceptEx;                // AcceptEx 和 GetAcceptExSockaddrs 的函数指针，用于调用这两个扩展函数
	LPFN_GETACCEPTEXSOCKADDRS    m_lpfnGetAcceptExSockAddrs; 

};

