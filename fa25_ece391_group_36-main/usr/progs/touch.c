

#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char* argv[]){
    //make sure a fie name is given
    if (argc < 2){
        _write(STDOUT, "Error. No file name provided\n", 30);
        return;
    }

    //create each file specified
    for (int i=1; i<argc; i++){
        //make sure an absolute path is passed 
        if (argv[i][0] != '/'){
            _write(STDOUT, "Error. Please provide an absolute path.\n", 41);
            continue;
        }
        
        int result = _fscreate(argv[i]);
        if (result < 0){
            _write(STDOUT, "Error. Couldn't create file: ", 29);
            _write(STDOUT, argv[i], strlen(argv[i]));
            _write(STDOUT, "\n", 1);
        }
    }
}
