#ifndef CAUSAL_ORDER_HPP
#define CAUSAL_ORDER_HPP
#include <queue>
#include <vector>
#include <string>
#include <algorithm>
#include <tuple>
namespace DistributedAlgorithms
{
	namespace Causal
	{
		template<class T>
		bool Equal(const std::vector<T> & lhs, const std::vector<T> & rhs)
		{
			if (lhs.size() != rhs.size()) return false;
			for (auto i = 0; i < lhs.size(); ++i)
				if (lhs[i] != rhs[i]) return false;
			return true;
		}
		template<class T>
		bool LessEqual(const std::vector<T> & lhs, const std::vector<T> & rhs)
		{
			if (lhs.size() != rhs.size()) return false;
			for (auto i = 0; i < lhs.size(); ++i)
				if (!(lhs[i] <= rhs[i])) return false;
			return true;
		}
		template<class T>
		bool Less(const std::vector<T> & lhs, const std::vector<T> & rhs) //e.g., causally related
		{
			return LessEqual(lhs, rhs) && !Equal(lhs, rhs);
		}
		template<class T>
		bool Concurrent(const std::vector<T> & lhs, const std::vector<T> & rhs) //e.g., NOT causally related
		{
			return !LessEqual(lhs, rhs) && !LessEqual(rhs, lhs);

			/*
			equivalent to 
			!Equal(lhs, rhs) && !Less(lhs, rhs) && !Less(rhs, lhs)

			= !Equal(lhs, rhs) && !(LessEqual(lhs, rhs) && !Equal(lhs, rhs)) && !(LessEqual(rhs, lhs) && !Equal(rhs, lhs))
			= !Equal(lhs, rhs) && (!LessEqual(lhs, rhs) || Equal(lhs, rhs)) && (!LessEqual(rhs, lhs) || Equal(rhs, lhs))
			NOTE: "|| (!Equal(lhs,rhs)&&Equal(lhs,rhs))" = "|| false"
			= (!Equal(lhs, rhs) && !LessEqual(lhs, rhs)) && (!Equal(lhs, rhs) && !LessEqual(rhs, lhs))
			= !LessEqual(lhs, rhs) && !LessEqual(rhs, lhs)
			*/
		}
	}
	class CausalOrder
	{
		typedef std::tuple<unsigned int, std::vector<unsigned long long>, std::string> SeqVecTuple;
		struct Greater
		{
			bool operator()(const SeqVecTuple & a, const SeqVecTuple & b) const
			{
				return std::get<1>(a)[std::get<0>(a)] > std::get<1>(b)[std::get<0>(b)];
			}
		};
		const unsigned int totalServer;
		const unsigned int hostIdx;
		std::vector<unsigned long long> channelSeq;
		std::vector<std::priority_queue<SeqVecTuple, std::vector<SeqVecTuple>, Greater> > channelBuf;
	public:
		CausalOrder(unsigned int _totalServer, unsigned int _hostIdx) : totalServer(_totalServer), hostIdx(_hostIdx), channelSeq(_totalServer, 0), channelBuf(_totalServer, std::priority_queue<SeqVecTuple, std::vector<SeqVecTuple>, Greater>())
		{

		}
		std::vector<unsigned long long> Get() const
		{
			return channelSeq;
		}
		std::string ToStr() const
		{
			std::string s;
                        for (auto i = 0; i < channelSeq.size(); ++i)
                        {
                                s += std::to_string(channelSeq[i]);
                                if (i != channelSeq.size()-1)
                                        s += " ";
                        }
                        return s;
		}
		std::vector<unsigned long long> OnSend()
		{
			++channelSeq[hostIdx];
			return channelSeq;
		}
		std::pair<bool,std::vector<unsigned long long>> OnRecv(unsigned int srcChannelIdx, const std::vector<unsigned long long> & srcChannelSeq, const std::string & srcChannelMsg, std::vector<std::string> & deliver)
		{
			bool delivered = false;
			if (Deliver(srcChannelIdx, srcChannelSeq))
			{
				delivered = true;
				deliver.push_back(srcChannelMsg);
				std::vector<std::string> nxtDeliver;
				do
				{
					nxtDeliver.clear();
					for (auto i = 0; i < channelBuf.size(); ++i)
					{
						while (!channelBuf[i].empty() && 
							Deliver(std::get<0>(channelBuf[i].top()), std::get<1>(channelBuf[i].top())))
						{
							nxtDeliver.push_back(std::get<2>(channelBuf[i].top()));
							channelBuf[i].pop();
						}
					}
					deliver.insert(deliver.end(), nxtDeliver.begin(), nxtDeliver.end());
				}while(!nxtDeliver.empty());
			}
			else
			{
				channelBuf[srcChannelIdx].push({srcChannelIdx, srcChannelSeq, srcChannelMsg});
			}
			return {delivered, channelSeq};
		}
	private:
		bool Deliver(unsigned int srcChannelIdx, const std::vector<unsigned long long> & srcChannelSeq)
		{
			if (channelSeq[srcChannelIdx]+1 == srcChannelSeq[srcChannelIdx]) //first condition
			{
				for (auto idx = 0; idx < channelSeq.size(); ++idx)
					if (idx != srcChannelIdx)
						if (!(channelSeq[idx] >= srcChannelSeq[idx])) //second condition
							return false;
				channelSeq[srcChannelIdx] = srcChannelSeq[srcChannelIdx]; //increment
				return true;
			}
			return false;
		}
	};
}
#endif
