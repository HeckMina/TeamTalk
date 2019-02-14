/** @file client_imp.h
  * @brief
  * @author Captain China
  * @date 2019/01/27
  */

#ifndef _CLIENT_IMP_FC05AC64_1E43_46D5_9781_F90C2803E96E_H_
#define _CLIENT_IMP_FC05AC64_1E43_46D5_9781_F90C2803E96E_H_

#include "api/iclient.h"
#include "protocol/IM.Login.pb.h"
#include "protocol/IM.Message.pb.h"
#include <google/protobuf/message_lite.h>
#include <boost/asio.hpp>
#include <thread>
#include <mutex>

namespace him {

	/** @struct MessageModel
	  * @brief ��Ϣ�ṹ�嶨��
	  */
	struct MessageModel
	{
		unsigned int from_user_id;
		unsigned int to_session_id;
		unsigned int msg_id;
		unsigned int create_time;
		IM::BaseDefine::MsgType msg_type;
		std::string msg_data;

		SendMsgCallback callback;
		unsigned int	seq;
	};

	/** @class Client_Imp
	  * @brief IClientʵ��
	  */
	class ClientImp : public IClient
	{
	public:
		ClientImp();
		~ClientImp();

	public:
		virtual void Login(std::string user_name, std::string pwd, std::string server_ip, unsigned short port, const LoginResultCallback &callback);
		virtual ClientState GetClientState();
		virtual void LoginOut();

		// �̰߳�ȫ
		virtual int Send(int server_id, int msg_id, google::protobuf::MessageLite* pbBody);
		virtual int Send(int server_id, int msg_id, unsigned int seq, google::protobuf::MessageLite* pbBody);

		// ���̰߳�ȫ
		virtual int Send(int server_id, int msg_id, const unsigned char* data, int len);
		/*virtual int Send(int server_id, int msg_id,unsigned int seq, const unsigned char* data, int len);*/
		virtual void SetReceiveDataCallback(ReceiveDateDelegate &callback);

		// msg
		virtual void SendTextMsg(unsigned int to_session_id, bool is_group, std::string text, const SendMsgCallback &callback);
	public:
		void ReceiveThreadProc();
		virtual void OnReceive(unsigned char* buf, int len);

	public:// �������
		void SendHeartBeat();
		size_t GetLastHeartBeatTime();
	private:
		unsigned int ThreadSafeGetSeq();
		void _OnLoginRes(IM::Login::IMLoginRes res);
		void _OnLogin(int code, std::string msg);
		void _OnMsgAck(unsigned int seq, IM::Message::IMMsgDataAck res);
	private:
		ClientState					client_state_;
		ReceiveDateDelegate			callback_;
		LoginResultCallback			callback_login_res_;

		std::string					server_ip_;
		std::string					user_name_;
		unsigned short				server_port_;
		unsigned int				seq_;
		bool						receive_thread_run_;
		time_t						heartbeat_time_;

		// boost
		boost::asio::io_service		io_server_;
		std::mutex					seq_mutex_;
		std::shared_ptr<boost::asio::ip::tcp::socket>	tcp_client_;
		std::shared_ptr<std::thread>					receive_thread_;

		// msg
		std::map<unsigned int, MessageModel>	send_msg_map_;
		std::mutex								msg_mutex_;
		unsigned int							server_time_;		// �������ϵ�ʱ�䣬��Ϣʱ��Ҫ�Ը�ʱ��Ϊ��׼
		int										server_time_offset_;// ����ʱ��ͷ�����ʱ���ƫ��
		IM::BaseDefine::UserInfo				user_info_;			// �û���Ϣ
		unsigned int							send_msg_count_;	// �ѷ�����Ϣ
		unsigned int							send_msg_success_count_;// �ɹ����͵���Ϣ
	};

	// heartbeat
	extern std::list<std::weak_ptr<ClientImp>>	g_client_list_;
	extern std::mutex							g_client_list_mutex_;
}

#endif//_CLIENT_IMP_FC05AC64_1E43_46D5_9781_F90C2803E96E_H_