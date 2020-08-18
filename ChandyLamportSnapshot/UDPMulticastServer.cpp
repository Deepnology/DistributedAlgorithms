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
#define DEST_IP_ADDRESS "127.0.0.1"
#define START_PORT 9000
static bool PRINT_SEND_MESSAGES = false;
static bool PRINT_RECV_MESSAGES = false;
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
static void Shuffle(std::vector<unsigned int> & v)
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
struct Snapshot
{
	std::string state; //state of host process
	std::vector<std::string> channelStates; //channel states from other processes to host process (except host)
	Snapshot(unsigned int _totalServer): state(), channelStates(_totalServer, std::string())
	{

	}
};
static void PrintSnapshot(const struct Snapshot & s, unsigned int markerSrcPort, unsigned int serverPort)
{
	printf("\nSnapshot from %u:", markerSrcPort);
	if (markerSrcPort == serverPort) printf(" (Initiator)\n");
	else printf("\n");
	printf("state: %s\n", s.state.c_str());
	for (auto i = 0; i < s.channelStates.size(); ++i)
		if (i+START_PORT != serverPort) //all src channels except self
			printf("channel_%u: %s\n", i+START_PORT, s.channelStates[i].c_str());
}
struct Shared
{
	pthread_mutex_t mutexMain; //between main thread, recvr thread and sender thread
	const unsigned int serverPort;
	const unsigned int totalServer;
	const int serverSocketFD;
	DistributedAlgorithms::VectorClock vecClock;

	bool initiateSnapshot;
	std::vector<Snapshot> snapshots; //snapshot initiated by host process or other processes
	std::vector<unsigned int> recvMarkerCount; //received marker count initiated by host process or other processes
	std::vector<std::vector<std::string>> channelMsgs; //recorded channel msgs since snapshot from other processes
	std::queue<std::vector<unsigned long long>> multicastMarkerQue; //<destPort,markerSrcPort,markerSrcTime,markerSrcVecClock> tuple to multicast when receiving first markers

	Shared(unsigned int _serverPort, unsigned int _totalServer, int _serverSocketFD): serverPort(_serverPort), totalServer(_totalServer), serverSocketFD(_serverSocketFD), vecClock(_totalServer, _serverPort-START_PORT)
	, initiateSnapshot(false)
	, snapshots(_totalServer, Snapshot(_totalServer)), recvMarkerCount(_totalServer, 0), channelMsgs(_totalServer, std::vector<std::string>())
	{
		int mutexMainInit = pthread_mutex_init(&mutexMain, NULL);
		if (mutexMainInit)
		{
			printf("pthread_mutex_init failed: %s\n", strerror(errno));
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
		Shuffle(ports);
		for (auto i = 0; i < ports.size(); ++i) //randomly multicast messages
		{
			//sleep for random nanosec to simulate network delay (pthread cancellation point)
			RandNanoSleep(1000000, 999999999);

			char buf[256];
			memset(buf, 0, sizeof(buf));
			unsigned destPort;
			
			pthread_mutex_lock(&shared->mutexMain);
			if (!shared->multicastMarkerQue.empty()) //multicast marker messages with first priority
			{
				destPort = (unsigned int)shared->multicastMarkerQue.front()[0];
				const unsigned int markerSrcPort = (unsigned int)shared->multicastMarkerQue.front()[1];
				const std::string markerSrcTimeVecClk = ToStr<unsigned long long>(std::vector<unsigned long long>(shared->multicastMarkerQue.front().begin()+2, shared->multicastMarkerQue.front().end()));
				shared->multicastMarkerQue.pop();
				shared->vecClock.OnSend();
				std::string vc = shared->vecClock.ToStr();
				sprintf(buf, "%u %llu %s %u %s", SERVER_PORT, GetTimeIn(1), vc.c_str(), markerSrcPort, markerSrcTimeVecClk.c_str()); //multicast marker message "curPort curTime curVecClock markerSrcPort markerSrcTime markerSrcVecClock"

				--i; //repeat
			}
			else
			{
				destPort = ports[i];
				shared->vecClock.OnSend();
				std::string vc = shared->vecClock.ToStr();
				sprintf(buf, "%u %llu %s", SERVER_PORT, GetTimeIn(1), vc.c_str()); //multicast message "curPort curTime curVecClock"
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
		RandNanoSleep(1000000, 999999999);

		if (PRINT_RECV_MESSAGES)
			printf("Recv: %d, %s, %u: %.*s\n", recvSize, inet_ntoa(clntAddr.sin_addr), ntohs(clntAddr.sin_port), recvSize, recvBuf);

		std::vector<unsigned long long> recvNums;
		const char * p = recvBuf;
		for (;;) //extract nums separated by spaces
		{
			char * end;
			unsigned long long i = strtoull(p, &end, 10);
			if (p == end) break;
			recvNums.push_back(i);
			p = end;
		}
		const std::string extracted = ToStr<unsigned long long>(recvNums);
		//printf("Extracted: %s\n:", extracted.c_str());

		pthread_mutex_lock(&shared->mutexMain);
		{
			const unsigned int srcPort = (unsigned int)recvNums[0];
			const unsigned long long srcTime = recvNums[1];
			const std::vector<unsigned long long> srcVecClock(recvNums.begin()+2, recvNums.begin()+2+TOTAL_SERVER);
			//printf("=> %s\n", ToStr<unsigned long long>(srcVecClock).c_str());
			shared->vecClock.OnRecv(srcVecClock);

			if (recvNums.size() == (TOTAL_SERVER+2)*2) //marker message for snapshot from process markerSrcPort
			{
				//printf("Recv Marker: %s\n", extracted.c_str());
				const unsigned int markerSrcPort = (unsigned int)recvNums[TOTAL_SERVER+2];
				const unsigned long long markerSrcTime = recvNums[TOTAL_SERVER+3];
				const std::vector<unsigned long long> markerSrcVecClock(recvNums.begin()+TOTAL_SERVER+4, recvNums.end());

				if (markerSrcPort == shared->serverPort) //snapshot-initiate process: record msgs from srcPort to host process since snapshot began
				{
					for (auto s : shared->channelMsgs[markerSrcPort-START_PORT])
					{
						unsigned int msgSrcPort = (unsigned int)strtoul(s.c_str(), NULL, 10);
						shared->snapshots[markerSrcPort-START_PORT].channelStates[msgSrcPort-START_PORT] += s + ",";
					}
				}
				else //non-snapshot-initiate process
				{
					if (shared->recvMarkerCount[markerSrcPort-START_PORT] == 0) //begin snapshot for process markerSrcPort
					{
						shared->snapshots[markerSrcPort-START_PORT].state = std::to_string(GetTimeIn(1)) + " " + shared->vecClock.ToStr(); //record own state: "curTime curVecClock"
						for (auto i = 0; i < shared->snapshots[markerSrcPort-START_PORT].channelStates.size(); ++i)
							shared->snapshots[markerSrcPort-START_PORT].channelStates[i].clear(); //clear all channel states
						shared->channelMsgs[markerSrcPort-START_PORT].clear(); //clear all channel msgs
						//push markers to queue to multicast to all other processes except self
						std::vector<unsigned int> markerPorts;
						for (auto i = 0; i < shared->totalServer; ++i)
							if (i+START_PORT != shared->serverPort)
								markerPorts.push_back(i+START_PORT);
						Shuffle(markerPorts);
						for (auto & i : markerPorts)
						{
							std::vector<unsigned long long> v({i, markerSrcPort, markerSrcTime});
							v.insert(v.end(), markerSrcVecClock.begin(), markerSrcVecClock.end());
							shared->multicastMarkerQue.push(v);
						}
					}
					else //record messages from srcPort to host process since snapshot began
					{
						for (auto s : shared->channelMsgs[markerSrcPort-START_PORT])
						{
							unsigned int msgSrcPort = (unsigned int)strtoul(s.c_str(), NULL, 10);
							shared->snapshots[markerSrcPort-START_PORT].channelStates[msgSrcPort-START_PORT] += s + ",";
						}
					}
				}

				if (++shared->recvMarkerCount[markerSrcPort-START_PORT] == shared->totalServer-1) //terminate snapshot for markerSrcPort process
				{
					shared->recvMarkerCount[markerSrcPort-START_PORT] = 0; //reset
					if (markerSrcPort == shared->serverPort) //snapshot-initiate process
					{
						shared->initiateSnapshot = false; //reset
					}
					else //non-snapshot-initiate process
					{

					}
					PrintSnapshot(shared->snapshots[markerSrcPort-START_PORT], markerSrcPort, shared->serverPort);
				}
			}
			else //non-marker message
			{
				//record message "srcPort srcTime srcVecClock curTime curVecClock"
				std::string curMsg = std::to_string(srcPort) + " " + std::to_string(srcTime) + " " + ToStr<unsigned long long>(srcVecClock) + " " + std::to_string(GetTimeIn(1)) + " " + shared->vecClock.ToStr();

				for (auto i = 0; i < shared->recvMarkerCount.size(); ++i)
				{
					if (shared->recvMarkerCount[i] != 0) //record message for processes that began snapshot
					{
						shared->channelMsgs[i].push_back(curMsg);
					}
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
		printf("Enter \"shot\" to initiate snapshot, \"exit\" to exit, or \"send/recv\" to turn on/off display send/recv messages: ");
		scanf("%s", buf);
		if (strncmp(buf, "exit", strlen("exit")) == 0) break;
		if (strncmp(buf, "send", strlen("send")) == 0) { PRINT_SEND_MESSAGES = !PRINT_SEND_MESSAGES; continue; }
		if (strncmp(buf, "recv", strlen("recv")) == 0) { PRINT_RECV_MESSAGES = !PRINT_RECV_MESSAGES; continue; }
		if (strncmp(buf, "shot", strlen("shot")) != 0) continue;

		pthread_mutex_lock(&shared->mutexMain);
		if (shared->initiateSnapshot) //host process already began snapshot
		{
			printf("Host process already began snaphot !!\n");
			pthread_mutex_unlock(&shared->mutexMain);
			continue;
		}
		shared->initiateSnapshot = true;
		shared->snapshots[SERVER_PORT-START_PORT].state = std::to_string(GetTimeIn(1)) + " " + shared->vecClock.ToStr(); //record state of host process: "curTime curVecClock"
		for (auto & s : shared->snapshots[SERVER_PORT-START_PORT].channelStates)
			s.clear(); //clear all channel states from other processes to host process to start recording
		shared->channelMsgs[SERVER_PORT-START_PORT].clear(); //clear all channel msgs from other processes to host process to start recording
		pthread_mutex_unlock(&shared->mutexMain);

		std::vector<unsigned int> ports;
		for (unsigned int i = 0; i < TOTAL_SERVER; ++i)
			if (i+START_PORT != SERVER_PORT) ports.push_back(i+START_PORT);
		Shuffle(ports);

		for (auto i = 0; i < ports.size(); ++i)
		{
			//sleep for random nanosec to simulate network delay
			RandNanoSleep(1000000, 999999999);
			memset(buf, 0, sizeof(buf));

			pthread_mutex_lock(&shared->mutexMain);
			shared->vecClock.OnSend();
			std::string vc = shared->vecClock.ToStr();
			sprintf(buf, "%u %llu %s %u %llu %s", SERVER_PORT, GetTimeIn(1), vc.c_str(), SERVER_PORT, GetTimeIn(1), vc.c_str());
			//multicast marker message "curPort curTime curVecClock curPort curTime curVecClock" to other processes
			pthread_mutex_unlock(&shared->mutexMain);

			struct sockaddr_in destAddr;
			memset(&destAddr, 0, sizeof(destAddr));
			destAddr.sin_family = AF_INET;
			destAddr.sin_port = htons(ports[i]);
			destAddr.sin_addr.s_addr = inet_addr(DEST_IP_ADDRESS);
			int sentSize = sendto(serverSocketFD, buf, sizeof(buf), 0,
					(struct sockaddr*)&destAddr, sizeof(struct sockaddr));
			if (sentSize == -1)
			{
				printf("MainThread Send: %s\n", strerror(errno));
				exit(EXIT_FAILURE);
			}
			if (PRINT_SEND_MESSAGES)
				printf("Sent: %d, %s, %u: %.*s\n", sentSize, inet_ntoa(destAddr.sin_addr), ntohs(destAddr.sin_port), sentSize, buf);
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
