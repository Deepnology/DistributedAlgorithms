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
#include "../VectorClock/VectorClock.hpp"
#include "CausalOrder.hpp"
#define DEST_IP_ADDRESS "127.0.0.1"
#define START_PORT 9000
#define ADVANCED_SEND_COUNT 8
static bool PRINT_SEND_MESSAGES = false;
static bool PRINT_RECV_MESSAGES = false;
static bool PRINT_CAUSAL_MESSAGES = true;
static long Rand(long lower, long upper)
{
	return rand() % (upper-lower+1) + lower;
}
static unsigned long long GetTimeIn(int precision)//0: sec, 1: millisec, 2: microsec, 3: nanosec
{
	struct timespec spec;
	clock_gettime(CLOCK_REALTIME, &spec);
	switch (precision)
	{
		case 0: return (unsigned long long)spec.tv_sec + (unsigned long long)spec.tv_nsec / 1000000000;//sec
		case 1: return (unsigned long long)spec.tv_sec * 1000 + (unsigned long long)spec.tv_nsec / 1000000;//millisec
		case 2: return (unsigned long long)spec.tv_sec * 1000000 + (unsigned long long)spec.tv_nsec / 1000;//microsec
		case 3: return (unsigned long long)spec.tv_sec * 1000000000 + (unsigned long long)spec.tv_nsec;//nanosec
		default: return (unsigned long long)spec.tv_sec * 1000000000 + (unsigned long long)spec.tv_nsec;//nanosec
	}
}
static void RandNanoSleep(long lower, long upper) //in nanoseconds
{
	const struct timespec sleeptime({0, Rand(lower, upper)});
	nanosleep(&sleeptime, NULL);
}
template<class T>
static void Shuffle(std::vector<T> & v)
{
	for (auto i = 0; i < v.size(); ++i)
	{
		auto j = rand() % (i+1);
		std::swap(v[i], v[j]);
	}
}
template<class T>
static std::string ToStr(const std::vector<T> & v)
{
        std::string s;
        for (auto i = 0; i < v.size(); ++i)
        {
                s += std::to_string(v[i]);
                if (i != v.size()-1)
                        s += " ";
        }
        return s;
}
static std::vector<unsigned long long> StrToUllVec(const char * str)
{
	std::vector<unsigned long long> v;
	const char * p = str;
	for (;;) //extract nums separated by spaces
	{
		char * end;
		unsigned long long i = strtoull(p, &end, 10);
		if (p == end) break;
		v.push_back(i);
		p = end;
	}
	return v;
}
struct Shared
{
	pthread_mutex_t mutexMain; //between main thread, recvr thread, and shuffle thread
	const unsigned int serverPort;
	const unsigned int totalServer;
	const int serverSocketFD;
	DistributedAlgorithms::VectorClock vecClock;
	DistributedAlgorithms::CausalOrder causal;
	pthread_cond_t condMain;
	bool allServerReady;

	pthread_mutex_t mutexShuffleSend; //between shuffle thread (producer) and sender thread (consumer)
	pthread_cond_t condShuffleSendFull;
	pthread_cond_t condShuffleSendEmpty;
	std::vector<std::pair<unsigned int, std::vector<unsigned long long>>> shuffleSendChannels; //<destPort, [time,vecClock,causal]>

	Shared(unsigned int _serverPort, unsigned int _totalServer, int _serverSocketFD):serverPort(_serverPort), totalServer(_totalServer), serverSocketFD(_serverSocketFD), vecClock(totalServer, serverPort-START_PORT), causal(totalServer, serverPort-START_PORT), allServerReady(false)
	, shuffleSendChannels()
	{
		int mutexMainInit = pthread_mutex_init(&mutexMain, NULL);
		if (mutexMainInit)
		{
			printf("pthread_mutex_init failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
		int condMainInit = pthread_cond_init(&condMain, NULL);
		if (condMainInit)
		{
			printf("pthread_cond_init failed: %s\n", strerror(errno));
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

	if (!shared->allServerReady)
		pthread_cond_wait(&shared->condMain, &shared->mutexMain); //wait until enter "ready"
	pthread_mutex_unlock(&shared->mutexMain);

	std::vector<unsigned int> ports;
	for (auto i = 0; i < TOTAL_SERVER; ++i)
		if (i+START_PORT != SERVER_PORT) ports.push_back(i+START_PORT);

	for (;;)
	{
		std::vector<std::pair<unsigned int, std::vector<unsigned long long>>> advSendPortTimePairs;//<destPort,[time,vecClock,causal]>
		for (auto i = 0; i < ADVANCED_SEND_COUNT; ++i)
		{
			pthread_mutex_lock(&shared->mutexMain);
			std::vector<unsigned long long> causal = shared->causal.OnSend(); //causal vector must be same for all channels !!
			//otherwise, need to maintain separate causal vectors for each channel
			pthread_mutex_unlock(&shared->mutexMain);
			for (auto j = 0; j < ports.size(); ++j)
			{
				RandNanoSleep(1000001, 1000009);
				pthread_mutex_lock(&shared->mutexMain);
				std::vector<unsigned long long> vc = shared->vecClock.OnSend(); //vector clock can be different for all channels
				pthread_mutex_unlock(&shared->mutexMain);

				std::vector<unsigned long long> timeVecClockCausal;
				timeVecClockCausal.push_back(GetTimeIn(1));
				timeVecClockCausal.insert(timeVecClockCausal.end(), vc.begin(), vc.end());
				timeVecClockCausal.insert(timeVecClockCausal.end(), causal.begin(), causal.end());
				advSendPortTimePairs.push_back({ports[j], timeVecClockCausal});
			}
		}
		Shuffle<std::pair<unsigned int, std::vector<unsigned long long>>>(advSendPortTimePairs);
		//shuffle sent out messages to simulate network delay and random order at receivers

		while (!advSendPortTimePairs.empty())
		{
			//producer: producing is faster than consuming
			pthread_mutex_lock(&shared->mutexShuffleSend);
			while (shared->shuffleSendChannels.size() == (ADVANCED_SEND_COUNT*(TOTAL_SERVER-1)))
				pthread_cond_wait(&shared->condShuffleSendFull, &shared->mutexShuffleSend);
			shared->shuffleSendChannels.push_back(advSendPortTimePairs.back());
			advSendPortTimePairs.pop_back();
			Shuffle<std::pair<unsigned int, std::vector<unsigned long long>>>(shared->shuffleSendChannels);
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
		RandNanoSleep(1000000, 999999999);

		char buf[256];
		memset(buf, 0, sizeof(buf));
		std::string srcTimeStr;
		for (auto & i : srcTime) srcTimeStr += std::to_string(i) + " ";
		sprintf(buf, "%u %s", SERVER_PORT, srcTimeStr.c_str()); //multicast message "curPort curTime curVecClock curCausalVec"

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
		RandNanoSleep(1000000, 999999999);

		if (PRINT_RECV_MESSAGES)
			printf("Recv: %d, %s, %u: %.*s\n", recvSize, inet_ntoa(clntAddr.sin_addr), ntohs(clntAddr.sin_port), recvSize, recvBuf);

		std::vector<unsigned long long> recvNums = StrToUllVec(recvBuf);
		pthread_mutex_lock(&shared->mutexMain);
		unsigned int srcPort = (unsigned int)recvNums[0];
		unsigned long long srcTime = recvNums[1];
		std::vector<unsigned long long> srcVecClock(recvNums.begin()+2, recvNums.begin()+2+TOTAL_SERVER);
		std::vector<unsigned long long> srcCausalVec(recvNums.begin()+2+TOTAL_SERVER, recvNums.end());
		std::vector<std::string> deliver;
		auto p = shared->causal.OnRecv(srcPort-START_PORT, srcCausalVec, std::string(recvBuf, recvSize), deliver);

		for (auto i = 0; i < deliver.size(); ++i)
		{
			recvNums.clear();
			recvNums = StrToUllVec(deliver[i].c_str());
			srcPort = recvNums[0];
			srcTime = recvNums[1];
			srcVecClock.clear(); srcVecClock.insert(srcVecClock.begin(), recvNums.begin()+2, recvNums.begin()+2+TOTAL_SERVER);
			srcCausalVec.clear(); srcCausalVec.insert(srcCausalVec.begin(), recvNums.begin()+2+TOTAL_SERVER, recvNums.end());
			std::vector<unsigned long long> curVecClock = shared->vecClock.OnRecv(srcVecClock);

			if (PRINT_CAUSAL_MESSAGES)
				printf("Causal: %s (%s %s)\n", deliver[i].c_str(), ToStr<unsigned long long>(curVecClock).c_str(), ToStr<unsigned long long>(p.second).c_str());
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

	char buf[256];

	for (;;)
	{
		memset(buf, 0, sizeof(buf));
		printf("Enter \"ready\" when all %d servers have started: ", TOTAL_SERVER);
		scanf("%s", buf);
		if (strncmp(buf, "ready", strlen("ready")) == 0)
		{
			pthread_mutex_lock(&shared->mutexMain);
			shared->allServerReady = true;
			pthread_cond_signal(&shared->condMain);
			pthread_mutex_unlock(&shared->mutexMain);
			break;
		}
	}

	for (;;)
	{
		memset(buf, 0, sizeof(buf));
		printf("Enter \"exit\" to exit, or \"send/recv/causal\" to turn on/off display send/recv/causal messages: ");
		scanf("%s", buf);
		if (strncmp(buf, "exit", strlen("exit")) == 0) break;
		if (strncmp(buf, "send", strlen("send")) == 0) { PRINT_SEND_MESSAGES = !PRINT_SEND_MESSAGES; continue; }
		if (strncmp(buf, "recv", strlen("recv")) == 0) { PRINT_RECV_MESSAGES = !PRINT_RECV_MESSAGES; continue; }
		if (strncmp(buf, "causal", strlen("causal")) == 0) { PRINT_CAUSAL_MESSAGES = !PRINT_CAUSAL_MESSAGES; continue; }
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
