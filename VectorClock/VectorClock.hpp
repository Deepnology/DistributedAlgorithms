#ifndef VECTOR_CLOCK_HPP
#define VECTOR_CLOCK_HPP
#include <vector>
#include <algorithm>
#include <string>
namespace DistributedAlgorithms
{
	class VectorClock
	{
		std::vector<unsigned long long> clocks;
		const unsigned int hostIdx;
	public:
		VectorClock(unsigned int _totalServer, unsigned int _hostIdx): clocks(_totalServer, 0), hostIdx(_hostIdx)
		{

		}
		unsigned long long & operator[](unsigned int idx)
		{
			if (idx >= clocks.size())
			{
				printf("VectorClock index out of bound\n");
				exit(EXIT_FAILURE);
			}
			return clocks.at(idx);
		}
		const unsigned long long & operator[](unsigned int idx) const
		{
			if (idx >= clocks.size())
			{
				printf("VectorClock index out of bound\n");
				exit(EXIT_FAILURE);
			}
			return clocks.at(idx);
		}
		std::vector<unsigned long long> OnRecv(const std::vector<unsigned long long> & src)
		{
			for (auto i = 0; i < clocks.size(); ++i)
			{
				if (i == hostIdx) ++clocks[i];
				else clocks[i] = std::max(clocks[i], src[i]);
			}
			return clocks;
		}
		std::vector<unsigned long long> OnSend()
		{
			return Increment();
		}
		std::vector<unsigned long long> Increment()
		{
			++clocks[hostIdx];
			return clocks;
		}
		std::vector<unsigned long long> Get() const
		{
			return clocks;
		}
		std::string ToStr() const
		{
			std::string s;
			for (auto i = 0; i < clocks.size(); ++i)
			{
				s += std::to_string(clocks[i]);
				if (i != clocks.size()-1)
					s += " ";
			}
			return s;
		}
	};
}
#endif
