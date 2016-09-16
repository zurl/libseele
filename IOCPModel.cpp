#include <inaddr.h>
#include "IOCPModel.h"
#include "stdio.h"

/*  本文件高度参考 http://blog.csdn.net/piggyxp/article/details/6922277
 *
 *  原作者：PiggyXP【小猪】 http://blog.csdn.net/PiggyXP
 *
 *  本人对其进行改写以支持更多功能
 */

// 每一个处理器上产生多少个线程(为了最大限度的提升服务器性能，详见配套文档)
#define WORKER_THREADS_PER_PROCESSOR 2
// 同时投递的Accept请求的数量(这个要根据实际的情况灵活设置)
#define MAX_POST_ACCEPT              10
// 传递给Worker线程的退出信号
#define EXIT_CODE                    NULL

// 释放指针和句柄资源的宏

// 释放指针宏
#define RELEASE(x)                      {if(x != NULL ){delete x;x=NULL;}}
// 释放句柄宏
#define RELEASE_HANDLE(x)               {if(x != NULL && x!=INVALID_HANDLE_VALUE){ CloseHandle(x);x = NULL;}}
// 释放Socket宏
#define RELEASE_SOCKET(x)               {if(x !=INVALID_SOCKET) { closesocket(x);x=INVALID_SOCKET;}}

CRITICAL_SECTION cs_printf;

int printf_w(const char *format, ...) {
    ::EnterCriticalSection(&cs_printf);
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
    ::LeaveCriticalSection(&cs_printf);
}

IOCPModel::IOCPModel(void) :
        m_nThreads(0),
        m_hShutdownEvent(NULL),
        m_hIOCompletionPort(NULL),
        m_phWorkerThreads(NULL),
        m_strIP(DEFAULT_IP),
        m_nPort(DEFAULT_PORT),
        m_lpfnAcceptEx(NULL),
        m_pListenContext(NULL) {
    ::InitializeCriticalSection(&cs_printf);

}


IOCPModel::~IOCPModel(void) {
    // 确保资源彻底释放
    ::DeleteCriticalSection(&cs_printf);
    this->Stop();
}




///////////////////////////////////////////////////////////////////
// 工作者线程：  为IOCP请求服务的工作者线程
//         也就是每当完成端口上出现了完成数据包，就将之取出来进行处理的线程
///////////////////////////////////////////////////////////////////

DWORD WINAPI IOCPModel::_WorkerThread(LPVOID lpParam) {
    THREADPARAMS_WORKER *pParam = (THREADPARAMS_WORKER *) lpParam;
    IOCPModel *pIOCPModel = pParam->pIOCPModel;
    int nThreadNo = pParam->nThreadNo;

    printf_w("worker thread start ID: %d.\n", nThreadNo);

    OVERLAPPED *pOverlapped = NULL;
    PER_SOCKET_CONTEXT *pSocketContext = NULL;
    DWORD dwBytesTransfered = 0;

    // 循环处理请求，知道接收到Shutdown信息为止
    while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPModel->m_hShutdownEvent, 0)) {
        BOOL bReturn = GetQueuedCompletionStatus(
                pIOCPModel->m_hIOCompletionPort,
                &dwBytesTransfered,
                (PULONG_PTR) &pSocketContext,
                &pOverlapped,
                INFINITE);

        // 如果收到的是退出标志，则直接退出
        if (EXIT_CODE == pSocketContext) {
            break;
        }

        // 判断是否出现了错误
        if (!bReturn) {
            DWORD dwErr = GetLastError();

            // 显示一下提示信息
            if (!pIOCPModel->HandleError(pSocketContext, dwErr)) {
                break;
            }

            continue;
        }
        else {
            // 读取传入的参数
            PER_IO_CONTEXT *pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);

            // 判断是否有客户端断开了
            if ((0 == dwBytesTransfered) &&
                (RECV_POSTED == pIoContext->m_OpType || SEND_POSTED == pIoContext->m_OpType)) {
                printf_w("client %s:%d disconnected.\n", inet_ntoa(pSocketContext->m_ClientAddr.sin_addr),
                         ntohs(pSocketContext->m_ClientAddr.sin_port));
                auto nRet = pIOCPModel->m_pIOCPCallback->disconnect_cb(
                        pSocketContext->m_ClientAddr.sin_addr.S_un.S_addr, pSocketContext->m_ClientAddr.sin_port);
                if (nRet == 0) {
                    pIoContext->m_isFinal = 1;
                    pIOCPModel->_PostSend(pSocketContext, pIoContext);
                }
                else {
                    pIOCPModel->_RemoveContext(pSocketContext);
                }
                continue;
            }
            else {
                switch (pIoContext->m_OpType) {
                    // Accept
                    case ACCEPT_POSTED:
                        pIOCPModel->_DoAccpet(pSocketContext, pIoContext);
                        break;
                        // RECV
                    case RECV_POSTED:
                        pIOCPModel->_DoRecv(pSocketContext, pIoContext);
                        break;
                    case SEND_POSTED:
                        pIOCPModel->_DoSend(pSocketContext, pIoContext);
                        break;
                    default:
                        printf_w("_WorkThread  pIoContext->m_OpType param error.\n");
                        break;
                } //switch
            }//if
        }//if

    }//while

    printf_w("worker thread %d exited.\n", nThreadNo);

    // 释放线程参数
    RELEASE(lpParam);

    return 0;
}



//====================================================================================
//
//				    系统初始化和终止
//
//====================================================================================




////////////////////////////////////////////////////////////////////
// 初始化WinSock 2.2
bool IOCPModel::LoadSocketLib() {
    WSADATA wsaData;
    int nResult;
    nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    // 错误(一般都不可能出现)
    if (NO_ERROR != nResult) {
        printf_w("init WinSock 2.2 failed！\n");
        return false;
    }

    return true;
}

//////////////////////////////////////////////////////////////////
//	启动服务器
bool IOCPModel::Start() {

    InitializeCriticalSection(&m_csContextList);

    m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (!_InitializeIOCP()) {
        printf_w("initialize IOCP failed！\n");
        return false;
    }
    else {
        printf_w("\nIOCP initialization successfully\n.");
    }

    if (!_InitializeListenSocket()) {
        printf_w("Listen Socket initialization failed\n");
        this->_DeInitialize();
        return false;
    }
    else {
        printf_w("Listen Socket initialization successful.\n");
    }

    printf_w("System has been ready waiting for connection....\n");

    return true;
}


////////////////////////////////////////////////////////////////////
//	开始发送系统退出消息，退出完成端口和线程资源
void IOCPModel::Stop() {
    if (m_pListenContext != NULL && m_pListenContext->m_Socket != INVALID_SOCKET) {
        SetEvent(m_hShutdownEvent);

        for (int i = 0; i < m_nThreads; i++) {
            PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD) EXIT_CODE, NULL);
        }

        WaitForMultipleObjects((DWORD) m_nThreads, m_phWorkerThreads, TRUE, INFINITE);

        this->_ClearContextList();

        this->_DeInitialize();

        printf_w("stop listening\n");
    }
}


////////////////////////////////
// 初始化完成端口
bool IOCPModel::_InitializeIOCP() {
    // 建立第一个完成端口
    m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    if (NULL == m_hIOCompletionPort) {
        printf_w("Create IOCP failed！Error: %d!\n", WSAGetLastError());
        return false;
    }

    // 根据本机中的处理器数量，建立对应的线程数
    m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors();

    // 为工作者线程初始化句柄
    m_phWorkerThreads = new HANDLE[m_nThreads];

    // 根据计算出来的数量建立工作者线程
    DWORD nThreadID;
    for (int i = 0; i < m_nThreads; i++) {
        THREADPARAMS_WORKER *pThreadParams = new THREADPARAMS_WORKER;
        pThreadParams->pIOCPModel = this;
        pThreadParams->nThreadNo = i + 1;
        m_phWorkerThreads[i] = ::CreateThread(0, 0, _WorkerThread, (void *) pThreadParams, 0, &nThreadID);
    }

    printf_w(" create _WorkerThread %d.\n", m_nThreads);

    return true;
}


/////////////////////////////////////////////////////////////////
// 初始化Socket
bool IOCPModel::_InitializeListenSocket() {
    // AcceptEx 和 GetAcceptExSockaddrs 的GUID，用于导出函数指针
    GUID GuidAcceptEx = WSAID_ACCEPTEX;
    GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;

    // 服务器地址信息，用于绑定Socket
    struct sockaddr_in ServerAddress;

    // 生成用于监听的Socket的信息
    m_pListenContext = new PER_SOCKET_CONTEXT;

    // 需要使用重叠IO，必须得使用WSASocket来建立Socket，才可以支持重叠IO操作
    m_pListenContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == m_pListenContext->m_Socket) {
        printf_w("initialize Socket failed %d.\n", WSAGetLastError());
        return false;
    }
    else {
        printf_w("WSASocket() OK.\n");
    }

    // 将Listen Socket绑定至完成端口中
    if (NULL ==
        CreateIoCompletionPort((HANDLE) m_pListenContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR) m_pListenContext,
                               0)) {
        printf_w("binding Listen Socket to IOCP failed %d\n", WSAGetLastError());
        RELEASE_SOCKET(m_pListenContext->m_Socket);
        return false;
    }
    else {
        printf_w("Listen Socket binding OK.\n");
    }

    // 填充地址信息
    ZeroMemory((char *) &ServerAddress, sizeof(ServerAddress));
    ServerAddress.sin_family = AF_INET;
    // 这里可以绑定任何可用的IP地址，或者绑定一个指定的IP地址
    //ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    ServerAddress.sin_addr.s_addr = inet_addr(m_strIP.c_str());
    ServerAddress.sin_port = htons(m_nPort);

    // 绑定地址和端口
    if (SOCKET_ERROR == bind(m_pListenContext->m_Socket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress))) {
        printf_w("bind() failed.\n");
        return false;
    }
    else {
        printf_w("bind() completed.\n");
    }

    // 开始进行监听
    if (SOCKET_ERROR == listen(m_pListenContext->m_Socket, SOMAXCONN)) {
        printf_w("Listen() error.\n");
        return false;
    }
    else {
        printf_w("Listen() finished.\n");
    }

    // 使用AcceptEx函数，因为这个是属于WinSock2规范之外的微软另外提供的扩展函数
    // 所以需要额外获取一下函数的指针，
    // 获取AcceptEx函数指针
    DWORD dwBytes = 0;
    if (SOCKET_ERROR == WSAIoctl(
            m_pListenContext->m_Socket,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &GuidAcceptEx,
            sizeof(GuidAcceptEx),
            &m_lpfnAcceptEx,
            sizeof(m_lpfnAcceptEx),
            &dwBytes,
            NULL,
            NULL)) {
        printf_w("WSAIoctl cannot get AcceptEx func ptr %d\n", WSAGetLastError());
        this->_DeInitialize();
        return false;
    }

    // 获取GetAcceptExSockAddrs函数指针，也是同理
    if (SOCKET_ERROR == WSAIoctl(
            m_pListenContext->m_Socket,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &GuidGetAcceptExSockAddrs,
            sizeof(GuidGetAcceptExSockAddrs),
            &m_lpfnGetAcceptExSockAddrs,
            sizeof(m_lpfnGetAcceptExSockAddrs),
            &dwBytes,
            NULL,
            NULL)) {
        printf_w("WSAIoctl cannot get GuidGetAcceptExSockAddrs func pointer. error:%d\n", WSAGetLastError());
        this->_DeInitialize();
        return false;
    }


    // 为AcceptEx 准备参数，然后投递AcceptEx I/O请求
    for (int i = 0; i < MAX_POST_ACCEPT; i++) {
        // 新建一个IO_CONTEXT
        PER_IO_CONTEXT *pAcceptIoContext = m_pListenContext->GetNewIoContext();

        if (!this->_PostAccept(pAcceptIoContext)) {
            m_pListenContext->RemoveContext(pAcceptIoContext);
            return false;
        }
    }

    printf_w("post %d AcceptEx request ok\n", MAX_POST_ACCEPT);

    return true;
}

////////////////////////////////////////////////////////////
//	最后释放掉所有资源
void IOCPModel::_DeInitialize() {
    // 删除客户端列表的互斥量
    DeleteCriticalSection(&m_csContextList);

    // 关闭系统退出事件句柄
    RELEASE_HANDLE(m_hShutdownEvent);

    // 释放工作者线程句柄指针
    for (int i = 0; i < m_nThreads; i++) {
        RELEASE_HANDLE(m_phWorkerThreads[i]);
    }

    RELEASE(m_phWorkerThreads);

    // 关闭IOCP句柄
    RELEASE_HANDLE(m_hIOCompletionPort);

    // 关闭监听Socket
    RELEASE(m_pListenContext);

    printf_w("resource released.\n");
}


//====================================================================================
//
//				    投递完成端口请求
//
//====================================================================================


//////////////////////////////////////////////////////////////////
// 投递Accept请求
bool IOCPModel::_PostAccept(PER_IO_CONTEXT *pAcceptIoContext) {
    if (INVALID_SOCKET == m_pListenContext->m_Socket) {
        printf_w("Internal Error: %d\n", WSAGetLastError());
        return false;
    }

    // 准备参数
    DWORD dwBytes = 0;
    pAcceptIoContext->m_OpType = ACCEPT_POSTED;
    WSABUF *p_wbuf = &pAcceptIoContext->m_wsaBuf;
    OVERLAPPED *p_ol = &pAcceptIoContext->m_Overlapped;

    // 为以后新连入的客户端先准备好Socket( 这个是与传统accept最大的区别 )
    pAcceptIoContext->m_sockAccept = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == pAcceptIoContext->m_sockAccept) {
        printf_w("create Accept Socket failed error code: %d\n", WSAGetLastError());
        return false;
    }

    // 投递AcceptEx
    if (FALSE == m_lpfnAcceptEx(m_pListenContext->m_Socket, pAcceptIoContext->m_sockAccept, p_wbuf->buf,
                                p_wbuf->len - ((sizeof(SOCKADDR_IN) + 16) * 2),
                                sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &dwBytes, p_ol)) {
        if (WSA_IO_PENDING != WSAGetLastError()) {
            printf_w("POST AcceptEx request failed, error code: %d\n", WSAGetLastError());
            return false;
        }
    }

    return true;
}

////////////////////////////////////////////////////////////
// 在有客户端连入的时候，进行处理
// 流程有点复杂，你要是看不懂的话，就看配套的文档吧....
// 如果能理解这里的话，完成端口的机制你就消化了一大半了

// 总之你要知道，传入的是ListenSocket的Context，我们需要复制一份出来给新连入的Socket用
// 原来的Context还是要在上面继续投递下一个Accept请求
//
bool IOCPModel::_DoAccpet(PER_SOCKET_CONTEXT *pSocketContext, PER_IO_CONTEXT *pIoContext) {
    SOCKADDR_IN *ClientAddr = NULL;
    SOCKADDR_IN *LocalAddr = NULL;
    int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

    ///////////////////////////////////////////////////////////////////////////
    // 1. 首先取得连入客户端的地址信息
    // 这个 m_lpfnGetAcceptExSockAddrs 不得了啊~~~~~~
    // 不但可以取得客户端和本地端的地址信息，还能顺便取出客户端发来的第一组数据，老强大了...
    this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf,
                                     pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN) + 16) * 2),
                                     sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, (LPSOCKADDR *) &LocalAddr,
                                     &localLen, (LPSOCKADDR *) &ClientAddr, &remoteLen);

    printf_w("client %s:%d connection.\n", inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port));
    //printf_w( "client %s:%d infomation: %s.\n",inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port),pIoContext->m_wsaBuf.buf );
    auto nConRet = m_pIOCPCallback->connect_cb(ClientAddr->sin_addr.S_un.S_addr, ClientAddr->sin_port);
    auto nRecRet = m_pIOCPCallback->recv_cb(ClientAddr->sin_addr.S_un.S_addr, ClientAddr->sin_port,
                                            pIoContext->m_wsaBuf.buf);
    //////////////////////////////////////////////////////////////////////////////////////////////////////
    // 2. 这里需要注意，这里传入的这个是ListenSocket上的Context，这个Context我们还需要用于监听下一个连接
    // 所以我还得要将ListenSocket上的Context复制出来一份为新连入的Socket新建一个SocketContext

    PER_SOCKET_CONTEXT *pNewSocketContext = new PER_SOCKET_CONTEXT;
    pNewSocketContext->m_Socket = pIoContext->m_sockAccept;
    memcpy(&(pNewSocketContext->m_ClientAddr), ClientAddr, sizeof(SOCKADDR_IN));

    // 参数设置完毕，将这个Socket和完成端口绑定(这也是一个关键步骤)
    if (!this->_AssociateWithIOCP(pNewSocketContext)) {
        RELEASE(pNewSocketContext);
        return false;
    }


    ///////////////////////////////////////////////////////////////////////////////////////////////////
    // 3. 继续，建立其下的IoContext，用于在这个Socket上投递第一个Recv数据请求
    PER_IO_CONTEXT *pNewIoContext = pNewSocketContext->GetNewIoContext();


    pNewIoContext->m_sockAccept = pNewSocketContext->m_Socket;
    // 如果Buffer需要保留，就自己拷贝一份出来
    //memcpy( pNewIoContext->m_szBuffer,pIoContext->m_szBuffer,MAX_BUFFER_LEN );

    // 绑定完毕之后，就可以开始在这个Socket上投递完成请求了
    if (nRecRet == 0) {
        //准备发送数据
        if (!this->_PostSend(pNewSocketContext, pNewIoContext)) {
            pNewSocketContext->RemoveContext(pNewIoContext);
            return false;
        }
    }
    else {
        //继续接受数据
        if (!this->_PostRecv(pNewIoContext)) {
            pNewSocketContext->RemoveContext(pNewIoContext);
            return false;
        }
    }


    /////////////////////////////////////////////////////////////////////////////////////////////////
    // 4. 如果投递成功，那么就把这个有效的客户端信息，加入到ContextList中去(需要统一管理，方便释放资源)
    this->_AddToContextList(pNewSocketContext);

    ////////////////////////////////////////////////////////////////////////////////////////////////
    // 5. 使用完毕之后，把Listen Socket的那个IoContext重置，然后准备投递新的AcceptEx
    pIoContext->ResetBuffer();
    return this->_PostAccept(pIoContext);
}

////////////////////////////////////////////////////////////////////
// 投递接收数据请求
bool IOCPModel::_PostRecv(PER_IO_CONTEXT *pIoContext) {
    printf_w("DEBUG: PostRecv\n");
    // 初始化变量
    DWORD dwFlags = 0;
    DWORD dwBytes = 0;
    WSABUF *p_wbuf = &pIoContext->m_wsaBuf;
    OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

    pIoContext->ResetBuffer();
    pIoContext->m_OpType = RECV_POSTED;

    // 初始化完成后，，投递WSARecv请求
    int nBytesRecv = WSARecv(pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL);

    // 如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
    if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError())) {
        printf_w("POST WSARecv failed\n");
        return false;
    }
    return true;
}

////////////////////////////////////////////////////////////////////
// 投递发送数据请求
bool IOCPModel::_PostSend(PER_SOCKET_CONTEXT *pSocketContext, PER_IO_CONTEXT *pIoContext) {
    // 初始化变量
    SOCKADDR_IN *ClientAddr = &pSocketContext->m_ClientAddr;
    printf_w("DEBUG: PostSend\n");
    DWORD dwFlags = 0;
    DWORD dwBytes = 0;
    WSABUF *p_wbuf = &pIoContext->m_wsaBuf;
    OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

    pIoContext->ResetBuffer();
    pIoContext->m_OpType = SEND_POSTED;
    pIoContext->m_isContinue = (char) m_pIOCPCallback->send_cb(ClientAddr->sin_addr.S_un.S_addr, ClientAddr->sin_port,
                                                               pIoContext->m_wsaBuf.buf);
    //pIoContext->m_isContinue = (char) m_pIOCPCallback->send_cb(ClientAddr->sin_addr.S_un.S_addr,ClientAddr->sin_port,pIoContext->m_wsaBuf.buf);

    // 初始化完成后，，投递WSASend请求
    int nBytesRecv = WSASend(pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, dwFlags, p_ol, NULL);

    // 如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
    if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError())) {
        printf_w("POST WSASend failed\n");
        return false;
    }
    return true;
}


/////////////////////////////////////////////////////////////////
// 在有接收的数据到达的时候，进行处理
bool IOCPModel::_DoRecv(PER_SOCKET_CONTEXT *pSocketContext, PER_IO_CONTEXT *pIoContext) {
    printf_w("DEBUG: DoRecv\n");
    SOCKADDR_IN *ClientAddr = &pSocketContext->m_ClientAddr;
    auto tmp = m_pIOCPCallback->recv_cb(ClientAddr->sin_addr.S_un.S_addr, ClientAddr->sin_port,
                                        pIoContext->m_wsaBuf.buf);
    // 然后开始投递下一个WSARecv请求
    if (tmp == 0) {
        //准备发送数据
        return _PostSend(pSocketContext, pIoContext);
    }
    else {
        //继续接受数据
        return _PostRecv(pIoContext);
    }
}

/////////////////////////////////////////////////////////////////
// 准备发送数据的时候，进行处理
bool IOCPModel::_DoSend(PER_SOCKET_CONTEXT *pSocketContext, PER_IO_CONTEXT *pIoContext) {
    printf_w("DEBUG: DoSend\n");
    if (pIoContext->m_isFinal) {
        _RemoveContext(pSocketContext);
    }
    else {
        if (pIoContext->m_isContinue) {
            return _PostSend(pSocketContext, pIoContext);
        }
        else {
            return _PostRecv(pIoContext);
        }
    }
}


/////////////////////////////////////////////////////
// 将句柄(Socket)绑定到完成端口中
bool IOCPModel::_AssociateWithIOCP(PER_SOCKET_CONTEXT *pContext) {
    // 将用于和客户端通信的SOCKET绑定到完成端口中
    HANDLE hTemp = CreateIoCompletionPort((HANDLE) pContext->m_Socket, m_hIOCompletionPort, (ULONG_PTR) pContext, 0);

    if (NULL == hTemp) {
        printf_w(("CreateIoCompletionPort() error: %d\n"), (int) GetLastError());
        return false;
    }

    return true;
}


//////////////////////////////////////////////////////////////
// 将客户端的相关信息存储到数组中
void IOCPModel::_AddToContextList(PER_SOCKET_CONTEXT *pHandleData) {
    EnterCriticalSection(&m_csContextList);

    m_arrayClientContext.emplace_back(pHandleData);

    LeaveCriticalSection(&m_csContextList);
}

////////////////////////////////////////////////////////////////
//	移除某个特定的Context
void IOCPModel::_RemoveContext(PER_SOCKET_CONTEXT *pSocketContext) {
    EnterCriticalSection(&m_csContextList);

    for (int i = 0; i < m_arrayClientContext.size(); i++) {
        if (pSocketContext == m_arrayClientContext[i]) {
            RELEASE(pSocketContext);
            m_arrayClientContext.erase(m_arrayClientContext.begin() + i);
            break;
        }
    }

    LeaveCriticalSection(&m_csContextList);
}

////////////////////////////////////////////////////////////////
// 清空客户端信息
void IOCPModel::_ClearContextList() {
    EnterCriticalSection(&m_csContextList);

    for (int i = 0; i < m_arrayClientContext.size(); i++) {
        delete m_arrayClientContext[i];
    }

    m_arrayClientContext.clear();

    LeaveCriticalSection(&m_csContextList);
}


///////////////////////////////////////////////////////////////////
// 获得本机中处理器的数量
int IOCPModel::_GetNoOfProcessors() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int) si.dwNumberOfProcessors;
}



/////////////////////////////////////////////////////////////////////
// 判断客户端Socket是否已经断开，否则在一个无效的Socket上投递WSARecv操作会出现异常
// 使用的方法是尝试向这个socket发送数据，判断这个socket调用的返回值
// 因为如果客户端网络异常断开(例如客户端崩溃或者拔掉网线等)的时候，服务器端是无法收到客户端断开的通知的

bool IOCPModel::_IsSocketAlive(SOCKET s) {
    int nByteSent = send(s, "", 0, 0);
    return -1 == nByteSent;
}

///////////////////////////////////////////////////////////////////
// 显示并处理完成端口上的错误
bool IOCPModel::HandleError(PER_SOCKET_CONTEXT *pContext, const DWORD &dwErr) {
    // 如果是超时了，就再继续等吧
    if (WAIT_TIMEOUT == dwErr) {
        // 确认客户端是否还活着...
        if (!_IsSocketAlive(pContext->m_Socket)) {
            printf_w("client quit\n ");
            this->_RemoveContext(pContext);
            return true;
        }
        else {
            printf_w("Network error\n");
            return true;
        }
    }

        // 可能是客户端异常退出了
    else
        if (ERROR_NETNAME_DELETED == dwErr) {
            printf_w("detected iocp exception\n");
            this->_RemoveContext(pContext);
            return true;
        }
        else {
            printf_w("IOCP ERROR, thread error,Error code : %d\n", dwErr);
            return false;
        }
}




