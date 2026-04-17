#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char* argv[]){
    //open the rtc device
    int fd = _open(-1, "dev/rtc");
    if (fd < 0){
        _write(CONSOLEOUT, "Error. Couldn't reach the RTC\n", 30);
        return;
    }

    //get the time in nanoseconds
    long nanoseconds;
    long bytes_read = _read(fd, &nanoseconds, sizeof(nanoseconds));
    //check for errors
    if (bytes_read < 0){
        _write(CONSOLEOUT, "Error. Couldn't read from RTC.\n", 31);
        _close(fd);
        return;
    }

    _close(fd);

    //get the seconds
    long total_seconds = nanoseconds / 1000000000;
    int sec = total_seconds % 60;

    //get the minutes
    total_seconds /= 60;
    int min = total_seconds % 60;

    //get the hours
    total_seconds /= 60;
    int hour = total_seconds % 24;

    //get the days
    long days = total_seconds / 24;

    //get the year (account for leap years)
    int year = 1970;
    while (1){
        int days_in_year;
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)){
            days_in_year = 366;
        }
        else{
            days_in_year = 365;
        }

        if (days < days_in_year){
            break;
        }
        days -= days_in_year;
        year++;
    }

    //get the month
    int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    char* month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    //adjust february if its a leap year
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)){
        days_in_month[1] = 29;
    }

    int month = 0;
    while (month < 12 && days >= days_in_month[month]){
        days -= days_in_month[month];
        month++;
    }

    //get the date 
    int day = days + 1;
    
    //print it out
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%02d %s %04d %02d:%02d:%02d\n", 
                       day, month_names[month], year, hour, min, sec);
    
    _write(CONSOLEOUT, buf, len);
}
