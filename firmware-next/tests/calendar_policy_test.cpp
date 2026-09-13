#include "../main/calendar_policy.h"
#include <cassert>
#include <iostream>
int main(){
 int y,m,h,n;using namespace localcalendar;
 assert(month("2026-07",y,m)&&y==2026&&m==7);
 for(auto s:{"2026-00","2026-13","2026-1","2026-a1","2023-12","2100-01","2026-07junk"})assert(!month(s,y,m));
 assert(days(2024,2)==29&&days(2026,2)==28&&days(2026,9)==30);
 assert(time("00:00",h,n)&&h==0&&n==0);assert(time("23:59",h,n)&&h==23&&n==59);
 for(auto s:{"24:00","07:60","7:00","07:00x","-1:00"})assert(!time(s,h,n));
 assert(epoch(2026,7,5,7,0)==1783206000LL);
 assert(epoch(2026,7,5,7,10)-epoch(2026,7,5,7,0)==600);
 assert(epoch(2024,3,1,0,0)-epoch(2024,2,28,0,0)==172800);
 const std::set<int64_t> workDates={epoch(2026,8,31,0,0),epoch(2026,9,1,0,0),epoch(2026,9,2,0,0),epoch(2026,9,5,0,0)};
 assert(shouldNotify(epoch(2026,8,31,0,0),workDates,true));
 assert(!shouldNotify(epoch(2026,9,1,0,0),workDates,true));
 assert(!shouldNotify(epoch(2026,9,2,0,0),workDates,true));
 assert(shouldNotify(epoch(2026,9,5,0,0),workDates,true));
 assert(shouldNotify(epoch(2026,9,1,0,0),workDates,false));
 assert(!mixedTimes({}));
 assert(!mixedTimes({{13,420},{14,420}}));
 assert(!mixedTimes({{13,420},{13,480},{14,420},{14,480}}));
 assert(mixedTimes({{13,420},{14,480}}));
 assert(mixedTimes({{13,420},{13,480},{14,420}}));
 std::cout<<"Calendar date, consecutive workday, time and Taiwan epoch checks passed\n";
}
