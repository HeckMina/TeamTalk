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

// aes����
#include "security.h"

#define MAX_RECV_BUFFER_LEN 10240	//Tcp���ݶ�ȡ������

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
		, seq_(0)
		, heartbeat_time_(0)
		, send_msg_count_(0)
		, send_msg_success_count_(0)
		, server_time_(0)
		, server_time_offset_(0)
	{
		tcp_client_ = std::make_shared<boost::asio::ip::tcp::socket>(io_server_);
	}
	ClientImp::~ClientImp()
	{
		receive_thread_run_ = false;
		if (receive_thread_ != nullptr) {
			if (receive_thread_->joinable())
				receive_thread_->join();
			receive_thread_ = nullptr;
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
		logd("��ѯ���÷������б���...");
		// ������Ĭ��3�볬ʱ
		UrlCode code = client->HttpGetReq(std::string(urlBuf), response);
		if (code == UrlCode::URLE_OK) {
			Json::Value json_value;
			Json::Reader json_reader;
			if (!json_reader.parse(response, json_value)) {
				loge("��ѯ�������ɹ������ǽ�������json���ʧ�ܣ��޷�������¼");
				_OnLogin(-1, "server internal error");
				return;
			}

			int json_code = json_value["code"].asInt();
			std::string json_msg = json_value["msg"].asString();
			if (json_code != 0) {
				loge("��ѯ�������ɹ������������ڲ���������%s", json_msg.c_str());
				_OnLogin(-1, json_msg);
				return;
			}

			std::string msg_server_ip = json_value["priorIP"].asString();
			std::string msg_server_port = json_value["port"].asString();
			if (msg_server_ip.empty() || msg_server_port.empty()) {
				loge("��ѯ�������ɹ������ǽ�������json���ʧ�ܣ��޷�������¼");
				_OnLogin(-1, "server internal error");
				return;
			}

			logd("��ѯ�ɹ�����ʼ������Ϣ��������%s:%s", msg_server_ip.c_str(), msg_server_port.c_str());

			boost::system::error_code code;
			boost::asio::ip::tcp::endpoint e(boost::asio::ip::address_v4::from_string(msg_server_ip), std::stoi(msg_server_port));
			tcp_client_->connect(e, code);

			if (code) {
				boost::system::system_error err(code);
				loge("connect error:%s", err.what());
				_OnLogin(-1, "�޷�����Ŀ�������");
				return;
			}
			client_state_ = ClientState::kClientConnectedOk;

			// ���������߳�
			receive_thread_ = std::make_shared<std::thread>(&ClientImp::ReceiveThreadProc, this);

			logd("connect success");
			// ������֤
			IM::Login::IMLoginReq req;
			req.set_user_name(user_name.c_str());
			req.set_password(pwd.c_str());
			req.set_online_status(IM::BaseDefine::USER_STATUS_ONLINE);
			req.set_client_type(IM::BaseDefine::CLIENT_TYPE_WINDOWS);
			req.set_client_version("v1");

			Send(IM::BaseDefine::SID_LOGIN, IM::BaseDefine::CID_LOGIN_REQ_USERLOGIN, &req);
		}
		else {
			loge("http connect error: %d", code);
		}
	}
	ClientState ClientImp::GetClientState()
	{
		return client_state_;
	}
	void ClientImp::LoginOut()
	{
		// ����ע������
		IM::Login::IMLogoutReq req;
		Send(IM::BaseDefine::SID_LOGIN, IM::BaseDefine::CID_LOGIN_REQ_LOGINOUT, &req);

		// �յ���Ӧ��ע��
		/*tcp_client_->close();
		receive_thread_run_ = false;
		if (receive_thread_ != nullptr && receive_thread_->joinable()) {
			receive_thread_->join();
			receive_thread_ = nullptr;
		}*/
	}

	int ClientImp::Send(int server_id, int msg_id, google::protobuf::MessageLite* pbBody)
	{
		return Send(server_id, msg_id, ThreadSafeGetSeq(), pbBody);
	}
	int ClientImp::Send(int server_id, int msg_id, unsigned int seq, google::protobuf::MessageLite* pbBody)
	{
		// ��װЭ��ͷ
		unsigned int len = pbBody->ByteSize() + HEADER_LENGTH;
		PBHeader header;
		header.SetModuleId(server_id);
		header.SetCommandId(msg_id);
		header.SetSeqNumber(seq);
		header.SetLength(len);

		std::unique_ptr<unsigned char> buf(new unsigned char[len]);
		::memset(buf.get(), 0, len);
		::memcpy(buf.get(), header.GetSerializeBuffer(), HEADER_LENGTH);
		pbBody->SerializeToArray(buf.get() + HEADER_LENGTH, pbBody->ByteSize());

		if (tcp_client_->is_open()) {
			size_t send_len = tcp_client_->write_some(boost::asio::buffer(buf.get(), len));
			if (send_len < (unsigned int)len) {
				loge("tcp send error,source len=%d B,send len=%d", len, send_len);
			}
			return send_len;
		}

		return -1;
	}


	int ClientImp::Send(int server_id, int msg_id, const unsigned char* data, int len)
	{
		// ��װЭ��ͷ
		len = len + HEADER_LENGTH;

		PBHeader header;
		header.SetModuleId(server_id);
		header.SetCommandId(msg_id);
		header.SetSeqNumber(ThreadSafeGetSeq());
		header.SetLength(len);

		// �������ݲ�
		std::unique_ptr<unsigned char> buf(new unsigned char[len]);
		::memset(buf.get(), 0, len);
		::memcpy(buf.get(), header.GetSerializeBuffer(), HEADER_LENGTH);
		::memcpy(buf.get() + HEADER_LENGTH, data, len - HEADER_LENGTH);

		if (tcp_client_->is_open()) {
			size_t send_len = tcp_client_->write_some(boost::asio::buffer(buf.get(), len));
			if (send_len < (unsigned int)len) {
				loge("tcp send error,source len=%d B,send len=%d", len, send_len);
			}
			return send_len;
		}
		return -1;
	}
	/*
	int ClientImp::Send(int server_id, int msg_id, unsigned int seq, const unsigned char* data, int len)
	{
		// ��װЭ��ͷ
		PBHeader header;
		header.SetModuleId(server_id);
		header.SetCommandId(msg_id);
		header.SetSeqNumber(seq);
		header.SetLength(len + HEADER_LENGTH);

		// �������ݲ�
		::memcpy(write_buffer_, header.GetSerializeBuffer(), HEADER_LENGTH);
		::memcpy(write_buffer_ + HEADER_LENGTH, data, len);

		if (tcp_client_->is_open()) {
			size_t send_len = tcp_client_->write_some(boost::asio::buffer(write_buffer_, len + HEADER_LENGTH));
			if (send_len < (unsigned int)len) {
				loge("tcp send error,source len=%d B,send len=%d", len, send_len);
			}
			return send_len;
		}
		return -1;
	}
	*/

	void ClientImp::SetReceiveDataCallback(ReceiveDateDelegate &callback)
	{
		callback_ = callback;
	}
	void ClientImp::SendTextMsg(unsigned int to_session_id, bool is_group, std::string text, const SendMsgCallback &callback)
	{
		time_t t;
		time(&t);
		unsigned int seq = ThreadSafeGetSeq();

		// ��Ϣ���ݶ˶Զ˼���
		char *msg_buf = nullptr;
		unsigned int out_len = 0;
		if (EncryptMsg(text.c_str(), text.length(), &msg_buf, out_len) != 0) {
			loge("��Ϣ����ʧ�ܣ�");
			Free(msg_buf);
			return;
		}
		std::string encrypt_msg(msg_buf, out_len);
		Free(msg_buf);

		MessageModel model;
		model.from_user_id = user_info_.user_id();
		model.to_session_id = to_session_id;
		model.msg_id = 0;
		model.msg_type = is_group ? IM::BaseDefine::MSG_TYPE_GROUP_TEXT : IM::BaseDefine::MSG_TYPE_SINGLE_TEXT;
		model.create_time = (unsigned int)t + server_time_offset_;
		model.callback = callback;
		model.msg_data = encrypt_msg;
		model.seq = seq;

		// ��ӵ�ackȷ���б���
		msg_mutex_.lock();
		send_msg_map_.insert(make_pair(seq, model));
		send_msg_count_++;
		msg_mutex_.unlock();

		IM::Message::IMMsgData req;
		req.set_from_user_id(model.from_user_id);
		req.set_to_session_id(model.to_session_id);
		req.set_msg_id(model.msg_id);
		req.set_msg_type(model.msg_type);

		req.set_create_time(model.create_time);
		req.set_msg_data(model.msg_data);

		Send(IM::BaseDefine::SID_MSG, IM::BaseDefine::CID_MSG_DATA, model.seq, &req);
		logd("����һ��������Ϣ��to_sesion_id=%d,seq=%d", model.to_session_id, model.seq);
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
					loge("remote has closed connection");
					tcp_client_->close();
				}
				else if (err_code) {
					boost::system::system_error err_desc(err_code);
					loge("receive thread read data error:%s", err_desc.what());
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

		logd("socket receive thread has destory");
		tcp_client_->close();
	}

	void ClientImp::OnReceive(unsigned char* buf, int len)
	{
		PBHeader head;
		head.UnSerialize(buf, HEADER_LENGTH);

		// ������
		if (head.GetCommandId() == IM::BaseDefine::CID_OTHER_HEARTBEAT) {
			time(&heartbeat_time_);
			logd("receive heartbeat:%lld", heartbeat_time_);
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
		// MsgAck�ظ�
		else if (head.GetCommandId() == IM::BaseDefine::CID_MSG_DATA_ACK) {
			IM::Message::IMMsgDataAck res;
			res.ParseFromArray(buf + HEADER_LENGTH, len - HEADER_LENGTH);
			_OnMsgAck(head.GetSeqNumber(), res);
			return;
		}

		logd("receive new msg:moduleId=%d,msgId=%d,seq=%d", head.GetModuleId(), head.GetCommandId(), head.GetSeqNumber());
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
			Send(IM::BaseDefine::SID_OTHER, IM::BaseDefine::CID_OTHER_HEARTBEAT, &req);
		}
	}
	size_t ClientImp::GetLastHeartBeatTime() {
		return (size_t)heartbeat_time_;
	}

	unsigned int ClientImp::ThreadSafeGetSeq()
	{
		unsigned int temp_seq = 0;
		{
			std::lock_guard<std::mutex> lock(seq_mutex_);
			seq_++;
			temp_seq = seq_;
		}
		return temp_seq;
	}
	void ClientImp::_OnLoginRes(IM::Login::IMLoginRes res)
	{
		logd("�յ���¼��Ӧ����¼�����%d,������%s", res.result_code(), res.result_string().c_str());

		if (res.result_code() == 0) {
			time_t cur_time;
			time(&cur_time);

			server_time_ = res.server_time();
			server_time_offset_ = server_time_ - (unsigned int)cur_time; // ��¼ʱ��ƫ��

			IM::BaseDefine::UserStatType user_online_status = res.online_status();
			user_info_ = res.user_info();
			logd("������ʱ�䣺%d������ʱ�䣺%d��ƫ������%d���û�״̬��%d", server_time_, (unsigned int)cur_time, server_time_offset_, user_online_status);
			logd("�û�ID=%d,�ǳƣ�%s������ID=%d", user_info_.user_id(), user_info_.user_nick_name().c_str(), user_info_.department_id());
		}

		_OnLogin((int)res.result_code(), res.result_string());
	}
	void ClientImp::_OnLogin(int code, std::string msg)
	{
		if (callback_login_res_ != nullptr) {
			callback_login_res_(code, msg);
		}
	}
	void ClientImp::_OnMsgAck(unsigned int seq, IM::Message::IMMsgDataAck res)
	{
		std::map<unsigned int, MessageModel>::iterator it;
		{
			std::lock_guard<std::mutex> lock(msg_mutex_);
			send_msg_success_count_++;
			it = send_msg_map_.find(seq);
		}
		if (it != send_msg_map_.end()) {
			MessageModel item = send_msg_map_.at(seq);
			if (item.callback != nullptr) {
				item.callback(seq, true);
			}
			send_msg_map_.erase(seq);
		}
		logd("�����ڷ���������Ϣack�յ��ظ�,seq=%d,msgId=%d,��Ϣ������[%d / %d]", seq, res.msg_id(), send_msg_success_count_, send_msg_count_);
	}
}

