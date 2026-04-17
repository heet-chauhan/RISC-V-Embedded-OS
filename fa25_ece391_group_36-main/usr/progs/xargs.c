

#include "syscall.h"
#include "string.h"
#include "shell.h"

void main(int argc, char* argv[]){
    //make sure a command is given
    if (argc < 2){
        _write(CONSOLEOUT, "Error. No command provided.\n", 28);
        return;
    }

    //get the stdin arguments that are seperated by white spaces and store in a buf
    char stdin_buf[1024];
    int stdin_idx = 0;
    char buf[1024];
    long bytes_read;

    bytes_read = _read(0, buf, 1023);
    while (bytes_read > 0){
        for (int i = 0; i < bytes_read; i++){
            char c = buf[i];
            //convert newlines and tabs to spaces
            if (c == '\n' || c == '\t'){
                c = ' ';
            }
            if (stdin_idx < 1023){
                stdin_buf[stdin_idx++] = c;
            } else {
                _write(CONSOLEOUT, "Error. Too many arguments.\n", 27);
                return;
            }
        }
        bytes_read = _read(0, buf, 1023);
    }
    stdin_buf[stdin_idx] = '\0';

    //build an argv array from stdin args. format: command + args
    char cmd[1024];
    int cmd_idx = 0;

    strncpy(cmd, argv[1], sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    cmd_idx = strlen(argv[1]);

    for (int i=2; i<argc; i++){
        cmd[cmd_idx++] = ' ';
        strncpy(cmd + cmd_idx, argv[i], sizeof(cmd) - cmd_idx - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        cmd_idx += strlen(cmd + cmd_idx);
    }

    //add in a temrinator for the array
    if (stdin_idx > 0){
        cmd[cmd_idx++] = ' ';
        strncpy(cmd + cmd_idx, stdin_buf, sizeof(cmd) - cmd_idx - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        cmd_idx += strlen(cmd + cmd_idx);
    }
    cmd[cmd_idx] = '\0';

    char* exec_argv[64];
    int exec_argc = 0;
    char* start = cmd;
    
    for (int i = 0; i <= cmd_idx; i++){
        if (cmd[i] == ' ' || cmd[i] == '\0'){
            if (start < &cmd[i]){
                cmd[i] = '\0';
                exec_argv[exec_argc++] = start;
                start = &cmd[i + 1];
            }
        }
    }
    exec_argv[exec_argc] = 0;

    //check for absolute path and add c/ if needed
    char pathBuf[256];
    const char* exec_path;
    
    if (exec_argv[0][0] != 'c' && exec_argv[0][0] != '/'){
        //add c/
        strncpy(pathBuf, "c/", 2);
        strncpy(pathBuf + 2, exec_argv[0], 253);
        pathBuf[255] = '\0';
        exec_path = pathBuf;
    } else {
        exec_path = exec_argv[0];
    }

    //open the command file
    int fd = _open(-1, exec_path);
    if (fd < 0){
        _write(CONSOLEOUT, "Error. Command not found\n", 25);
        return;
    }

    //execute the command
    _exec(fd, exec_argc, exec_argv);
}

