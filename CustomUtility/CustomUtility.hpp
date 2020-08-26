#include <vector>
#include <string>
#include <algorithm>
#include <time.h>
#include <stdlib.h>
namespace CustomUtility
{
long Rand(long lower, long upper)
{
        return rand() % (upper-lower+1) + lower;
}
unsigned long long GetTimeIn(int precision)//0: sec, 1: millisec, 2: microsec, 3: nanosec
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
void RandNanoSleep(long lower, long upper) //in nanoseconds
{
        const struct timespec sleeptime({0, Rand(lower, upper)});
        nanosleep(&sleeptime, NULL);
}
template<class T>
void Shuffle(std::vector<T> & v)
{
        for (auto i = 0; i < v.size(); ++i)
        {
                auto j = rand() % (i+1);
                std::swap(v[i], v[j]);
        }
}
template<class T>
std::string ToStr(const std::vector<T> & v)
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
template<class T>
std::vector<T> StrToNumVec(const char * str, int base = 10)
{
        std::vector<T> v;
        const char * p = str;
        for (;;) //extract nums separated by spaces
        {
                char * end;
                T i;
                if (typeid(T) == typeid(long))
                        i = strtol(p, &end, base);
                else if (typeid(T) == typeid(long long))
                        i = strtoll(p, &end, base);
                else if (typeid(T) == typeid(unsigned long))
                        i = strtoul(p, &end, base);
                else if (typeid(T) == typeid(unsigned long long))
                        i = strtoull(p, &end, base);
		else
			break;
                if (p == end) break;
                v.push_back(i);
                p = end;
        }
        return v;
}



}
