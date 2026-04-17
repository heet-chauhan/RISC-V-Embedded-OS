

#include "syscall.h"
#include "uio.h"
#include "shell.h"

void main(int argc, char* argv[]){
    //make sure a filename is passed
    if (argc < 2){
        _write(CONSOLEOUT, "Error. No file name provided.\n", 30);
        return;
    }

    const char* path;
    char pathBuf[256];
    
    //check for absolute path and add c/ if needed
    if (argv[1][0] != 'c' && argv[1][0] != '/'){
        //add c/
        strncpy(pathBuf, "c/", 2);
        strncpy(pathBuf + 2, argv[1], 253);
        pathBuf[255] = '\0';
        path = pathBuf;
    }
    else{
        path = argv[1];
    }

    //open the file
    int fd = _open(-1, path);
    if (fd < 0){
        _write(CONSOLEOUT, "Error. Couldn't open file\n", 26);
        return;
    }
    

    //define a buffer
    char buf[1024];
    long bytes_read;

    //read from the file and print
    bytes_read = _read(fd, buf, 1023);
    while (bytes_read > 0){
        _write(CONSOLEOUT, buf, bytes_read);
        bytes_read = _read(fd, buf, 1023);
    }

    //close the file
    _close(fd);
        
    }
