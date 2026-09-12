#pragma once
#include <cstdint>
#include <string>
#include <set>
#include <utility>
namespace localcalendar {
// A month can be represented as shared daily times only when every selected
// day has exactly the same set of times. Duplicate events do not change shape.
inline bool mixedTimes(const std::set<std::pair<int,int>> &slots){
 std::set<int> dates,minutes;
 for(const auto &slot:slots){dates.insert(slot.first);minutes.insert(slot.second);}
 return slots.size()!=dates.size()*minutes.size();
}
inline int days(int y,int m){if(m<1||m>12)return 0;static const int n[]={31,28,31,30,31,30,31,31,30,31,30,31};return n[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0));}
inline bool month(const std::string &s,int &y,int &m){
 if(s.size()!=7||s[4]!='-')return false;
 for(unsigned i=0;i<s.size();i++)if(i!=4&&(s[i]<'0'||s[i]>'9'))return false;
 y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';m=(s[5]-'0')*10+s[6]-'0';return y>=2024&&y<=2099&&m>=1&&m<=12;
}
inline bool time(const std::string &s,int &h,int &m){
 if(s.size()!=5||s[2]!=':')return false;
 for(unsigned i=0;i<s.size();i++)if(i!=2&&(s[i]<'0'||s[i]>'9'))return false;
 h=(s[0]-'0')*10+s[1]-'0';m=(s[3]-'0')*10+s[4]-'0';return h<24&&m<60;
}
// Gregorian civil date to Unix seconds, fixed Asia/Taipei UTC+8.
inline int64_t epoch(int y,int m,int d,int h,int min){
 y-=m<=2;const int era=y/400;const unsigned yo=y-era*400;
 const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
 const unsigned doe=yo*365+yo/4-yo/100+doy;
 return (int64_t(era)*146097+doe-719468)*86400+h*3600+min*60-28800;
}
}
