#ifndef _RICART_AGRAWALA_HPP
#define _RICART_AGRAWALA_HPP
#include <vector>
#include <string>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include "../MulticastCausalOrder/CausalOrder.hpp"
namespace DistributedAlgorithms
{
	class RicartAgrawala
	{
	public:
		enum STATE
		{
			RELEASED = 0,
			WANTED = 1,
			HELD = 2,
		};
	private:
		STATE state;
		const unsigned int totalServer;
		const unsigned int hostIdx;

		std::vector<unsigned long long> vecClockAtRequest;//host's vecClock of cur request to enter distributed CS
		std::vector<unsigned long long> vecClockAtRecvAll;//host's vecClock when received all replys
		std::vector<unsigned long long> vecClockAtExit;//host's vecClock when exit distributed CS
		std::queue<std::vector<unsigned long long>> requestQue;//<destIdx,hostIdx,isRequest,hostVecClockAtRequest>, for host's request
		std::unordered_set<unsigned int> recvdReply;//received replys until (totalServer-1)
		std::queue<std::vector<unsigned long long>> replyQue;//<destIdx,hostIdx,isRequest,srcVecClockAtRequest>, for other hosts' requests
		std::queue<std::vector<unsigned long long>> deferReplyQue;//<destIdx,hostIdx,isRequest,srcVecClockAtRequest> for other hosts' requests
	public:
		RicartAgrawala(unsigned int _totalServer, unsigned int _hostIdx): state(RELEASED), totalServer(_totalServer), hostIdx(_hostIdx)
		{

		}

		STATE GetState() const
		{ return state; }

		bool Enter(const std::vector<unsigned long long> & curVecClock)
		{
			if (state != RELEASED) return false; //already in HELD or WANTED state
			if (!deferReplyQue.empty()) return false; //wait until deferReplyQue is empty
			state = WANTED;
			vecClockAtRequest = curVecClock; //record curVecClock
			for (auto idx = 0; idx < totalServer; ++idx)
			{
				if (idx == hostIdx) continue;
				std::vector<unsigned long long> request({(unsigned long long)idx, (unsigned long long)hostIdx, 1});
				request.insert(request.end(), curVecClock.begin(), curVecClock.end());
				requestQue.push(request); //let sender thread to send requests
			}
			recvdReply.clear(); //reset
			return true;
		}

		bool Exit(const std::vector<unsigned long long> & curVecClock)
		{
			if (state != HELD) return false; //already in RELEASED or WANTED state
			state = RELEASED; //such that sender thread can send replys in deferReplyQue
			vecClockAtExit = curVecClock;
			return true;
		}

		bool OnRecv(unsigned int srcIdx, bool isRequest, const std::vector<unsigned long long> & srcVecClockAtRequest, const std::vector<unsigned long long> & curVecClock)
		{
			if (isRequest)
			{
				if (state == HELD || 
				(state == WANTED && 
				 (DistributedAlgorithms::Causal::Less<unsigned long long>(vecClockAtRequest, srcVecClockAtRequest) ||
				  (!DistributedAlgorithms::Causal::Less<unsigned long long>(srcVecClockAtRequest, vecClockAtRequest) && hostIdx < srcIdx)) //for concurrent requests, use process's index to break the tie
				  )) //add request to deferReplyQue to reply when exit
				{
					std::vector<unsigned long long> v({srcIdx, hostIdx, 0});
					v.insert(v.end(), srcVecClockAtRequest.begin(), srcVecClockAtRequest.end());
					deferReplyQue.push(v);
				}
				else //add request to replyQue to reply immediately
				{
					std::vector<unsigned long long> v({srcIdx, hostIdx, 0});
					v.insert(v.end(), srcVecClockAtRequest.begin(), srcVecClockAtRequest.end());
					replyQue.push(v);
				}
			}
			else //collect (totalServer-1) replys from other hosts for this host's request
			{
				recvdReply.insert(srcIdx);
				if (recvdReply.size() == totalServer-1) //now enters distributed CS: change state to HELD
				{
					state = HELD;
					vecClockAtRecvAll = curVecClock;
					return true;
				}
			}
			return false;
		}

		bool OnSend(std::vector<unsigned long long> & toSend)
		{
			if (!requestQue.empty()) //multicast requests to other hosts (state==WANTED)
			{
				toSend = requestQue.front();
				requestQue.pop();
				return true;
			}
			else if (state == RELEASED && !deferReplyQue.empty()) //send replys for requests from other hosts when exit
			{
				toSend = deferReplyQue.front();
				deferReplyQue.pop();
				return true;
			}
			else if (!replyQue.empty()) //send replys for requests from other hosts immediately (state==WANTED||state==RELEASED)
			{
				toSend = replyQue.front();
				replyQue.pop();
				return true;
			}
			return false;
		}

		
	};
}
#endif
