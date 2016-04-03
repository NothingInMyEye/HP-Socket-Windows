#include "CworkerThread.h"
#include<process.h>

/*
* CThread.cc
*
*  Created on: Mar 4, 2013
*      Author: yaowei
*/

#include "global_settings.h"
#include "utils.h"
//#include "socket_wrapper.h"

int CWorkerThread::init_count_ = 0;
//pthread_mutex_t	CWorkerThread::init_lock_ = PTHREAD_MUTEX_INITIALIZER;
//pthread_cond_t  CWorkerThread::init_cond_ = PTHREAD_COND_INITIALIZER;
HANDLE CWorkerThread::h_InitFinish = CreateEvent(NULL, TRUE, FALSE, NULL);

int CWorkerThread::freetotal_ = 0;
int CWorkerThread::freecurr_ = 0;
//boost::mutex CWorkerThread::mutex_;
std::mutex CWorkerThread::mutex_;
std::vector<CONN*> CWorkerThread::vec_freeconn_;


CWorkerThread::CWorkerThread()
{
	last_thread_ = -1;
}

CWorkerThread::~CWorkerThread()
{
}

/* ��ʼ��worker�̳߳� */
bool CWorkerThread::InitThreads(struct event_base* main_base)
{

	InitFreeConns();

	////LOG4CXX_INFO(g_logger, "Initializes worker threads...");
	
	for (unsigned int i = 0; i<utils::G<CGlobalSettings>().thread_num_; ++i)
	{
		LIBEVENT_THREAD* libevent_thread_ptr = new LIBEVENT_THREAD;
		/* ����ÿ��worker�̺߳��������߳�ͨ�ŵĹܵ� */
		//int fds[2];
		
		if (CreatePipe(&libevent_thread_ptr->notify_receive_fd, &libevent_thread_ptr->notify_send_fd, NULL, 0) != TRUE)
		{
			////LOG4CXX_ERROR(g_logger, "CThread::InitThreads:Can't create notify pipe");
			return false;
		}

		//��ʼ����������Ϊ0
		libevent_thread_ptr->msg_count = 0;

	/*	libevent_thread_ptr->notify_receive_fd = (evutil_socket_t)fds[0];
		libevent_thread_ptr->notify_send_fd = (evutil_socket_t)fds[1];*/

		if (!SetupThread(libevent_thread_ptr))
		{
			utils::SafeDelete(libevent_thread_ptr);
			g_pLogger->Error("CThread::InitThreads:SetupThread failed.");
			////LOG4CXX_ERROR(g_logger, "CThread::InitThreads:SetupThread failed.");
			return false;
		}

		vec_libevent_thread_.push_back(libevent_thread_ptr);
	}

	for (unsigned int i = 0; i < utils::G<CGlobalSettings>().thread_num_; i++)
	{
		CreateWorker(WorkerLibevent, vec_libevent_thread_.at(i));
	}

	/* �ȴ������̶߳��Ѿ��������. */
	WaitForThreadRegistration(utils::G<CGlobalSettings>().thread_num_);
	g_pLogger->Info("Create threads success. we hava done all the libevent setup.");
	//LOG4CXX_INFO(g_logger, "Create threads success. we hava done all the libevent setup.");

	return true;
}

void CWorkerThread::CreateWorker(void (*func)(void *), void *arg)
{
	//pthread_t thread;
	//pthread_attr_t attr;
	//int ret;

	//pthread_attr_init(&attr);

	if (_beginthread(func, 0, arg) == -1)
	{
		g_pLogger->Fatal("CWorkerThread::CreateWorker:Can't create thread.");
		//LOG4CXX_FATAL(g_logger, "CWorkerThread::CreateWorker:Can't create thread:" << strerror(ret));
		exit(1);
	}
}

void CWorkerThread::TimeOutCb(evutil_socket_t none_use, short flags, void * parm)
{
	LIBEVENT_THREAD *libevent_thread_ptr = static_cast<LIBEVENT_THREAD*>(parm);
	assert(libevent_thread_ptr != NULL);

	while (libevent_thread_ptr->msg_count)
	{
		libevent_thread_ptr->msg_count--;
		DWORD read_bytes = 0;
		/* read from master-thread had write, a byte ����һ���ͻ������� */
		char buf[1];
		if (ReadFile(libevent_thread_ptr->notify_receive_fd, buf, 1, &read_bytes, NULL) != TRUE)
		{
			////LOG4CXX_ERROR(g_logger, "CWorkerThread::ThreadLibeventProcess:Can't read from libevent pipe.");
			return;
		}
		else if (read_bytes != 1)
		{
			return;
		}

		/* �����߳����������е�����pop���� */
		CONN_INFO connInfo;
		if (!libevent_thread_ptr->list_conn.pop_front(connInfo))
		{
			////LOG4CXX_ERROR(g_logger, "CWorkerThread::ThreadLibeventProcess:list_conn.pop_front NULL.");
			return;
		}

		/*��ʼ�������ӣ��������¼�ע����libevent */
		if (connInfo.sfd != 0)
		{
			CONN* conn = InitNewConn(connInfo, libevent_thread_ptr);
			if (NULL == conn)
			{
				////LOG4CXX_ERROR(g_logger, "CWorkerThread::ReadPipeCb:Can't listen for events on sfd = " << connInfo.sfd);
				CloseHandle((HANDLE)connInfo.sfd);
			}
			//LOG4CXX_TRACE(g_logger, "CWorkerThread::ReadPipeCb thread id = " << conn->thread->thread_id);
		}
	}	

	//std::cout << "call " << std::endl;
}

void CWorkerThread::WorkerLibevent(void *arg)
{
	LIBEVENT_THREAD *me = static_cast<LIBEVENT_THREAD *>(arg);

	me->thread_id = GetCurrentThreadId();

	RegisterThreadInitialized();

	int ret = 0;

	ret = event_base_dispatch(me->base);

	//cout << "loop out" <<"" << i <<  endl;
}


bool CWorkerThread::SetupThread(LIBEVENT_THREAD* me)
{
	me->base = event_base_new();
	assert(me != NULL);

	/* ͨ��ÿ��worker�̵߳Ķ��ܵ���������master��֪ͨ */
	//me->notify_event = event_new(me->base, me->notify_send_fd, EV_READ | EV_PERSIST, ReadPipeCb, (void*)me);
	//assert(&me->notify_event != NULL);

	struct timeval tm;
	tm.tv_sec = 1;
	tm.tv_usec = 0;
	me->timeout_event = evtimer_new(me->base, TimeOutCb, me);
	me->timeout_event = event_new(me->base, -1, EV_PERSIST, TimeOutCb, me);
	int i = evtimer_add(me->timeout_event, &tm);
	//if (event_add(me->notify_event, NULL) == -1)
	//{
	//	int error_code = EVUTIL_SOCKET_ERROR();
	//	////LOG4CXX_ERROR(g_logger, "CWorkerThread::SetupThread:event_add errorCode = " << error_code
	//	//	<< ", description = " << evutil_socket_error_to_string(error_code));
	//	return false;
	//}

	return true;
}

//void CWorkerThread::ReadPipeCb(int fd, short event, void* arg)
//{
//
//	LIBEVENT_THREAD *libevent_thread_ptr = static_cast<LIBEVENT_THREAD*>(arg);
//	assert(libevent_thread_ptr != NULL);
//
//	HANDLE fdRead = (HANDLE)fd;
//	DWORD read_bytes = 0;
//	/* read from master-thread had write, a byte ����һ���ͻ������� */
//	char buf[1];
//	if (ReadFile(fdRead, buf, 1, &read_bytes, NULL) != TRUE)
//	{
//		////LOG4CXX_ERROR(g_logger, "CWorkerThread::ThreadLibeventProcess:Can't read from libevent pipe.");
//		return;
//	}
//	else if (read_bytes != 1)
//	{
//		return;
//	}
//
//	/* �����߳����������е�����pop���� */
//	CONN_INFO connInfo;
//	if (!libevent_thread_ptr->list_conn.pop_front(connInfo))
//	{
//		////LOG4CXX_ERROR(g_logger, "CWorkerThread::ThreadLibeventProcess:list_conn.pop_front NULL.");
//		return;
//	}
//
//	/*��ʼ�������ӣ��������¼�ע����libevent */
//	if (connInfo.sfd != 0)
//	{
//		CONN* conn = InitNewConn(connInfo, libevent_thread_ptr);
//		if (NULL == conn)
//		{
//			////LOG4CXX_ERROR(g_logger, "CWorkerThread::ReadPipeCb:Can't listen for events on sfd = " << connInfo.sfd);
//			CloseHandle((HANDLE)connInfo.sfd);
//		}
//		//LOG4CXX_TRACE(g_logger, "CWorkerThread::ReadPipeCb thread id = " << conn->thread->thread_id);
//	}
//}

CONN* CWorkerThread::InitNewConn(const CONN_INFO& conn_info, LIBEVENT_THREAD* libevent_thread_ptr)
{
	CONN* conn = GetConnFromFreelist();
	if (NULL == conn)
	{
		conn = new CONN;
		if (NULL == conn)
		{
			////LOG4CXX_ERROR(g_logger, "CWorkerThread::InitNewConn:new conn error.");
			return NULL;
		}

		try
		{
			conn->rBuf = new char[DATA_BUFFER_SIZE];
			conn->wBuf = new char[DATA_BUFFER_SIZE];
		}
		catch (std::bad_alloc &)
		{
			FreeConn(conn);
			////LOG4CXX_ERROR(g_logger, "CWorkerThread::InitNewConn:new buf error.");
			return NULL;
		}
	}

	conn->sfd = conn_info.sfd;
	conn->rlen = 0;
	conn->wlen = 0;
	conn->thread = libevent_thread_ptr;

	/* �������Ӽ�����߳�libevent�¼�ѭ�� */
	int flag = EV_READ | EV_PERSIST;
	struct bufferevent *client_tcp_event = bufferevent_socket_new(libevent_thread_ptr->base, conn->sfd, BEV_OPT_CLOSE_ON_FREE);
	if (NULL == client_tcp_event)
	{
		if (!AddConnToFreelist(conn))
		{
			FreeConn(conn);
		}
		int error_code = EVUTIL_SOCKET_ERROR();
		////LOG4CXX_ERROR(g_logger,
		//	"CWorkerThread::conn_new:bufferevent_socket_new errorCode = " << error_code << ", description = " << evutil_socket_error_to_string(error_code));

		return NULL;
	}
	bufferevent_setcb(client_tcp_event, ClientTcpReadCb, NULL, ClientTcpErrorCb, (void*)conn);

	/* ���ÿͻ���������ʱ���ƴ���뿪���� */
	struct timeval heartbeat_sec;
	heartbeat_sec.tv_sec = utils::G<CGlobalSettings>().client_heartbeat_timeout_;
	heartbeat_sec.tv_usec = 0;
	bufferevent_set_timeouts(client_tcp_event, &heartbeat_sec, NULL);

	bufferevent_enable(client_tcp_event, flag);

	return conn;
}


void CWorkerThread::ClientTcpReadCb(struct bufferevent *bev, void *arg)
{
	CONN* conn = static_cast<CONN*>(arg);
	assert(conn != NULL);

	int recv_size = 0;
	if ((recv_size = bufferevent_read(bev, conn->rBuf + conn->rlen, DATA_BUFFER_SIZE - conn->rlen)) > 0)
	{
	//	conn->rlen = conn->rlen + recv_size;
	//	//��ֹ�������ӣ�����tokenУ�飬������У��������Ϊ�������ӣ�ֱ�ӹر�
	//	if (conn->rlen >= TOKEN_LENGTH && conn->isVerify == false)
	//	{
	//		conn->isVerify = true;
	//		std::string str_verify(conn->rBuf, TOKEN_LENGTH);
	//		if (str_verify.compare(std::string(TOKEN_STR)) != 0)
	//		{
	//			//LOG4CXX_WARN(g_logger, "CWorkerThread::ClientTcpReadCb DDOS. str = " << str_verify);
	//			CloseConn(conn, bev);
	//			return;
	//		}
	//		else
	//		{
	//			conn->rlen = conn->rlen - TOKEN_LENGTH;
	//			memmove(conn->rBuf, conn->rBuf + TOKEN_LENGTH, conn->rlen);
	//		}
	//	}
	}

	std::string str_recv(conn->rBuf, conn->rlen);
	//if (utils::FindCRLF(str_recv))
	//{
	//	/* �п���ͬʱ�յ�������Ϣ */
	//	std::vector<std::string> vec_str;
	//	utils::SplitData(str_recv, CRLF, vec_str);

	//	for (unsigned int i = 0; i < vec_str.size(); ++i)
	//	{
	//		if (!SocketOperate::WriteSfd(conn->sfd, vec_str.at(i).c_str(), vec_str.at(i).length()))
	//		{
	//			////LOG4CXX_ERROR(g_logger, "CWorkerThread::ClientTcpReadCb:send sfd .error = " << strerror(errno));
	//		}
	//	}

	//	int len = str_recv.find_last_of(CRLF) + 1;
	//	memmove(conn->rBuf, conn->rBuf + len, DATA_BUFFER_SIZE - len);
	//	conn->rlen = conn->rlen - len;
	//}
}

void CWorkerThread::ClientTcpErrorCb(struct bufferevent *bev, short event, void *arg)
{
	CONN* conn = static_cast<CONN*>(arg);

	if (event & BEV_EVENT_TIMEOUT)
	{
		//LOG4CXX_WARN(g_logger, "CWorkerThread::ClientTcpErrorCb:TimeOut.");
	}
	else if (event & BEV_EVENT_EOF)
	{
	}
	else if (event & BEV_EVENT_ERROR)
	{
		int error_code = EVUTIL_SOCKET_ERROR();
		//LOG4CXX_WARN(g_logger,
		//	"CWorkerThread::ClientTcpErrorCb:some other errorCode = " << error_code << ", description = " << evutil_socket_error_to_string(error_code));
	}

	CloseConn(conn, bev);
}

void CWorkerThread::DispatchSfdToWorker(int sfd)
{
	/* Round Robin*/
	int tid = (last_thread_ + 1) % utils::G<CGlobalSettings>().thread_num_;
	LIBEVENT_THREAD *libevent_thread_ptr = vec_libevent_thread_.at(tid);
	last_thread_ = tid;

	/* �������ӵļ����worker�߳����Ӷ��� */
	CONN_INFO connInfo;
	connInfo.sfd = sfd;
	libevent_thread_ptr->list_conn.push_back(connInfo);

	/* ֪ͨ��worker�߳��������ӵ��������Զ�ȡ�� */
	char buf[1];
	buf[0] = 'c';
	DWORD write_bytes = 0;
	if (WriteFile(libevent_thread_ptr->notify_send_fd, buf, 1, &write_bytes,NULL))
	{
		//LOG4CXX_WARN(g_logger, "CWorkerThread::DispatchSfdToWorker:Writing to thread notify pipe");
		libevent_thread_ptr->msg_count++;
		cout << "Writing to thread notify pipe! " << endl;
	}
	else if (write_bytes != 1)
	{
		cout << "Write Pipe Failed! " << endl;
	}
}

void CWorkerThread::RegisterThreadInitialized(void)
{
	mutex_.lock();
	init_count_++;
	mutex_.unlock();
	if (init_count_ == int(utils::G<CGlobalSettings>().thread_num_))
	{
		SetEvent(h_InitFinish);
	}
	
}

void CWorkerThread::WaitForThreadRegistration(int nthreads)
{
	WaitForSingleObject(h_InitFinish, INFINITE);
}

void CWorkerThread::InitFreeConns()
{
	freetotal_ = 200;
	freecurr_ = 0;

	vec_freeconn_.resize(freetotal_);
}

CONN* CWorkerThread::GetConnFromFreelist()
{
	CONN *conn = NULL;

	mutex_.lock();
	if (freecurr_ > 0)
	{
		conn = vec_freeconn_.at(--freecurr_);
	}
	mutex_.unlock();

	return conn;
}

bool CWorkerThread::AddConnToFreelist(CONN* conn)
{
	bool ret = false;
	mutex_.lock();
	if (freecurr_ < freetotal_)
	{
		vec_freeconn_.at(freecurr_++) = conn;
		ret = true;
	}
	else
	{
		/* ���������ڴ�ض��� */
		size_t newsize = freetotal_ * 2;
		vec_freeconn_.resize(newsize);
		freetotal_ = newsize;
		vec_freeconn_.at(freecurr_++) = conn;
		ret = true;
	}
	mutex_.unlock();

	return ret;
}

void CWorkerThread::FreeConn(CONN* conn)
{
	if (conn)
	{
		//utils::SafeDelete(conn->thread->notify_event);
		utils::SafeDeleteArray(conn->rBuf);
		utils::SafeDeleteArray(conn->wBuf);
		utils::SafeDelete(conn);
	}
}

void CWorkerThread::CloseConn(CONN* conn, struct bufferevent* bev)
{
	assert(conn != NULL);

	/* ������Դ��the event, the socket and the conn */
	bufferevent_free(bev);

	//LOG4CXX_TRACE(g_logger, "CWorkerThread::conn_close sfd = " << conn->sfd);

	/* if the connection has big buffers, just free it */
	if (!AddConnToFreelist(conn))
	{
		FreeConn(conn);
	}

	return;
}
