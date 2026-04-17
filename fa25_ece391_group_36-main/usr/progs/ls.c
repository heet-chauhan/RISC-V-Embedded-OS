

#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char* argv[]){
    const char* path;
    char pathBuf[256];

    //check if a path is passed
    if (argc < 2){
        path = "c/";
    }
    //make sure the first arg is absolute
    else if (argv[1][0] != 'c' || argv[1][1] != '/'){
        //append c/ to the first arg
        strncpy(pathBuf, "c/", 2);
        strncpy(pathBuf+2, argv[1], 253);
        pathBuf[255] = '\0';
        path = pathBuf;
    }
    else{
        path = argv[1];
    }

    //open that directory
    int fd = _open(-1, path);
    if (fd < 0){
        _write(CONSOLEOUT, "Error. Couldn't open that directory.\n", 37);
        return;
    }

    char buf[1024];
    long bytes_read;

    //read the dentries
    bytes_read = _read(fd, buf, 1023);
    while (bytes_read > 0){
        buf[bytes_read] = '\0';
        _write(CONSOLEOUT, buf, bytes_read);
        _write(CONSOLEOUT, "\n", 1);
        bytes_read = _read(fd, buf, 1023);
    }
    

    //close that path
    _close(fd);
}
