#include <time.h>
#include <sys/time.h>
static void InternalGetTime(long *seconds, long *milliseconds);
static void AddMillisecondsToNow(long long milliseconds, long *sec, long *ms);
