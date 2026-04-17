

#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char* argv[]){
    //if no arg is passed, read from stdin
    int fd;
    if (argc < 2){
        fd = STDIN;
    }
    else{
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
        
        //open that file
        fd = _open(-1, path);
        if (fd < 0){
            _write(CONSOLEOUT, "Error. Couldn't open that file.\n", 32);
            return;
        }
    }

    //counters
    long lines = 0;
    long words = 0;
    long bytes = 0;
    int in_word = 0;

    char buf[1024];
    long bytes_read;

    bytes_read = _read(fd, buf, 1024);

    while (bytes_read > 0){
        bytes += bytes_read;

        for (int i=0; i<bytes_read; i++){
            char c = buf[i];
            if (c == '\n') {lines++;} //count the lines
            if (c == ' ' || c == '\t' || c == '\n'){ //track when we exit a word
                in_word = 0;
            }
            else if (!in_word){
                //weve just entered a word
                words++;
                in_word = 1;
            }
        }
        bytes_read = _read(fd, buf, 1024);
    }

    char output[64];
    int len = snprintf(output, sizeof(output), "%ld\t%ld\t%ld\n", lines, words, bytes);
    _write(CONSOLEOUT, output, len);
    _close(fd);
}
