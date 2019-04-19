#include "Kcp.h"
#include "Service.h"

#include <asio/asio.hpp>
#include <micro-ecc/uECC.h>
#include "Log.h"
#include "Message.h"
#include "Clock.h"
#include <map>
#include <asio/yield.hpp>
#include <kcp/ikcp.h>
#include <random>

extern "C"
{
	extern void *sha3(const void *in, size_t inlen, void *md, int mdlen);
}

namespace nicehero
{
	KcpMessageParser& getKcpMessagerParse(const std::type_info& typeInfo)
	{
		static std::map<const std::type_info*, KcpMessageParser> gKcpMessageParse;
		return gKcpMessageParse[&typeInfo];
	}
	class KcpSessionImpl
	{
	public:
		KcpSessionImpl(KcpSession& session)
			:m_session(session)
		{
			m_createTime = Clock::getInstance()->getTime();
			static thread_local std::default_random_engine e;
			m_iocontextIndex = e() % WORK_THREAD_COUNT;
		}
		~KcpSessionImpl()
		{
			if (m_kcp)
			{
				ikcp_release(m_kcp);
				m_kcp = nullptr;
			}
		}
		bool kcpUpdate()
		{
			ui64 milliSec = Clock::getInstance()->getMilliSeconds();
			if (milliSec >= m_kcpUpdateTime)
			{
				IUINT32 current = (IUINT32)milliSec;
				ikcp_update(m_kcp, current);
				IUINT32 next = ikcp_check(m_kcp, current);
				m_kcpUpdateTime = milliSec + (next - current);
				return true;
			}
			return false;
		}
		int kcpRecv(ikcpcb *kcp, unsigned char *buffer, int len)
		{
			return ikcp_recv(m_kcp, (char*)buffer, len);
		}
		static int udpOutput(const char *buf, int len, ikcpcb *kcp, void *user);
		std::shared_ptr<asio::ip::udp::socket> m_socket;
		asio::ip::udp::endpoint m_endpoint;
		KcpSession& m_session;
		ikcpcb* m_kcp = nullptr;
		ui32 m_iocontextIndex = 0;
		ui64 m_kcpUpdateTime = 0;
		ui64 m_createTime;
		bool m_inited = false;
		asio::io_context& getIoContext()
		{
			return nicehero::gWorkerServices[m_iocontextIndex];
		}
	};

	class KcpServerImpl
	{
public:
		KcpServerImpl(asio::ip::address ip,ui16 port,KcpServer& server_)
			:m_server(server_), m_ip(ip), m_port(port)
		{
			m_socket = std::make_shared<asio::ip::udp::socket>(gMultiWorkerService, asio::ip::basic_endpoint<asio::ip::udp>(m_ip, m_port));
		}
		~KcpServerImpl()
		{
			nlogerr("~KcpServerImpl()");
		}
		void accept()
		{
			for (size_t i = 0; i < WORK_THREAD_COUNT; ++i)
			{
				std::shared_ptr<std::string> buffer(new std::string(NETWORK_BUF_SIZE,0));
				std::shared_ptr<asio::ip::udp::endpoint> senderEndpoint(new asio::ip::udp::endpoint());
				startReceive(buffer, senderEndpoint,m_socket);
 				startRoutine(i);
			}
		}
		void startRoutine(size_t workerIndex)
		{
			std::shared_ptr<asio::steady_timer> t = std::make_shared<asio::steady_timer>(nicehero::gWorkerServices[workerIndex]);
			t->expires_from_now(std::chrono::milliseconds(1));
			t->async_wait([this,workerIndex,t](std::error_code ec) {
				if (ec)
				{
					nlog("KcpServerImpl::startRoutine error %s",ec.message().c_str());
					return;
				}

				for (auto it:m_RunningSessions[workerIndex])
				{
					it.second->doRead();
				}
				
				startRoutine(workerIndex);
			});
		}
		bool checkSign(const ui8* data_,size_t len,const std::string& hash_)
		{
			bool allSame = true;
			for (size_t i = 0; i < PUBLIC_KEY_SIZE; ++i)
			{
				if (m_server.m_publicKey[i] != data_[i])
				{
					allSame = false;
					break;
				}
			}
			if (allSame)
			{
				return false;
			}
			if (uECC_verify((const ui8*)data_, (const ui8*)hash_.c_str(), HASH_SIZE, data_ + PUBLIC_KEY_SIZE, uECC_secp256k1()) != 1)
			{
				return false;
			}
			return true;
		}

		void startReceive(std::shared_ptr<std::string> buffer
			, std::shared_ptr<asio::ip::udp::endpoint> senderEndpoint
			, std::shared_ptr<asio::ip::udp::socket> s)
		{
			s->async_receive_from(asio::buffer(const_cast<char *>(buffer->c_str()), buffer->length())
				, *senderEndpoint,
				[=](asio::error_code ec, std::size_t bytesRecvd)
			{
				if (buffer->c_str()[0] == 1 && bytesRecvd == 1)
				{
					std::shared_ptr<KcpSession> ks = std::shared_ptr<KcpSession>(m_server.createSession());
					KcpSessionS* ss = dynamic_cast<KcpSessionS*>(ks.get());
					if (ss)
					{
						ss->m_KcpServer = &m_server;
						ss->m_MessageParser = &getKcpMessagerParse(typeid(*ss));
					}
					ks->m_impl->m_endpoint = *senderEndpoint;
					ks->m_impl->m_socket = m_socket;
					ss->init(m_server);
				}
				else if (buffer->c_str()[0] == 2)
				{
					m_PreSessionsLock.lock();
					std::shared_ptr<KcpSession> ks = std::shared_ptr<KcpSession>(m_PreSessions[*senderEndpoint]);
					m_PreSessions.erase(*senderEndpoint);
					m_PreSessionsLock.unlock();
					if (bytesRecvd >= PUBLIC_KEY_SIZE + SIGN_SIZE + 1 && ks)
					{
						KcpSessionS* ss = dynamic_cast<KcpSessionS*>(ks.get());
						if (ss)
						{
							if (checkSign((const ui8*)(buffer->c_str() + 1),bytesRecvd,ss->m_hash))
							{
								ss->m_uid = std::string((const char*)(buffer->c_str() + 1), PUBLIC_KEY_SIZE);
								ss->init3(m_server);
							}
						}
					}
				}
				else if (buffer->c_str()[0] == 3 && bytesRecvd >= IKCP_OVERHEAD)
				{
					ui16 workerIndex = *(ui16*)(buffer->c_str() + 1);
					if ((ui32)workerIndex < WORK_THREAD_COUNT)
					{
						std::string uid = std::string((const char*)(buffer->c_str() + 3), PUBLIC_KEY_SIZE);
						std::string data_ = std::string((const char*)(buffer->c_str() + 3 + PUBLIC_KEY_SIZE), bytesRecvd - 3 - PUBLIC_KEY_SIZE);
						nicehero::gWorkerServices[workerIndex].post([&,uid, data_] {
							auto it = m_RunningSessions[workerIndex].find(uid);
							if (it != m_RunningSessions[workerIndex].end() && it->second
								&& it->second->m_impl->m_kcp)
							{
								ikcp_input(it->second->m_impl->m_kcp, data_.c_str(), (long)(bytesRecvd - 3 - PUBLIC_KEY_SIZE));
								it->second->m_impl->m_kcpUpdateTime = Clock::getInstance()->getMilliSeconds();
							}
						});
					}
				}
				else if (buffer->c_str()[0] == 4 && bytesRecvd > 1 + PUBLIC_KEY_SIZE)
				{
					std::string uid = std::string((const char*)(buffer->c_str() + 3), PUBLIC_KEY_SIZE);
					ui32 len = *(ui32*)(buffer->c_str() + 1 + PUBLIC_KEY_SIZE);
					if (len < bytesRecvd - 1 - PUBLIC_KEY_SIZE)
					{
						return;
					}
					auto recvMsg = std::make_shared<Message>(buffer->c_str() + 1, len);
					nicehero::post([&,recvMsg,uid] {
						auto it = m_server.m_sessions.find(uid);
						if (it != m_server.m_sessions.end() && it->second)
						{
							it->second->handleMessage(recvMsg);
						}
					});

				}

				startReceive(buffer, senderEndpoint, s);
			});
		}
		KcpServer& m_server;
		std::shared_ptr<asio::ip::udp::socket> m_socket;
		asio::ip::address m_ip;
		ui16 m_port;
		std::map<asio::ip::udp::endpoint, std::shared_ptr<KcpSession> > m_PreSessions;
		std::mutex m_PreSessionsLock;
		std::unordered_map<kcpuid, std::shared_ptr<KcpSession> > m_RunningSessions[nicehero::WORK_THREAD_COUNT];
	};

	KcpServer::KcpServer(const std::string& ip, ui16 port)
	{
		asio::error_code ec;
		auto addr = asio::ip::address::from_string(ip, ec);
		if (ec)
		{
			nlogerr("KcpServer::KcpServer ip error:%s", ip.c_str());
		}
		try
		{
			m_impl = std::make_shared<KcpServerImpl>(addr, port,*this);
		}
		catch (asio::system_error & ec)
		{
			nlogerr("cannot open %s:%d", ip.c_str(), int(port));
			nlogerr("%s",ec.what());
		}
	}

	KcpServer::~KcpServer()
	{
		
	}

	KcpSessionS* KcpServer::createSession()
	{
		return new KcpSessionS();
	}

	void KcpServer::addSession(const kcpuid& uid, std::shared_ptr<KcpSession> session)
	{
		auto it = m_sessions.find(uid);
		if (it != m_sessions.end())
		{
			it->second->close();
		}
		m_sessions[uid] = session;
	}

	void KcpServer::removeSession(const kcpuid& uid, ui64 serialID)
	{
		auto it = m_sessions.find(uid);
		if (it != m_sessions.end() && it->second->m_serialID == serialID)
		{
			it->second->close();
			m_sessions.erase(uid);
		}
	}

	void KcpServer::accept()
	{
		m_impl->accept();
	}

	ui32 KcpServer::getFreeUid()
	{
		if (m_sessions.size() >= m_maxSessions)
		{
			return INVALID_CONV;
		}
		return 1;
		/*
		for (;;)
		{
			if (m_nextConv == INVALID_CONV)
			{
				++m_nextConv;
			}
			if (m_sessions.find(m_nextConv) == m_sessions.end())
			{
				return m_nextConv++;
			}
			++m_nextConv;
		}
		*/
	}

	KcpSessionS::KcpSessionS()
	{

	}

	KcpSessionS::~KcpSessionS()
	{

	}

	void KcpSessionS::init(KcpServer& server)
	{
		m_uid = "";
		m_conv = 1;
		init_kcp();
		auto self(shared_from_this());

		ui8 buff[PUBLIC_KEY_SIZE + 8 + HASH_SIZE + SIGN_SIZE] = {0};
		memcpy(buff, server.m_publicKey, PUBLIC_KEY_SIZE);
		ui64 now = nNow;
		*(ui64*)(buff + PUBLIC_KEY_SIZE) = now;
		sha3(buff, PUBLIC_KEY_SIZE + 8, buff + PUBLIC_KEY_SIZE + 8, HASH_SIZE);
		m_hash = std::string((const char*)(buff + PUBLIC_KEY_SIZE + 8),HASH_SIZE);
		if (uECC_sign(server.m_privateKey
			, buff + PUBLIC_KEY_SIZE + 8, HASH_SIZE
			, buff + PUBLIC_KEY_SIZE + 8 + HASH_SIZE
			, uECC_secp256k1()) != 1
			) 
		{
			nlogerr("uECC_sign() failed\n");
			return;
		}
// 		if (uECC_verify(buff, (const ui8*)(buff + PUBLIC_KEY_SIZE + 8), HASH_SIZE, buff + PUBLIC_KEY_SIZE + 8 + HASH_SIZE, uECC_secp256k1()) != 1)
// 		{
// 			nlogerr("error check hash2");
// 		}
		m_impl->m_socket->async_send_to(
			asio::buffer(buff, sizeof(buff)),
			m_impl->m_endpoint,
			[&,self](std::error_code ec,size_t s) {
			if (ec)
			{
				nlogerr("%d\n", ec.value());
				return;
			}
			init2(server);
		});
	}

	void KcpSessionS::init2(KcpServer& server)
	{
		auto self(shared_from_this());
		{
			std::lock_guard<std::mutex> g(server.m_impl->m_PreSessionsLock);
			server.m_impl->m_PreSessions[m_impl->m_endpoint] = self;
		}
		std::shared_ptr<asio::steady_timer> t = std::make_shared<asio::steady_timer>(m_impl->getIoContext());
		t->expires_from_now(std::chrono::seconds(2));
		t->async_wait([&,server,self](std::error_code ec) {
			if (!ec)
			{
				nlog("session connecting timeout");
			}
			{
				std::lock_guard<std::mutex> g(server.m_impl->m_PreSessionsLock);
				server.m_impl->m_PreSessions.erase(m_impl->m_endpoint);
			}
		});
	}


	void KcpSessionS::init3(KcpServer& server)
	{
		auto self(shared_from_this());
		nicehero::post([&, this, self] {
			server.addSession(m_uid, self);
			m_impl->getIoContext().post([&, self] {
				auto oldSession = server.m_impl->m_RunningSessions[m_impl->m_iocontextIndex][m_uid];
				if (oldSession)
				{
					char buffer[1] = "";
					oldSession->m_impl->m_socket->async_send_to(asio::buffer(buffer,1),
						oldSession->m_impl->m_endpoint
						, [] (asio::error_code, std::size_t) {
					});
				}
				server.m_impl->m_RunningSessions[m_impl->m_iocontextIndex][m_uid] = self;
			});
		});
	}

	void KcpSessionS::removeSelf()
	{
		auto self(shared_from_this());
		nicehero::post([&,self] {
			removeSelfImpl();
		});
	}

	void KcpSessionS::removeSelfImpl()
	{
		if (m_KcpServer)
		{
			m_KcpServer->removeSession(m_uid, m_serialID);
			auto self(shared_from_this());
			m_impl->getIoContext().post([&, self] {
				if (m_KcpServer)
				{
					m_KcpServer->m_impl->m_RunningSessions[m_impl->m_iocontextIndex].erase(m_uid);
				}
			});
		}
	}

	KcpSession::KcpSession()
	{
		m_impl = std::make_shared<KcpSessionImpl>(*this);
		m_IsSending = false;
	}

	void KcpSession::init(KcpServer& server)
	{

	}

	void KcpSession::init()
	{

	}

	void KcpSession::init2(KcpServer& server)
	{

	}

	void KcpSession::doRead()
	{
		if (!m_impl->kcpUpdate())
		{
			return;
		}
		unsigned char data_[NETWORK_BUF_SIZE];
		while (int len = m_impl->kcpRecv(m_impl->m_kcp, data_, NETWORK_BUF_SIZE) > 0)
		{
			if (!parseMsg(data_, len))
			{
				removeSelf();
				return;
			}
		}
	}

	bool KcpSession::parseMsg(unsigned char* data, ui32 len)
	{
		if (len > (ui32)NETWORK_BUF_SIZE)
		{
			return false;
		}
		Message& prevMsg = m_PreMsg;
		auto self(shared_from_this());
		if (prevMsg.m_buff == nullptr)
		{
			if (len < 4)
			{
// 				nlog("KcpSession::parseMsg len < 4");
				memcpy(&prevMsg.m_writePoint, data, len);
				prevMsg.m_buff = (unsigned char*)&prevMsg.m_writePoint;
				prevMsg.m_readPoint = len;
				return true;
			}
			ui32 msgLen = *((ui32*)data);
			if (msgLen > MSG_SIZE)
			{
				return false;
			}
			if (msgLen <= len)
			{
				auto recvMsg = std::make_shared<Message>(data, *((ui32*)data));
				
// 				if (m_MessageParser && m_MessageParser->m_commands[recvMsg->getMsgID()] == nullptr)
// 				{
// 					nlogerr("KcpSession::parseMsg err 1");
// 				}

				nicehero::post([self,recvMsg] {
					self->handleMessage(recvMsg);
				});
				if (msgLen < len)
				{
					return parseMsg( data + msgLen, len - msgLen);
				}
				else
				{
					return true;
				}
			}
			else
			{
				prevMsg.m_buff = new unsigned char[msgLen];
				memcpy(prevMsg.m_buff, data, len);
				prevMsg.m_writePoint = len;
				return true;
			}
		}
		ui32 msgLen = 0;
		ui32 cutSize = 0;
		if (prevMsg.m_buff == (unsigned char*)&prevMsg.m_writePoint)
		{
			if (prevMsg.m_readPoint + len < 4)
			{
// 				nlog("KcpSession::parseMsg prevMsg.m_readPoint + len < 4");
				memcpy(((unsigned char*)&prevMsg.m_writePoint) + prevMsg.m_readPoint
					, data, len);
				prevMsg.m_readPoint = prevMsg.m_readPoint + len;
				return true;
			}
			cutSize = 4 - prevMsg.m_readPoint;
			memcpy(((unsigned char*)&prevMsg.m_writePoint) + prevMsg.m_readPoint
				, data, cutSize);
			msgLen = prevMsg.m_writePoint;
			prevMsg.m_buff = new unsigned char[msgLen];
			memcpy(prevMsg.m_buff, &msgLen, 4);
			prevMsg.m_readPoint = 4;
			prevMsg.m_writePoint = 4;
		}
		msgLen = prevMsg.getSize();
		if (msgLen > MSG_SIZE)
		{
			return false;
		}
		if (len + prevMsg.m_writePoint - cutSize >= msgLen)
		{
// 			ui32 oldWritePoint = 0;//test value
// 			oldWritePoint = prevMsg.m_writePoint;//test value
			memcpy(prevMsg.m_buff + prevMsg.m_writePoint, data + cutSize, msgLen - prevMsg.m_writePoint);
			data = data + cutSize + (msgLen - prevMsg.m_writePoint);
			len = len - cutSize - (msgLen - prevMsg.m_writePoint);
			auto recvMsg = std::make_shared<Message>();
			recvMsg->swap(prevMsg);
// 			if (m_MessageParser && m_MessageParser->m_commands[recvMsg->getMsgID()] == nullptr)
// 			{
// 				nlogerr("KcpSession::parseMsg err 2");
// 			}

			nicehero::post([=] {
				self->handleMessage(recvMsg);
			});
			if (len > 0)
			{
				return parseMsg( data, len);
			}
			return true;
		}
// 		nlog("KcpSession::parseMsg else");
		memcpy(prevMsg.m_buff + prevMsg.m_writePoint, data + cutSize, len - cutSize);
		prevMsg.m_writePoint += len - cutSize;
		return true;
	}

	void KcpSession::removeSelf()
	{
	}

	void KcpSession::removeSelfImpl()
	{

	}

	void KcpSession::handleMessage(std::shared_ptr<Message> msg)
	{
		if (m_MessageParser)
		{
			if (m_MessageParser->m_commands[msg->getMsgID()] == nullptr)
			{
				nlogerr("KcpSession::handleMessage undefined msg:%d", ui32(msg->getMsgID()));
				return;
			}
			m_MessageParser->m_commands[msg->getMsgID()](*this, *msg.get());
		}
	}

	void KcpSession::close()
	{
		m_uid = "";
	}

	void KcpSession::setMessageParser(KcpMessageParser* messageParser)
	{
		m_MessageParser = messageParser;
	}

	kcpuid& KcpSession::getUid()
	{
		return m_uid;
	}

	void KcpSession::doSend(Message& msg,bool pureUdp)
	{
		auto self(shared_from_this());
		std::shared_ptr<Message> msg_ = std::make_shared<Message>();
		msg_->swap(msg);
		if (pureUdp)
		{
			std::shared_ptr<std::string> buffer(new std::string());
			char h = 4;
			buffer->append(1,h);
			buffer->append((char*)msg_->m_buff, msg_->getSize());
			m_impl->m_socket->async_send_to(asio::buffer(buffer->data(), buffer->size()),
				m_impl->m_endpoint, [](asio::error_code, std::size_t) {
			});
			return;
		}
		m_impl->getIoContext().post([this,self, msg_,pureUdp] {
			if (m_impl->m_kcp)
			{
				ikcp_send(m_impl->m_kcp, (const char*)msg_->m_buff, msg_->getSize());
				m_impl->m_kcpUpdateTime = Clock::getInstance()->getMilliSeconds();
			}
		});
	}

	void KcpSession::sendMessage(Message& msg, bool pureUdp)
	{
		doSend(msg,pureUdp);
	}

	void KcpSession::sendMessage(Serializable& msg, bool pureUdp)
	{
		Message msg_;
		msg.toMsg(msg_);
		sendMessage(msg_,pureUdp);
	}

	int KcpSessionImpl::udpOutput(const char *buf, int len, ikcpcb *kcp, void *user)
	{
		KcpSession* s = reinterpret_cast<KcpSession*>(user);
		std::shared_ptr<std::string> buffer(new std::string());
		buffer->assign(buf, len);
		s->m_impl->m_socket->async_send_to(
			asio::buffer(buffer->c_str(), len),
			s->m_impl->m_endpoint,
			[] (asio::error_code, std::size_t) {
		});
		return 0;
	}

	void KcpSession::init_kcp()
	{
		m_impl->m_kcp = ikcp_create(m_conv,this);
		m_impl->m_kcp->output = &KcpSessionImpl::udpOutput;
		// boot fast
		// param2 nodelay
		// param3 interval 2ms
		// param4 resend
		// param5 disable congestion control
		ikcp_nodelay(m_impl->m_kcp, 1, 2, 1, 1);
	}

	void KcpSession::init3(KcpServer& server)
	{

	}

	KcpSessionC::KcpSessionC()
	{
		m_isInit = false;
		m_impl = std::make_shared<KcpSessionImpl>(*this);
	}

	KcpSessionC::~KcpSessionC()
	{

	}

	bool KcpSessionC::connect(const std::string& ip, ui16 port)
	{
		asio::error_code ec;
		auto addr = asio::ip::address::from_string(ip, ec);
		if (ec)
		{
			nlogerr("KcpSessionC::KcpSessionC ip error:%s", ip.c_str());
			return false;
		}
		m_impl->m_socket->connect({addr,port} , ec);
		if (ec)
		{
			nlogerr("KcpSessionC::KcpSessionC connect error:%s", ec.message().c_str());
			return false;
		}
		return true;
	}

	void KcpSessionC::init(bool isSync)
	{
		std::shared_ptr<asio::steady_timer> t = std::make_shared<asio::steady_timer>(getWorkerService());
		auto f = [&, t](std::error_code ec) {
			t->cancel();
			if (ec)
			{
				nlogerr("KcpSessionC::init err %s", ec.message().c_str());
				return;
			}
			ui8 data_[PUBLIC_KEY_SIZE + 8 + HASH_SIZE + SIGN_SIZE] = "";
			std::size_t len = m_impl->m_socket->receive(
				asio::buffer(data_, sizeof(data_)));
			if (ec)
			{
				nlogerr("KcpSessionC::init err %s", ec.message().c_str());
				return;
			}
			if (len < sizeof(data_))
			{
				nlogerr("server sign data len err");
				return;
			}
			if (checkServerSign(data_) == 1)
			{
				nlogerr("server sign err");
				return;
			}
			ui8 sendSign[PUBLIC_KEY_SIZE + SIGN_SIZE] = { 0 };
			memcpy(sendSign, m_publicKey, PUBLIC_KEY_SIZE);
			if (uECC_sign(m_privateKey
				, (const ui8*)data_ + PUBLIC_KEY_SIZE + 8, HASH_SIZE
				, sendSign + PUBLIC_KEY_SIZE
				, uECC_secp256k1()) != 1)
			{
				nlogerr("uECC_sign() failed\n");
				return;
			}
			m_uid = std::string((const char*)data_, PUBLIC_KEY_SIZE);
			static ui64 nowSerialID = 10000;
			m_serialID = nowSerialID++;
			m_impl->m_socket->send(asio::buffer(sendSign, PUBLIC_KEY_SIZE + SIGN_SIZE));
			if (ec)
			{
				nlogerr("KcpSessionC::init err %s", ec.message().c_str());
				return;
			}
			m_isInit = true;
			m_MessageParser = &getKcpMessagerParse(typeid(*this));
		};
		if (isSync)
		{
			m_impl->m_socket->async_wait(
				asio::ip::udp::socket::wait_read,f);
			t->expires_from_now(std::chrono::seconds(2));
			t->async_wait([&](std::error_code ec) {
				if (!ec)
				{
					close();
				}
			});
		}
		else
		{
			f(std::error_code());
		}
	}


	void KcpSessionC::startRead()
	{
		doRead();
	}

	void KcpSessionC::removeSelf()
	{
		close();
	}

	int KcpSessionC::checkServerSign(ui8* data_)
	{
		bool allSame = true;
		for (size_t i = 0; i < PUBLIC_KEY_SIZE; ++i)
		{
			if (m_publicKey[i] != data_[i])
			{
				allSame = false;
				break;
			}
		}
		if (allSame)
		{
			nlogerr("same publicKey");
			return 1;
		}
		ui64& serverTime = *(ui64*)(data_ + PUBLIC_KEY_SIZE);
		int ret = 0;
		if (nNow > serverTime + 10 || nNow < serverTime - 10)
		{
			nlogerr("your time is diff from serverTime");
			ret = 2;
		}
		ui64 checkHash[HASH_SIZE / 8] = { 0 };
		sha3(data_, PUBLIC_KEY_SIZE + 8, checkHash, HASH_SIZE);
		allSame = true;
		for (size_t i = 0; i < HASH_SIZE / 8; ++i)
		{
			if (checkHash[i] != ((ui64*)(data_ + PUBLIC_KEY_SIZE + 8))[i])
			{
				allSame = false;
			}
		}
		if (!allSame)
		{
			nlogerr("error check hash");
			return 1;
		}
		if (uECC_verify(data_, (const ui8*)(data_ + PUBLIC_KEY_SIZE + 8), HASH_SIZE, data_ + PUBLIC_KEY_SIZE + 8 + HASH_SIZE, uECC_secp256k1()) != 1)
		{
			nlogerr("error check hash2");
			return 1;
		}
		return ret;
	}


}

