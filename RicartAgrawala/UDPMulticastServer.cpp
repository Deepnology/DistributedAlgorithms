#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
//#include <string.h> //don't include <string.h> when including <string> !!
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <memory.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <time.h>
#include <vector>
#include <string>
#include <queue>
#include "../CustomUtility/CustomUtility.hpp"
#include "../VectorClock/VectorClock.hpp"
#include "RicartAgrawala.hpp"
#define DEST_IP_ADDRESS "127.0.0.1"
#define START_PORT 9000
static bool PRINT_SEND_MESSAGES = false;
static bool PRINT_RECV_MESSAGES = false;
struct Shared
{
	pthread_mutex_t mutexMain; //between main thread, recvr thread, sender thread, and access cs thread
	const unsigned int serverPort;
	const unsigned int totalServer;
	const int serverSocketFD;
	DistributedAlgorithms::VectorClock vecClock;
	DistributedAlgorithms::RicartAgrawala RA;

	pthread_mutex_t mutexDistributedCS;
	pthread_cond_t condDistributedCS;

	Shared(unsigned int _serverPort, unsigned int _totalServer, int _serverSocketFD): serverPort(_serverPort), totalServer(_totalServer), serverSocketFD(_serverSocketFD), vecClock(_totalServer, _serverPort-START_PORT), RA(_totalServer, _serverPort-START_PORT)
	{
		int mutexMainInit = pthread_mutex_init(&mutexMain, NULL);
		if (mutexMainInit)
		{
			printf("pthread_mutex_init failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		int mutexDistributedCSInit = pthread_mutex_init(&mutexDistributedCS, NULL);
		if (mutexDistributedCSInit)
		{
			printf("pthread_mutex_init failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		int condDistributedCSInit = pthread_cond_init(&condDistributedCS, NULL);
		if (condDistributedCSInit)
		{
			printf("pthread_cond_init failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
};
static void * SenderThreadFunc(void * args)
{
	int setCancelState = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	if (setCancelState)
	{
		printf("pthread_setcancelstate failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	struct Shared * shared = (struct Shared*) args;
	pthread_mutex_lock(&shared->mutexMain);
	const unsigned int SERVER_PORT = shared->serverPort;
	const unsigned int TOTAL_SERVER = shared->totalServer;
	const int serverSocketFD = shared->serverSocketFD;
	std::vector<unsigned int> ports;
	for (auto i = 0; i < shared->totalServer; ++i)
		if (i+START_PORT != shared->serverPort) ports.push_back(i+START_PORT);
	pthread_mutex_unlock(&shared->mutexMain);

	for (;;)
	{
		CustomUtility::Shuffle<unsigned int>(ports);
		for (auto i = 0; i < ports.size(); ++i) //randomly multicast messages
		{
			//sleep for random nanosec to simulate network delay (pthread cancellation point)
			CustomUtility::RandNanoSleep(1000000, 999999999);

			char buf[256];
			memset(buf, 0, sizeof(buf));
			unsigned int destPort = ports[i];
			
			pthread_mutex_lock(&shared->mutexMain);
			std::vector<unsigned long long> requestReplyMsgToSend; //[destIdx,hostIdx,isRequest,vecClockAtRequest]
			if (shared->RA.OnSend(requestReplyMsgToSend)) //multicast request/reply messages with first priority
			{
				destPort = (unsigned int)requestReplyMsgToSend[0] + START_PORT;
				const unsigned int curPort = (const unsigned int)requestReplyMsgToSend[1] + START_PORT;
				const unsigned int curIsRequest = (const unsigned int)requestReplyMsgToSend[2];
				const std::string vecClockAtRequest = CustomUtility::ToStr<unsigned long long>(std::vector<unsigned long long>(requestReplyMsgToSend.begin()+3, requestReplyMsgToSend.end()));
				shared->vecClock.OnSend();
				std::string vc = shared->vecClock.ToStr();
				sprintf(buf, "%u %u %s %s", curPort, curIsRequest, vc.c_str(), vecClockAtRequest.c_str()); //multicast marker message "curPort curIsRequest curVecClock vecClockAtRequest"

				--i; //repeat
			}
			else
			{
				destPort = ports[i];
				shared->vecClock.OnSend();
				std::string vc = shared->vecClock.ToStr();
				sprintf(buf, "%u %u %s", SERVER_PORT, 2, vc.c_str()); //multicast message "curPort curIsRequest curVecClock"
			}
			pthread_mutex_unlock(&shared->mutexMain);

			struct sockaddr_in destAddr;
			memset(&destAddr, 0, sizeof(destAddr));
			destAddr.sin_family = AF_INET;
			destAddr.sin_port = htons(destPort);
			destAddr.sin_addr.s_addr = inet_addr(DEST_IP_ADDRESS);
			int sentSize = sendto(serverSocketFD, buf, sizeof(buf), 0,
					(struct sockaddr*)&destAddr, sizeof(struct sockaddr));
			if (sentSize == -1)
			{
				printf("SenderThread sendto: %s\n", strerror(errno));
				exit(EXIT_FAILURE);
			}
			if (PRINT_SEND_MESSAGES)
				printf("Sent: %d, %s, %u: %.*s\n", sentSize, inet_ntoa(destAddr.sin_addr), ntohs(destAddr.sin_port), sentSize, buf);
		}
	}
}
static void * RecvrThreadFunc(void * args)
{
	int setCancelState = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	if (setCancelState)
	{
		printf("pthread_setcancelstate failed: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	struct Shared * shared = (struct Shared*) args;
	pthread_mutex_lock(&shared->mutexMain);
	const unsigned int SERVER_PORT = shared->serverPort;
	const unsigned int TOTAL_SERVER = shared->totalServer;
	const int serverSocketFD = shared->serverSocketFD;
	pthread_mutex_unlock(&shared->mutexMain);

	for (;;)
	{
		char recvBuf[256];
		memset(recvBuf, 0, sizeof(recvBuf));
		struct sockaddr_in clntAddr;
		int clntAddrLen = sizeof(clntAddr);
		int recvSize = recvfrom(serverSocketFD, recvBuf, sizeof(recvBuf), 0,
				(struct sockaddr*)&clntAddr, (socklen_t*)&clntAddrLen); //(pthread cancellation point)
		if (recvSize == -1)
		{
			perror("RecvrThread recvfrom");
			exit(EXIT_FAILURE);
		}

		//sleep for random nanosec to simulate network delay (pthread cancellation point)
		CustomUtility::RandNanoSleep(1000000, 999999999);

		if (PRINT_RECV_MESSAGES)
			printf("Recv: %d, %s, %u: %.*s\n", recvSize, inet_ntoa(clntAddr.sin_addr), ntohs(clntAddr.sin_port), recvSize, recvBuf);

		std::vector<unsigned long long> recvNums = CustomUtility::StrToNumVec<unsigned long long>(recvBuf); //[srcPort,srcIsRequest,srcVecClock,vecClockAtRequest]
		//const std::string extracted = ToStr<unsigned long long>(recvNums);
		//printf("Extracted: %s\n:", extracted.c_str());

		pthread_mutex_lock(&shared->mutexMain);
		{
			const unsigned int srcPort = (unsigned int)recvNums[0];
			const unsigned int srcIsRequest = (unsigned int)recvNums[1];
			const std::vector<unsigned long long> srcVecClock(recvNums.begin()+2, recvNums.begin()+2+TOTAL_SERVER);
			//printf("=> %s\n", ToStr<unsigned long long>(srcVecClock).c_str());
			const std::vector<unsigned long long> curVecClock = shared->vecClock.OnRecv(srcVecClock);
			if (srcIsRequest == 2) //non-request-nor-reply messages
			{

			}
			else //request or reply messages (srcIsRequest==0||srcIsRequest==1)
			{
				const std::vector<unsigned long long> vecClockAtRequest(recvNums.begin()+2+TOTAL_SERVER, recvNums.end());
				if (shared->RA.OnRecv(srcPort-START_PORT, srcIsRequest, vecClockAtRequest, curVecClock)) //now enters CS
				{
					printf("\n>>>Enters Distributed CS @ %s (%s)\n", CustomUtility::ToStr<unsigned long long>(curVecClock).c_str(), CustomUtility::ToStr<unsigned long long>(vecClockAtRequest).c_str());
				}
				else //stil collecting replys toward (totalServer-1)
				{

				}
			}
		}
		pthread_mutex_unlock(&shared->mutexMain);
	}
}
int main()
{
	srand(time(0));
	unsigned int SERVER_PORT;
	printf("Enter Server Port (start from 9000): ");
	scanf("%u", &SERVER_PORT);
	unsigned int TOTAL_SERVER;
	printf("Enter Number of Server (>= 2): ");
	scanf("%u", &TOTAL_SERVER);
	if (TOTAL_SERVER < 2)
	{
		printf("Number of Server: %u (must be at least 2)\n", TOTAL_SERVER);
		exit(EXIT_FAILURE);
	}
	int serverSocketFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        printf("Socket: %d\n", serverSocketFD);
        if (serverSocketFD == -1)
        {
                printf("Socket");
                exit(EXIT_FAILURE);
        }

        struct sockaddr_in servAddr;
        memset(&servAddr, 0, sizeof(servAddr));
        servAddr.sin_family = AF_INET;
        servAddr.sin_port = htons(SERVER_PORT);
        servAddr.sin_addr.s_addr = INADDR_ANY;//means any interface ip address of this machine

        int bindStatus = bind(serverSocketFD, (struct sockaddr*) &servAddr, sizeof(servAddr));
        printf("Bind: %d\n", bindStatus);
        if (bindStatus == -1)
        {
                printf("Bind");
                exit(EXIT_FAILURE);
        }

	struct Shared * shared = new Shared(SERVER_PORT, TOTAL_SERVER, serverSocketFD);

	pthread_t threadId[2];
	pthread_attr_t threadAttr[2];
	for (int i = 0; i < 2; ++i)
	{
		pthread_attr_init(&threadAttr[i]);
		int pthreadCreateErr = pthread_create(&threadId[i], &threadAttr[i], (i==0?RecvrThreadFunc:SenderThreadFunc), (void*)shared);
		if (pthreadCreateErr)
		{
			printf("pthread_create fail: %s\n", strerror(errno));
			return pthreadCreateErr;
		}
	}

	for (;;)
	{
		char buf[256];
		memset(buf, 0, sizeof(buf));
		printf("Enter \"enter/exit\" to enter/exit Distributed CS, \"quit\" to quit, or \"send/recv\" to turn on/off display send/recv messages: ");
		scanf("%s", buf);
		if (strncmp(buf, "quit", strlen("quit")) == 0) break;
		if (strncmp(buf, "send", strlen("send")) == 0) { PRINT_SEND_MESSAGES = !PRINT_SEND_MESSAGES; continue; }
		if (strncmp(buf, "recv", strlen("recv")) == 0) { PRINT_RECV_MESSAGES = !PRINT_RECV_MESSAGES; continue; }
		if (strncmp(buf, "enter", strlen("enter")) == 0)
		{
			pthread_mutex_lock(&shared->mutexMain);
			auto curVecClock = shared->vecClock.Increment();
			if (shared->RA.Enter(curVecClock))
				printf("\nRequest to Enter Distributed CS @ %s\n", CustomUtility::ToStr<unsigned long long>(curVecClock).c_str());
			else
				printf("\nAlready Entered Distributed CS or Wait until DeferReplyQueue is empty @ %s\n", CustomUtility::ToStr<unsigned long long>(curVecClock).c_str());
			pthread_mutex_unlock(&shared->mutexMain);
			continue;
		}
		if (strncmp(buf, "exit", strlen("exit")) == 0)
		{
			pthread_mutex_lock(&shared->mutexMain);
			auto curVecClock = shared->vecClock.Increment();
			if (shared->RA.Exit(curVecClock))
				printf("\n>>>Exits Distributed CS @ %s\n", CustomUtility::ToStr<unsigned long long>(curVecClock).c_str());
			else
				printf("\nAlready in RELEASED or WANTED state @ %s\n", CustomUtility::ToStr<unsigned long long>(curVecClock).c_str());
			pthread_mutex_unlock(&shared->mutexMain);
			continue;
		}
	}

	for (int i = 0; i < 2; ++i)
	{
		int pthreadCancelErr = pthread_cancel(threadId[i]);
		printf("Pthread Cancel: %d\n", pthreadCancelErr);
		if (pthreadCancelErr)
		{
			printf("pthread_cancel failed: %s\n", strerror(errno));
			return pthreadCancelErr;
		}
		void * joinStatus;
		int pthreadJoinErr = pthread_join(threadId[i], &joinStatus);
		printf("Pthread Join: %d\n", pthreadJoinErr);
		if (pthreadJoinErr)
		{
			printf("pthread_join failed: %s\n", strerror(errno));
			return pthreadJoinErr;
		}
	}
	delete shared;
	
	int closeStatus = close(serverSocketFD);
	printf("Close: %d\n", closeStatus);
	if (closeStatus == -1)
	{
		perror("Close");
		exit(EXIT_FAILURE);
	}

	return 0;
}
/*
g++ UDPMulticastServer.cpp -o UDPMulticastServer -lpthread -lrt
valgrind --leak-check=full --show-leak-kinds=all ./UDPMulticastServer
 */
