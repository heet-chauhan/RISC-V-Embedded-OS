

#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char* argv[]){
    //make sure a file name is given
    if (argc < 2){
        _write(CONSOLEOUT, "Error. No file name provided.\n", 30);
        return;
    }

    //delete each file specified
    for (int i=1; i<argc; i++){
       //make sure an absolute path is passed 
        if (argv[i][0] != 'c' && argv[i][0] != '/'){
            _write(CONSOLEOUT, "Error. Please provide the correct path.\n", 40);
            continue;
        }
       
        int result = _fsdelete(argv[i]);
        if (result < 0){
            _write(CONSOLEOUT, "Error. Couldn't delete file: ", 29);
            _write(CONSOLEOUT, argv[i], strlen(argv[i]));
            _write(CONSOLEOUT, "\n", 1);
        }
    }
}    

