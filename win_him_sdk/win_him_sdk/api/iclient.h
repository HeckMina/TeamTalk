/** @file client.h
  * @brief SDKȫ�ֹ��������¼��ע����
  * @author Captain China
  * @date 2019/01/27
  */

#ifndef _CLIENT_FD1A9425_314E_4337_89BE_B3F537C78A8F_H_
#define _CLIENT_FD1A9425_314E_4337_89BE_B3F537C78A8F_H_

#include "him_sdk_dll.h"
#include <memory>
#include <string>
#include <functional>

namespace him {

	/** @enum ClientState
	  * @brief
	  */
	typedef enum
	{
		kClientConnectedOk = 0, // ������
		kClientDisconnect = 1,  // ���ӶϿ�
		kClientReConnect = 2,   // ������
	}ClientState;

#define BIND_CALLBACK_2(func)	std::bind(&func, this, placeholders::_1, placeholders::_2)
	typedef std::function<void(const char* data, int len)> ReceiveDateDelegate; // ���յ����ݵĻص���������

	class HIM_SDK_API IClient
	{
	public:// login

		/** @fn void Login(std::string user_name, std::string pwd, std::string serverIp, unsigned short port)
		  * @brief ��¼
		  * @param user_name: �û���
		  * @param pwd: ����
		  * @param server_ip: ������ip��ַ
		  * @param port: �˿�
		  */
		virtual void Login(std::string user_name, std::string pwd, std::string server_ip, unsigned short port) = 0;

		/** @fn ClientState GetClientState()
		  * @brief ��ȡ��ǰ�ͻ�������״̬
		  * @return ״̬
		  */
		virtual ClientState GetClientState() = 0;

		/** @fn void LoginOut()
		  * @brief ע��
		  */
		virtual void LoginOut() = 0;

	public:// send
		/** @fn int Send(int server_id, int msg_id, const char* data, int len)
		  * @brief ����
		  * @param server_id: ģ��ID
		  * @param msg_id: ��ϢID
		  * @param data: ���ݻ�����
		  * @param len: Ҫ���͵ĳ���
		  * @return �ѷ��͵����ݳ���
		  */
		virtual int Send(int server_id, int msg_id, const char* data, int len) = 0;
		virtual void SetReceiveDataCallback(ReceiveDateDelegate callback) = 0;
	};

	/** @fn void Init();
	  * @brief ��ʼ��sdk
	  */
	HIM_SDK_API void GlobalInit();
	/** @fn void Uninit()
	  * @brief �˳�ǰ����
	  */
	HIM_SDK_API void GlobalUninit();
	/** @fn std::shared_ptr<IClient> FactoryNew();
	  * @brief ��ȡClient����ʵ��
	  * @retrun �µ�ʵ��
	  */
	HIM_SDK_API std::shared_ptr<IClient> GetClientModule();
}

#endif//_CLIENT_FD1A9425_314E_4337_89BE_B3F537C78A8F_H_