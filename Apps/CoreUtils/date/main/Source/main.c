#include <stdio.h>
#include <time.h>

int main(int argc, char* argv[]) {
    time_t now = time(NULL);
    char buffer[64];
    struct tm local_time;
    localtime_r(&now, &local_time);
    strftime(buffer, sizeof(buffer), "%a %b %e %H:%M:%S %Z %Y", &local_time);
    printf("%s\n", buffer);
    return 0;
}
