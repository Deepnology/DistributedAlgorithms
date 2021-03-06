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
#include "VectorClock.hpp"
#define DEST_IP_ADDRESS "127.0.0.1"
#define START_PORT 9000
#define ADVANCED_SEND_COUNT 8
static bool PRINT_SEND_MESSAGES = false;
static bool PRINT_RECV_MESSAGES = false;
struct Shared
{
	pthread_mutex_t mutexMain; //between main thread, recvr thread, and shuffle thread
	const unsigned int serverPort;
	const unsigned int totalServer;
	const int serverSocketFD;
	DistributedAlgorithms::VectorClock vecClock;

	pthread_mutex_t mutexShuffleSend; //between shuffle thread (producer) and sender thread (consumer)
	pthread_cond_t condShuffleSendFull;
	pthread_cond_t condShuffleSendEmpty;
	std::vector<std::pair<unsigned int, std::vector<unsigned long long>>> shuffleSendChannels; //<destPort, time>

	Shared(unsigned int _serverPort, unsigned int _totalServer, int _serverSocketFD):serverPort(_serverPort), totalServer(_totalServer), serverSocketFD(_serverSocketFD), vecClock(totalServer, serverPort-START_PORT)
	, shuffleSendChannels()
	{
		int mutexMainInit = pthread_mutex_init(&mutexMain, NULL);
		if (mutexMainInit)
		{
			printf("pthread_mutex_init failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		int mutexShuffleSendInit = pthread_mutex_init(&mutexShuffleSend, NULL);
		if (mutexShuffleSendInit)
		{
			printf("pthread_mutex_init failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		int condShuffleSendFullInit = pthread_cond_init(&condShuffleSendFull, NULL);
		if (condShuffleSendFullInit)
		{
			printf("pthread_cond_init failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		int condShuffleSendEmptyInit = pthread_cond_init(&condShuffleSendEmpty, NULL);
		if (condShuffleSendEmptyInit)
		{
			printf("pthread_cond_init failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
};
static void * ShuffleThreadFunc(void * args)
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

	std::vector<unsigned int> ports;
	for (auto i = 0; i < TOTAL_SERVER; ++i)
		if (i+START_PORT != SERVER_PORT) ports.push_back(i+START_PORT);

	for (;;)
	{
		std::vector<std::pair<unsigned int, std::vector<unsigned long long>>> advSendPortTimePairs;
		for (auto i = 0; i < ADVANCED_SEND_COUNT; ++i)
		{
			for (auto j = 0; j < ports.size(); ++j)
			{
				CustomUtility::RandNanoSleep(1000001, 1000009);
				pthread_mutex_lock(&shared->mutexMain);
				std::vector<unsigned long long> vc = shared->vecClock.OnSend();
				pthread_mutex_unlock(&shared->mutexMain);
				vc.insert(vc.begin(), CustomUtility::GetTimeIn(1));
				advSendPortTimePairs.push_back({ports[j], vc});
			}
		}
		CustomUtility::Shuffle<std::pair<unsigned int, std::vector<unsigned long long>>>(advSendPortTimePairs);
		//shuffle sent out messages to simulate network delay and random order at receivers

		while (!advSendPortTimePairs.empty())
		{
			//producer: producing is faster than consuming
			pthread_mutex_lock(&shared->mutexShuffleSend);
			while (shared->shuffleSendChannels.size() == (ADVANCED_SEND_COUNT*(TOTAL_SERVER-1)))
				pthread_cond_wait(&shared->condShuffleSendFull, &shared->mutexShuffleSend);
			shared->shuffleSendChannels.push_back(advSendPortTimePairs.back());
			advSendPortTimePairs.pop_back();
			CustomUtility::Shuffle<std::pair<unsigned int, std::vector<unsigned long long>>>(shared->shuffleSendChannels);
			pthread_cond_signal(&shared->condShuffleSendEmpty);
			pthread_mutex_unlock(&shared->mutexShuffleSend);
		}
	}
}
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
	pthread_mutex_unlock(&shared->mutexMain);
	
	for (;;)
	{
		unsigned int destPort = 0;
		std::vector<unsigned long long> srcTime;

		//consumer: consuming is slower than producing with random delay
		pthread_mutex_lock(&shared->mutexShuffleSend);
		while (shared->shuffleSendChannels.empty())
			pthread_cond_wait(&shared->condShuffleSendEmpty, &shared->mutexShuffleSend);
		destPort = shared->shuffleSendChannels.back().first;
		srcTime = shared->shuffleSendChannels.back().second;
		shared->shuffleSendChannels.pop_back();
		pthread_cond_signal(&shared->condShuffleSendFull);
		pthread_mutex_unlock(&shared->mutexShuffleSend);
		
		//sleep for random nanosec to simulate network delay (pthread cancellation point)
		CustomUtility::RandNanoSleep(1000000, 999999999);

		char buf[256];
		memset(buf, 0, sizeof(buf));
		std::string srcTimeStr;
		for (auto & i : srcTime) srcTimeStr += std::to_string(i) + " ";
		sprintf(buf, "%u %s", SERVER_PORT, srcTimeStr.c_str()); //multicast message "curPort curTime"

		struct sockaddr_in destAddr;
		memset(&destAddr, 0, sizeof(destAddr));
		destAddr.sin_family = AF_INET;
		destAddr.sin_port = htons(destPort);
		destAddr.sin_addr.s_addr = inet_addr(DEST_IP_ADDRESS);
		int sentSize = sendto(serverSocketFD, buf, sizeof(buf), 0,
				(struct sockaddr*)&destAddr, sizeof(struct sockaddr));
		if (sentSize == -1)
		{
			printf("SenderThread sendto failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (PRINT_SEND_MESSAGES)
			printf("Sent: %d, %s, %u: %.*s\n", sentSize, inet_ntoa(destAddr.sin_addr), ntohs(destAddr.sin_port), sentSize, buf);
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

	std::vector<unsigned int> ports;
	for (auto i = 0; i < TOTAL_SERVER; ++i)
		if (i+START_PORT != SERVER_PORT) ports.push_back(i+START_PORT);

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

		std::vector<unsigned long long> recvNums = CustomUtility::StrToNumVec<unsigned long long>(recvBuf);

		pthread_mutex_lock(&shared->mutexMain);
		unsigned int srcPort = (unsigned int)recvNums[0];
		unsigned long long srcTime = recvNums[1];
		std::vector<unsigned long long> srcVecClock(recvNums.begin()+2, recvNums.end());
		shared->vecClock.OnRecv(srcVecClock);
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

	pthread_t threadId[3];
	pthread_attr_t threadAttr[3];
	for (int i = 0; i < 3; ++i)
	{
		pthread_attr_init(&threadAttr[i]);
		int pthreadCreateErr = pthread_create(&threadId[i], &threadAttr[i], (i==0?ShuffleThreadFunc:i==1?SenderThreadFunc:RecvrThreadFunc), (void*)shared);
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
		printf("Enter \"exit\" to exit, or \"send/recv\" to turn on/off display send/recv messages: ");
		scanf("%s", buf);
		if (strncmp(buf, "exit", strlen("exit")) == 0) break;
		if (strncmp(buf, "send", strlen("send")) == 0) { PRINT_SEND_MESSAGES = !PRINT_SEND_MESSAGES; continue; }
		if (strncmp(buf, "recv", strlen("recv")) == 0) { PRINT_RECV_MESSAGES = !PRINT_RECV_MESSAGES; continue; }
	}

	for (int i = 0; i < 3; ++i)
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
