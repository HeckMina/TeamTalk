#include "stdafx.h"
#include "client_imp.h"
#include <list>

#include "api/ihttp_client.h"
#include "api/log.h"

#include "json/json.h"

// Э��
#include "protocol/IM.BaseDefine.pb.h"
#include "protocol/IM.Other.pb.h"
#include "network/PBHeader.h"
#include "network/UtilPdu.h"

#define MAX_RECV_BUFFER_LEN 10240	//Tcp���ݶ�ȡ������
#define MAX_SEND_BUFFER_LEN 1024

namespace him {
	std::shared_ptr<IClient> NewClientModule()
	{
		std::shared_ptr<ClientImp> instance = std::make_shared<ClientImp>();

		g_client_list_mutex_.lock();
		g_client_list_.push_back(instance);
		g_client_list_mutex_.unlock();

		return std::dynamic_pointer_cast<IClient>(instance);
	}

	ClientImp::ClientImp()
		: server_port_(0)
		, callback_(nullptr)
		, client_state_(ClientState::kClientDisconnect)
		, receive_thread_run_(true)
		, write_buffer_(nullptr)
		, seq_(0)
		, heartbeat_time_(0)
	{
		tcp_client_ = std::make_shared<boost::asio::ip::tcp::socket>(io_server_);
		write_buffer_ = new unsigned char[MAX_SEND_BUFFER_LEN];
		::memset(write_buffer_, 0, MAX_SEND_BUFFER_LEN);
	}
	ClientImp::~ClientImp()
	{
		receive_thread_run_ = false;
		if (receive_thread_ != nullptr) {
			if (receive_thread_->joinable())
				receive_thread_->join();
			receive_thread_ = nullptr;
		}
		if (write_buffer_ != nullptr) {
			delete[] write_buffer_;
			write_buffer_ = nullptr;
		}
	}

	void ClientImp::Login(std::string user_name, std::string pwd, std::string server_ip, unsigned short port, const LoginResultCallback &callback)
	{
		callback_login_res_ = callback;
		client_state_ = ClientState::kClientDisconnect;

		char urlBuf[128] = { 0 };
		sprintf(urlBuf, "http://%s:%d/msg_server", server_ip.c_str(), port);

		std::shared_ptr<IHttpClient> client = him::GetHttpClientModule();
		std::string response;
		logd("��ѯ���÷������б���...\n");
		// ������Ĭ��3�볬ʱ
		UrlCode code = client->HttpGetReq(std::string(urlBuf), response);
		if (code == UrlCode::URLE_OK) {
			Json::Value json_value;
			Json::Reader json_reader;
			if (!json_reader.parse(response, json_value)) {
				loge("��ѯ�������ɹ������ǽ�������json���ʧ�ܣ��޷�������¼ \n");
				_OnLogin(-1, "server internal error");
				return;
			}

			int json_code = json_value["code"].asInt();
			std::string json_msg = json_value["msg"].asString();
			if (json_code != 0) {
				loge("��ѯ�������ɹ������������ڲ���������%s \n", json_msg.c_str());
				_OnLogin(-1, json_msg);
				return;
			}

			std::string msg_server_ip = json_value["priorIP"].asString();
			std::string msg_server_port = json_value["port"].asString();
			if (msg_server_ip.empty() || msg_server_port.empty()) {
				loge("��ѯ�������ɹ������ǽ�������json���ʧ�ܣ��޷�������¼ \n");
				_OnLogin(-1, "server internal error");
				return;
			}

			logd("��ѯ�ɹ�����ʼ������Ϣ��������%s:%s \n", msg_server_ip.c_str(), msg_server_port.c_str());

			boost::system::error_code code;
			boost::asio::ip::tcp::endpoint e(boost::asio::ip::address_v4::from_string(msg_server_ip), std::stoi(msg_server_port));
			tcp_client_->connect(e, code);

			if (code) {
				boost::system::system_error err(code);
				loge("connect error:%s \n", err.what());
				_OnLogin(-1, "�޷�����Ŀ�������");
				return;
			}
			client_state_ = ClientState::kClientConnectedOk;

			// ���������߳�
			receive_thread_ = std::make_shared<std::thread>(&ClientImp::ReceiveThreadProc, this);

			logd("connect success \n");
			// ������֤
			IM::Login::IMLoginReq req;
			req.set_user_name(user_name.c_str());
			req.set_password(pwd.c_str());
			req.set_online_status(IM::BaseDefine::USER_STATUS_ONLINE);
			req.set_client_type(IM::BaseDefine::CLIENT_TYPE_WINDOWS);
			req.set_client_version("v1");

			int temp_buf_len = req.ByteSize();
			unsigned char* temp_buf = new unsigned char[temp_buf_len];
			req.SerializeToArray(temp_buf, temp_buf_len);
			Send(IM::BaseDefine::SID_LOGIN, IM::BaseDefine::CID_LOGIN_REQ_USERLOGIN, temp_buf, temp_buf_len);
			delete[] temp_buf;
		}
		else {
			loge("http connect error: %d \n", code);
		}
	}
	ClientState ClientImp::GetClientState()
	{
		return ClientState::kClientDisconnect;
	}
	void ClientImp::LoginOut()
	{
		// ����ע������
		IM::Login::IMLogoutReq req;
		int temp_buf_len = req.ByteSize();
		unsigned char* temp_buf = new unsigned char[temp_buf_len];
		req.SerializeToArray(temp_buf, temp_buf_len);
		Send(IM::BaseDefine::SID_LOGIN, IM::BaseDefine::CID_LOGIN_REQ_LOGINOUT, temp_buf, temp_buf_len);
		delete[] temp_buf;

		// �յ���Ӧ��ע��
		/*tcp_client_->close();
		receive_thread_run_ = false;
		if (receive_thread_ != nullptr && receive_thread_->joinable()) {
			receive_thread_->join();
			receive_thread_ = nullptr;
		}*/
	}

	int ClientImp::Send(int server_id, int msg_id, const unsigned char* data, int len)
	{
		// ��װЭ��ͷ
		PBHeader header;
		header.SetModuleId(server_id);
		header.SetCommandId(msg_id);
		header.SetSeqNumber(ThreadSafeGetSeq());
		header.SetLength(len + HEADER_LENGTH);

		// �������ݲ�
		::memcpy(write_buffer_, header.GetSerializeBuffer(), HEADER_LENGTH);
		::memcpy(write_buffer_ + HEADER_LENGTH, data, len);

		if (tcp_client_->is_open()) {
			size_t send_len = tcp_client_->write_some(boost::asio::buffer(write_buffer_, len + HEADER_LENGTH));
			if (send_len < (unsigned int)len) {
				loge("tcp send error,source len=%d B,send len=%d \n", len, send_len);
			}
			return send_len;
		}
		return -1;
	}
	void ClientImp::SetReceiveDataCallback(ReceiveDateDelegate &callback)
	{
		callback_ = callback;
	}
	void ClientImp::ReceiveThreadProc()
	{
		unsigned char buf[MAX_RECV_BUFFER_LEN] = { 0 };

		while (receive_thread_run_)
		{
			if (tcp_client_->is_open() && client_state_ == ClientState::kClientConnectedOk) {
				boost::system::error_code err_code;
				// ����ֱ��һ�´�������
				size_t len = tcp_client_->read_some(boost::asio::buffer(buf, MAX_RECV_BUFFER_LEN), err_code);

				if (err_code == boost::asio::error::eof) {
					loge("remote has closed connection \n");
					tcp_client_->close();
				}
				else if (err_code) {
					boost::system::system_error err_desc(err_code);
					loge("receive thread read data error:%s \n", err_desc.what());
				}
				// �Ƿ����ճ�����⣿�����ش�İ�ʱ��ֻ��ȡ��һ��
				if (len > 0) {
					OnReceive(buf, len);
				}
			}
			else {
				Sleep(100);
			}
		}

		logd("socket receive thread has destory \n");
		tcp_client_->close();
	}

	void ClientImp::OnReceive(unsigned char* buf, int len)
	{
		PBHeader head;
		head.UnSerialize(buf, HEADER_LENGTH);

		// ������
		if (head.GetCommandId() == IM::BaseDefine::CID_OTHER_HEARTBEAT) {
			time(&heartbeat_time_);
			logd("receive heartbeat:%lld \n", heartbeat_time_);
			return;
		}
		// ��¼��Ӧ
		else if (head.GetCommandId() == IM::BaseDefine::CID_LOGIN_RES_USERLOGIN) {
			IM::Login::IMLoginRes res;
			res.ParseFromArray(buf + HEADER_LENGTH, len - HEADER_LENGTH);
			_OnLoginRes(res);
			return;
		}
		// ע��
		else if (head.GetCommandId() == IM::BaseDefine::CID_LOGIN_RES_LOGINOUT) {
			receive_thread_run_ = false;
			return;
		}

		logd("receive new msg:moduleId=%d,msgId=%d,seq=%d \n", head.GetModuleId(), head.GetCommandId(), head.GetSeqNumber());
		// ȥ����Э��ͷ�����ݲ�
		unsigned char *temp_buf = new unsigned char[len - HEADER_LENGTH];
		::memcpy(temp_buf, buf + HEADER_LENGTH, len - HEADER_LENGTH);
		if (callback_ != nullptr) {
			callback_(temp_buf, len - HEADER_LENGTH);
		}
	}

	void ClientImp::SendHeartBeat() {
		if (tcp_client_->is_open() && client_state_ == him::ClientState::kClientConnectedOk) {
			IM::Other::IMHeartBeat req;
			int temp_buf_len = req.ByteSize();
			unsigned char* temp_buf = new unsigned char[temp_buf_len];
			req.SerializeToArray(temp_buf, temp_buf_len);
			Send(IM::BaseDefine::SID_OTHER, IM::BaseDefine::CID_OTHER_HEARTBEAT, temp_buf, temp_buf_len);
			delete[] temp_buf;
		}
	}
	size_t ClientImp::GetLastHeartBeatTime() {
		return (size_t)heartbeat_time_;
	}

	int ClientImp::ThreadSafeGetSeq()
	{
		int temp_seq = 0;
		{
			std::lock_guard<std::mutex> lock(seq_mutex_);
			seq_++;
			temp_seq = seq_;
		}
		return temp_seq;
	}
	void ClientImp::_OnLoginRes(IM::Login::IMLoginRes res)
	{
		logd("�յ���¼��Ӧ����¼�����%d,������%s \n", res.result_code(), res.result_string().c_str());
		_OnLogin((int)res.result_code(), res.result_string());
	}
	void ClientImp::_OnLogin(int code, std::string msg)
	{
		if (callback_login_res_ != nullptr) {
			callback_login_res_(code, msg);
		}
	}
}

