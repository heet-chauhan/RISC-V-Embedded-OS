
#include "string.h"
#include "shell.h"
#include "syscall.h"


void main(int argc, char* argv[]){
        for (int i=1; i<argc; i++){
            _write(CONSOLEOUT, argv[i], strlen(argv[i]));
            _write(CONSOLEOUT, " ", 1);
        }
        _write(CONSOLEOUT, "\n", 1);
    }
