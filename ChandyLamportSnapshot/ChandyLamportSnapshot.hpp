#ifndef CHANDY_LAMPORT_SNAPSHOT_HPP
#define CHANDY_LAMPORT_SNAPSHOT_HPP
#include <vector>
#include <string>
#include <algorithm>
namespace DistributedAlgorithms
{
	class ChandyLamportSnapshot
	{
	public:
		struct Snapshot
		{
        		std::string state; //state of host process
        		std::vector<std::string> channelStates; //channel states from other processes to host process (except host)
        		explicit Snapshot(unsigned int _totalServer): state(), channelStates(_totalServer, std::string())
        		{

        		}
		};
	private:
		bool initiateSnapshot;
		const unsigned int totalServer;
		const unsigned int hostIdx;
        	std::vector<Snapshot> snapshots; //snapshot initiated by host process or other processes
        	std::vector<unsigned int> recvMarkerCount; //received marker count initiated by host process or other processes
        	std::vector<std::vector<std::vector<std::string>>> channelMsgs; //recorded channel msgs since snapshot from other processes
        	std::queue<std::vector<unsigned long long>> multicastMarkerQue; //<destIdx,markerSrcPort,markerSrcTime,markerSrcVecClock> tuple to multicast when receiving first markers
	public:
		ChandyLamportSnapshot(unsigned int _totalServer, unsigned int _hostIdx): initiateSnapshot(false), totalServer(_totalServer), hostIdx(_hostIdx)
	        , snapshots(_totalServer, Snapshot(_totalServer)), recvMarkerCount(_totalServer, 0), channelMsgs(_totalServer, std::vector<std::vector<std::string>>(_totalServer, std::vector<std::string>())), multicastMarkerQue()
		{

		}

		bool IsInitiated() const
		{ return initiateSnapshot; }

		bool Initiate(const std::string & curState)
		{
			if (initiateSnapshot) return false;
			initiateSnapshot = true;
			snapshots[hostIdx].state = curState; //record state of host process
                	for (auto & s : snapshots[hostIdx].channelStates)
                        	s.clear(); //clear all channel states from other processes to host process to start recording
			for (auto & v : channelMsgs[hostIdx])
                		v.clear(); //clear all channel msgs from other processes to host process to start recording
			return true;
		}

		bool OnSendMarkerMsg(std::vector<unsigned long long> & markerMsgToSend)
		{
			if (multicastMarkerQue.empty())
				return false;
			markerMsgToSend.clear();
			markerMsgToSend = multicastMarkerQue.front();
			multicastMarkerQue.pop();
			return true;
		}

		std::pair<bool,Snapshot> OnRecvMarkerMsg(unsigned int srcIdx, unsigned int markerSrcIdx, const std::vector<unsigned long long> & markerSrcInfo, const std::string & curState)
		{
			if (markerSrcIdx == hostIdx) //snapshot-initiate process: record msgs from src process to host process since snapshot began
			{
				for (auto i = 0; i < channelMsgs[markerSrcIdx][srcIdx].size(); ++i)
                	        	snapshots[markerSrcIdx].channelStates[srcIdx] += channelMsgs[markerSrcIdx][srcIdx][i] + ",";
			}
			else //non-snapshot-initiate process
			{
                        	if (recvMarkerCount[markerSrcIdx] == 0) //begin snapshot for process markerSrcPort
                               	{
                                	snapshots[markerSrcIdx].state = curState; //record own state
                                       	for (auto i = 0; i < snapshots[markerSrcIdx].channelStates.size(); ++i)
                        	        	snapshots[markerSrcIdx].channelStates[i].clear(); //clear all channel states
					for (auto i = 0; i < channelMsgs[markerSrcIdx].size(); ++i)
                                        	channelMsgs[markerSrcIdx][i].clear(); //clear all channel msgs
					
                                        //push markers to queue to multicast to all other processes except self
                                        std::vector<unsigned int> markerDestIdx = MarkerMsgDestIdx();
                                        for (auto i : markerDestIdx)
                                        {
                                        	std::vector<unsigned long long> v({(unsigned long long)i});
                                                v.insert(v.end(), markerSrcInfo.begin(), markerSrcInfo.end());
                                                multicastMarkerQue.push(v);
                                       	 }
                            	}
                              	else //record messages from srcIdx to host process since snapshot began
                              	{
					for (auto i = 0; i < channelMsgs[markerSrcIdx][srcIdx].size(); ++i)
                	        		snapshots[markerSrcIdx].channelStates[srcIdx] += channelMsgs[markerSrcIdx][srcIdx][i] + ",";
                              	}
			}

			bool terminated = false;
			if (++recvMarkerCount[markerSrcIdx] == totalServer-1) //terminate snapshot for markerSrcPort process
                       	{
				terminated = true;
                        	recvMarkerCount[markerSrcIdx] = 0; //reset
                                if (markerSrcIdx == hostIdx) //snapshot-initiate process
                                {
                                	initiateSnapshot = false; //reset
                                }
                                else //non-snapshot-initiate process
                                {

                                }
                      	}
			return { terminated, snapshots[markerSrcIdx] };
		}

		void OnRecvNonMarkerMsg(unsigned int srcIdx, const std::string & msg) //msg: "srcPort srcTime srcVecClock curTime curVecClock"
		{
			for (auto i = 0; i < recvMarkerCount.size(); ++i)
                        {
				if ((i == hostIdx && initiateSnapshot) //record message for host process that began snapshot
						|| recvMarkerCount[i] != 0) //record message for other processes that began snapshot
				{
                                	channelMsgs[i][srcIdx].push_back(msg);
				}
                        }
		}

		std::vector<unsigned int> MarkerMsgDestIdx() const
		{
			std::vector<unsigned int> idx;
                        for (auto i = 0; i < totalServer; ++i)
                        	if (i != hostIdx) idx.push_back(i);
			return idx;
		}
	};
}
#endif
